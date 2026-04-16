/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 * Full implementation on top of the provided boilerplate.
 */
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include "monitor_ioctl.h"

#define DEVICE_NAME        "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ------------------------------------------------------------------ */
/*  TODO 1: Linked-list node                                           */
/* ------------------------------------------------------------------ */

struct monitored_entry {
    pid_t          pid;
    char           container_id[MONITOR_NAME_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            soft_warned;      /* 1 once soft-limit warning emitted */
    struct list_head list;
};

/* ------------------------------------------------------------------ */
/*  TODO 2: Global list + mutex                                        */
/*                                                                     */
/*  We use a mutex (not spinlock) because the timer callback runs in   */
/*  softirq context — actually, kernel timers run in softirq so we     */
/*  should use spin_lock_bh.  We use a spinlock with _bh suffix.       */
/*  The ioctl path is process context so spin_lock_bh is safe there    */
/*  too (it just disables BH).                                         */
/* ------------------------------------------------------------------ */

static LIST_HEAD(monitored_list);
static DEFINE_SPINLOCK(monitored_lock);

/* ------------------------------------------------------------------ */
/*  Provided: internal device / timer state                            */
/* ------------------------------------------------------------------ */

static struct timer_list monitor_timer;
static dev_t              dev_num;
static struct cdev        c_dev;
static struct class      *cl;

/* ------------------------------------------------------------------ */
/*  Provided: RSS helper                                               */
/* ------------------------------------------------------------------ */

static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);
    return rss_pages * PAGE_SIZE;
}

/* ------------------------------------------------------------------ */
/*  Provided: limit event helpers                                      */
/* ------------------------------------------------------------------ */

static void log_soft_limit_event(const char *container_id,
                                  pid_t pid,
                                  unsigned long limit_bytes,
                                  long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

static void kill_process(const char *container_id,
                          pid_t pid,
                          unsigned long limit_bytes,
                          long rss_bytes)
{
    struct task_struct *task;
    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();
    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ------------------------------------------------------------------ */
/*  TODO 3: Timer callback — periodic monitoring                       */
/* ------------------------------------------------------------------ */

static void timer_callback(struct timer_list *t)
{
    struct monitored_entry *entry, *tmp;
    LIST_HEAD(to_free);

    spin_lock_bh(&monitored_lock);

    list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
        long rss = get_rss_bytes(entry->pid);

        /* Process has exited — remove stale entry */
        if (rss < 0) {
            printk(KERN_INFO
                   "[container_monitor] Process exited: container=%s pid=%d — removing\n",
                   entry->container_id, entry->pid);
            list_move(&entry->list, &to_free);
            continue;
        }

        /* Hard limit check */
        if ((unsigned long)rss >= entry->hard_limit_bytes) {
            kill_process(entry->container_id, entry->pid,
                         entry->hard_limit_bytes, rss);
            list_move(&entry->list, &to_free);
            continue;
        }

        /* Soft limit check — warn once */
        if (!entry->soft_warned &&
            (unsigned long)rss >= entry->soft_limit_bytes) {
            log_soft_limit_event(entry->container_id, entry->pid,
                                 entry->soft_limit_bytes, rss);
            entry->soft_warned = 1;
        }
    }

    spin_unlock_bh(&monitored_lock);

    /* Free removed entries outside the lock */
    list_for_each_entry_safe(entry, tmp, &to_free, list) {
        list_del(&entry->list);
        kfree(entry);
    }

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ------------------------------------------------------------------ */
/*  ioctl handler                                                      */
/* ------------------------------------------------------------------ */

static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    /* Null-terminate container_id defensively */
    req.container_id[MONITOR_NAME_LEN - 1] = '\0';

    if (cmd == MONITOR_REGISTER) {
        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid,
               req.soft_limit_bytes, req.hard_limit_bytes);

        /* TODO 4: allocate and insert entry */
        if (req.soft_limit_bytes > req.hard_limit_bytes) {
            printk(KERN_WARNING
                   "[container_monitor] Invalid limits for container=%s (soft > hard)\n",
                   req.container_id);
            return -EINVAL;
        }

        struct monitored_entry *entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid               = req.pid;
        entry->soft_limit_bytes  = req.soft_limit_bytes;
        entry->hard_limit_bytes  = req.hard_limit_bytes;
        entry->soft_warned       = 0;
        strncpy(entry->container_id, req.container_id, MONITOR_NAME_LEN - 1);
        entry->container_id[MONITOR_NAME_LEN - 1] = '\0';
        INIT_LIST_HEAD(&entry->list);

        spin_lock_bh(&monitored_lock);
        list_add_tail(&entry->list, &monitored_list);
        spin_unlock_bh(&monitored_lock);

        return 0;
    }

    /* MONITOR_UNREGISTER */
    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    /* TODO 5: find and remove entry */
    {
        struct monitored_entry *entry, *tmp;
        struct monitored_entry *found = NULL;

        spin_lock_bh(&monitored_lock);
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            if (entry->pid == req.pid &&
                strncmp(entry->container_id, req.container_id,
                        MONITOR_NAME_LEN) == 0) {
                found = entry;
                list_del(&entry->list);
                break;
            }
        }
        spin_unlock_bh(&monitored_lock);

        if (found) {
            kfree(found);
            return 0;
        }
    }

    return -ENOENT;
}

/* ------------------------------------------------------------------ */
/*  File operations (provided)                                         */
/* ------------------------------------------------------------------ */

static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ------------------------------------------------------------------ */
/*  Module init (provided)                                             */
/* ------------------------------------------------------------------ */

static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif

    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }
    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }
    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n",
           DEVICE_NAME);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Module exit (provided + TODO 6)                                    */
/* ------------------------------------------------------------------ */

static void __exit monitor_exit(void)
{
    del_timer_sync(&monitor_timer);

    /* TODO 6: free all remaining list entries */
    {
        struct monitored_entry *entry, *tmp;
        spin_lock_bh(&monitored_lock);
        list_for_each_entry_safe(entry, tmp, &monitored_list, list) {
            list_del(&entry->list);
            kfree(entry);
        }
        spin_unlock_bh(&monitored_lock);
    }

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
