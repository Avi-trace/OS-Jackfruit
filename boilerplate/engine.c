/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 * Full implementation on top of the provided boilerplate.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "monitor_ioctl.h"

#define STACK_SIZE           (1024 * 1024)
#define CONTAINER_ID_LEN     32
#define CONTROL_PATH         "/tmp/mini_runtime.sock"
#define LOG_DIR              "logs"
#define CONTROL_MESSAGE_LEN  256
#define CHILD_COMMAND_LEN    256
#define LOG_CHUNK_SIZE       4096
#define LOG_BUFFER_CAPACITY  16
#define DEFAULT_SOFT_LIMIT   (40UL << 20)
#define DEFAULT_HARD_LIMIT   (64UL << 20)
#define MAX_CONTAINERS       64

/* ------------------------------------------------------------------ */
/*  Enumerations (unchanged from boilerplate)                          */
/* ------------------------------------------------------------------ */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

/* ------------------------------------------------------------------ */
/*  Data structures (unchanged from boilerplate)                       */
/* ------------------------------------------------------------------ */

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t     head;
    size_t     tail;
    size_t     count;
    int        shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;

typedef struct {
    int              server_fd;
    int              monitor_fd;
    int              should_stop;
    pthread_t        logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t  metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* ------------------------------------------------------------------ */
/*  Globals needed for signal handler                                  */
/* ------------------------------------------------------------------ */

static supervisor_ctx_t *g_ctx = NULL;

/* ------------------------------------------------------------------ */
/*  Usage / helpers (unchanged from boilerplate)                       */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                           const char *value,
                           unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                 int argc, char *argv[], int start_index)
{
    int i;
    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long  nice_value;
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/*  Bounded buffer                                                     */
/* ------------------------------------------------------------------ */

static int bounded_buffer_init(bounded_buffer_t *buf)
{
    int rc;
    memset(buf, 0, sizeof(*buf));
    rc = pthread_mutex_init(&buf->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buf->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buf->mutex); return rc; }
    rc = pthread_cond_init(&buf->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buf->not_empty);
        pthread_mutex_destroy(&buf->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buf)
{
    pthread_cond_destroy(&buf->not_full);
    pthread_cond_destroy(&buf->not_empty);
    pthread_mutex_destroy(&buf->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buf)
{
    pthread_mutex_lock(&buf->mutex);
    buf->shutting_down = 1;
    pthread_cond_broadcast(&buf->not_empty);
    pthread_cond_broadcast(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
}

/*
 * Push a log item into the buffer.
 * Blocks when full; returns 0 on success, -1 if shutting down.
 */
int bounded_buffer_push(bounded_buffer_t *buf, const log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == LOG_BUFFER_CAPACITY && !buf->shutting_down)
        pthread_cond_wait(&buf->not_full, &buf->mutex);

    if (buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return -1;
    }

    buf->items[buf->tail] = *item;
    buf->tail = (buf->tail + 1) % LOG_BUFFER_CAPACITY;
    buf->count++;

    pthread_cond_signal(&buf->not_empty);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/*
 * Pop a log item from the buffer.
 * Returns 0 on success, 1 if shutdown + empty (caller should exit).
 */
int bounded_buffer_pop(bounded_buffer_t *buf, log_item_t *item)
{
    pthread_mutex_lock(&buf->mutex);
    while (buf->count == 0 && !buf->shutting_down)
        pthread_cond_wait(&buf->not_empty, &buf->mutex);

    if (buf->count == 0 && buf->shutting_down) {
        pthread_mutex_unlock(&buf->mutex);
        return 1;   /* signal: done */
    }

    *item = buf->items[buf->head];
    buf->head = (buf->head + 1) % LOG_BUFFER_CAPACITY;
    buf->count--;

    pthread_cond_signal(&buf->not_full);
    pthread_mutex_unlock(&buf->mutex);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Logging consumer thread                                            */
/* ------------------------------------------------------------------ */

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (1) {
        int rc = bounded_buffer_pop(&ctx->log_buffer, &item);
        if (rc != 0)
            break;  /* shutdown + empty */

        /* Find log file path for this container */
        char log_path[PATH_MAX] = {0};
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *r = ctx->containers;
        while (r) {
            if (strncmp(r->id, item.container_id, CONTAINER_ID_LEN) == 0) {
                strncpy(log_path, r->log_path, PATH_MAX - 1);
                break;
            }
            r = r->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (log_path[0] == '\0')
            continue;

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0)
            continue;
        ssize_t written = 0;
        while (written < (ssize_t)item.length) {
            ssize_t n = write(fd, item.data + written, item.length - written);
            if (n <= 0) break;
            written += n;
        }
        close(fd);
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Per-container log reader thread                                    */
/*  Reads from the pipe connected to a container's stdout/stderr       */
/*  and pushes chunks into the bounded buffer.                         */
/* ------------------------------------------------------------------ */

typedef struct {
    int              read_fd;
    char             container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buf;
} reader_arg_t;

static void *container_reader_thread(void *arg)
{
    reader_arg_t *ra = (reader_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    while (1) {
        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, ra->container_id, CONTAINER_ID_LEN - 1);
        n = read(ra->read_fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0)
            break;
        item.length = (size_t)n;
        bounded_buffer_push(ra->buf, &item);
    }

    close(ra->read_fd);
    free(ra);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Container child entrypoint                                         */
/* ------------------------------------------------------------------ */

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to the logging pipe */
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    /* Set hostname to container ID */
    sethostname(cfg->id, strlen(cfg->id));

    /* Mount setup */
    /* Make the mount namespace private so our mounts don't leak */
    mount("none", "/", NULL, MS_REC | MS_PRIVATE, NULL);

    /* Bind-mount the rootfs onto itself so we can pivot/chroot */
    if (mount(cfg->rootfs, cfg->rootfs, "bind", MS_BIND | MS_REC, NULL) != 0) {
        perror("mount --bind rootfs");
        return 1;
    }

    /* chroot into rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir /");
        return 1;
    }

    /* Mount /proc inside container */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        /* Non-fatal: warn but continue */
        fprintf(stderr, "Warning: could not mount /proc: %s\n", strerror(errno));
    }

    /* Mount /dev/null etc if needed */
    mkdir("/dev", 0755);
    mount("tmpfs", "/dev", "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=755,size=65536k");

    /* Apply nice value */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* Execute the command */
    char *args[] = { "/bin/sh", "-c", cfg->command, NULL };
    execv("/bin/sh", args);
    perror("execv");
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Monitor registration helpers (unchanged from boilerplate)          */
/* ------------------------------------------------------------------ */

int register_with_monitor(int monitor_fd,
                           const char *container_id,
                           pid_t host_pid,
                           unsigned long soft_limit_bytes,
                           unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Container launch helper                                            */
/* ------------------------------------------------------------------ */

static container_record_t *launch_container(supervisor_ctx_t *ctx,
                                             const control_request_t *req)
{
    /* Check for duplicate ID */
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *existing = ctx->containers;
    while (existing) {
        if (strncmp(existing->id, req->container_id, CONTAINER_ID_LEN) == 0) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            fprintf(stderr, "Container '%s' already exists\n", req->container_id);
            return NULL;
        }
        existing = existing->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create log directory and file */
    mkdir(LOG_DIR, 0755);
    char log_path[PATH_MAX];
    snprintf(log_path, sizeof(log_path), "%s/%s.log", LOG_DIR, req->container_id);

    /* Create pipe for container output */
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) {
        perror("pipe2");
        return NULL;
    }
    /* pipefd[0] = read end (supervisor), pipefd[1] = write end (child) */

    /* Build child config */
    child_config_t *cfg = calloc(1, sizeof(child_config_t));
    if (!cfg) { close(pipefd[0]); close(pipefd[1]); return NULL; }
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,       PATH_MAX - 1);
    strncpy(cfg->command, req->command,      CHILD_COMMAND_LEN - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    /* Allocate stack for clone */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg);
        close(pipefd[0]);
        close(pipefd[1]);
        return NULL;
    }

    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, clone_flags, cfg);

    /* Close write end in parent immediately after clone */
    close(pipefd[1]);

    if (pid < 0) {
        perror("clone");
        free(stack);
        free(cfg);
        close(pipefd[0]);
        return NULL;
    }

    /* stack can be freed after clone; child has its own copy */
    free(stack);
    /* cfg will be freed by child process space; parent doesn't need it */

    /* Build container record */
    container_record_t *rec = calloc(1, sizeof(container_record_t));
    if (!rec) {
        close(pipefd[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return NULL;
    }
    strncpy(rec->id,       req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid          = pid;
    rec->started_at        = time(NULL);
    rec->state             = CONTAINER_RUNNING;
    rec->soft_limit_bytes  = req->soft_limit_bytes;
    rec->hard_limit_bytes  = req->hard_limit_bytes;
    rec->exit_code         = -1;
    rec->exit_signal       = -1;
    strncpy(rec->log_path, log_path, PATH_MAX - 1);

    /* Insert into linked list */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next       = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with kernel monitor */
    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, rec->id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);

    /* Start reader thread for this container's pipe */
    reader_arg_t *ra = malloc(sizeof(reader_arg_t));
    if (ra) {
        ra->read_fd = pipefd[0];
        strncpy(ra->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        ra->buf = &ctx->log_buffer;
        pthread_t tid;
        pthread_create(&tid, NULL, container_reader_thread, ra);
        pthread_detach(tid);
    } else {
        close(pipefd[0]);
    }

    fprintf(stderr, "[supervisor] Started container '%s' pid=%d\n",
            rec->id, rec->host_pid);
    return rec;
}

/* ------------------------------------------------------------------ */
/*  SIGCHLD handler                                                    */
/* ------------------------------------------------------------------ */

static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx) continue;

        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *r = g_ctx->containers;
        while (r) {
            if (r->host_pid == pid) {
                if (WIFEXITED(status)) {
                    r->exit_code  = WEXITSTATUS(status);
                    r->exit_signal = -1;
                    r->state      = CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    r->exit_signal = WTERMSIG(status);
                    r->exit_code   = -1;
                    /* Distinguish killed-by-monitor vs stopped */
                    if (r->state != CONTAINER_STOPPED)
                        r->state = CONTAINER_KILLED;
                }
                if (g_ctx->monitor_fd >= 0)
                    unregister_from_monitor(g_ctx->monitor_fd, r->id, pid);
                break;
            }
            r = r->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }

    errno = saved_errno;
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

/* ------------------------------------------------------------------ */
/*  Supervisor event loop helpers                                      */
/* ------------------------------------------------------------------ */

static void handle_ps(supervisor_ctx_t *ctx, int client_fd)
{
    char buf[4096] = {0};
    char line[512];

    snprintf(line, sizeof(line),
             "%-16s %-8s %-10s %-12s %-12s %-10s\n",
             "ID", "PID", "STATE", "SOFT(MB)", "HARD(MB)", "STARTED");
    strncat(buf, line, sizeof(buf) - strlen(buf) - 1);

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *r = ctx->containers;
    while (r) {
        char ts[32];
        struct tm *tm_info = localtime(&r->started_at);
        strftime(ts, sizeof(ts), "%H:%M:%S", tm_info);
        snprintf(line, sizeof(line),
                 "%-16s %-8d %-10s %-12lu %-12lu %-10s\n",
                 r->id, r->host_pid,
                 state_to_string(r->state),
                 r->soft_limit_bytes >> 20,
                 r->hard_limit_bytes >> 20,
                 ts);
        strncat(buf, line, sizeof(buf) - strlen(buf) - 1);
        r = r->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    control_response_t resp;
    resp.status = 0;
    strncpy(resp.message, buf, CONTROL_MESSAGE_LEN - 1);
    send(client_fd, &resp, sizeof(resp), 0);
}

static void handle_logs(supervisor_ctx_t *ctx, int client_fd,
                         const char *container_id)
{
    char log_path[PATH_MAX] = {0};
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *r = ctx->containers;
    while (r) {
        if (strncmp(r->id, container_id, CONTAINER_ID_LEN) == 0) {
            strncpy(log_path, r->log_path, PATH_MAX - 1);
            break;
        }
        r = r->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    control_response_t resp;
    if (log_path[0] == '\0') {
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN,
                 "Container '%s' not found\n", container_id);
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    resp.status = 0;
    snprintf(resp.message, CONTROL_MESSAGE_LEN, "Log: %s\n", log_path);
    send(client_fd, &resp, sizeof(resp), 0);

    /* Stream log file contents */
    int fd = open(log_path, O_RDONLY);
    if (fd < 0) return;
    char fbuf[4096];
    ssize_t n;
    while ((n = read(fd, fbuf, sizeof(fbuf))) > 0)
        send(client_fd, fbuf, n, 0);
    close(fd);
}

static void handle_stop(supervisor_ctx_t *ctx, int client_fd,
                          const char *container_id)
{
    control_response_t resp;
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *r = ctx->containers;
    while (r) {
        if (strncmp(r->id, container_id, CONTAINER_ID_LEN) == 0) {
            if (r->state == CONTAINER_RUNNING) {
                kill(r->host_pid, SIGTERM);
                r->state = CONTAINER_STOPPED;
            }
            resp.status = 0;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "Stopped container '%s'\n", container_id);
            pthread_mutex_unlock(&ctx->metadata_lock);
            send(client_fd, &resp, sizeof(resp), 0);
            return;
        }
        r = r->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
    resp.status = -1;
    snprintf(resp.message, CONTROL_MESSAGE_LEN,
             "Container '%s' not found\n", container_id);
    send(client_fd, &resp, sizeof(resp), 0);
}

static void handle_start(supervisor_ctx_t *ctx, int client_fd,
                           const control_request_t *req, int wait_for_exit)
{
    control_response_t resp;
    container_record_t *rec = launch_container(ctx, req);
    if (!rec) {
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN,
                 "Failed to launch container '%s'\n", req->container_id);
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }
    resp.status = 0;
    snprintf(resp.message, CONTROL_MESSAGE_LEN,
             "Started container '%s' pid=%d\n", rec->id, rec->host_pid);
    send(client_fd, &resp, sizeof(resp), 0);

    if (wait_for_exit) {
        /* run mode: block until container exits */
        int status;
        waitpid(rec->host_pid, &status, 0);
        /* sigchld_handler will update state; send another message */
        snprintf(resp.message, CONTROL_MESSAGE_LEN,
                 "Container '%s' exited\n", rec->id);
        send(client_fd, &resp, sizeof(resp), 0);
    }
}

/* ------------------------------------------------------------------ */
/*  Supervisor main loop                                               */
/* ------------------------------------------------------------------ */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    int rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) { perror("bounded_buffer_init"); return 1; }

    /* Open kernel monitor (optional — don't fail if not loaded) */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] Warning: kernel monitor not available: %s\n",
                strerror(errno));

    /* Create log directory */
    mkdir(LOG_DIR, 0755);

    /* Create UNIX domain socket */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen"); return 1;
    }

    /* Signal handlers */
    struct sigaction sa_chld, sa_term;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sa_chld.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sigterm_handler;
    sigaction(SIGINT,  &sa_term, NULL);
    sigaction(SIGTERM, &sa_term, NULL);

    /* Start logging thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) { perror("pthread_create logger"); return 1; }

    fprintf(stderr, "[supervisor] Ready. Listening on %s\n", CONTROL_PATH);

    /* Set socket non-blocking for graceful shutdown check */
    int flags = fcntl(ctx.server_fd, F_GETFL, 0);
    fcntl(ctx.server_fd, F_SETFL, flags | O_NONBLOCK);

    /* Event loop */
    while (!ctx.should_stop) {
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(ctx.server_fd,
                               (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);
                continue;
            }
            if (errno == EINTR) continue;
            if (!ctx.should_stop) perror("accept");
            break;
        }

        control_request_t req;
        ssize_t n = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
        if (n != (ssize_t)sizeof(req)) {
            close(client_fd);
            continue;
        }

        switch (req.kind) {
        case CMD_START:
            handle_start(&ctx, client_fd, &req, 0);
            break;
        case CMD_RUN:
            handle_start(&ctx, client_fd, &req, 1);
            break;
        case CMD_PS:
            handle_ps(&ctx, client_fd);
            break;
        case CMD_LOGS:
            handle_logs(&ctx, client_fd, req.container_id);
            break;
        case CMD_STOP:
            handle_stop(&ctx, client_fd, req.container_id);
            break;
        default:
            break;
        }
        close(client_fd);
    }

    fprintf(stderr, "[supervisor] Shutting down...\n");

    /* Stop all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *r = ctx.containers;
    while (r) {
        if (r->state == CONTAINER_RUNNING) {
            kill(r->host_pid, SIGTERM);
            r->state = CONTAINER_STOPPED;
        }
        r = r->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait for children */
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    sleep(1);
    while (waitpid(-1, NULL, WNOHANG) > 0) {}

    /* Shutdown logging */
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    /* Free container records */
    pthread_mutex_lock(&ctx.metadata_lock);
    r = ctx.containers;
    while (r) {
        container_record_t *next = r->next;
        free(r);
        r = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    fprintf(stderr, "[supervisor] Clean shutdown complete.\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Client-side: send a request and print the response                 */
/* ------------------------------------------------------------------ */

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n"
                        "Is the supervisor running?\n",
                CONTROL_PATH, strerror(errno));
        close(fd);
        return 1;
    }

    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send"); close(fd); return 1;
    }

    /* Read response(s) */
    control_response_t resp;
    ssize_t n;
    int got_response = 0;
    while ((n = recv(fd, &resp, sizeof(resp), 0)) == (ssize_t)sizeof(resp)) {
        printf("%s", resp.message);
        got_response = 1;
        if (req->kind != CMD_LOGS) break;
        /* For logs, keep reading until socket closes */
    }
    /* For logs, drain any remaining raw data */
    if (req->kind == CMD_LOGS && got_response) {
        char buf[4096];
        while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
            fwrite(buf, 1, n, stdout);
    }

    close(fd);
    return got_response ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/*  CLI command handlers                                               */
/* ------------------------------------------------------------------ */

static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
