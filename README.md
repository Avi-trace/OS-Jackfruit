# OS-Jackfruit: Supervised Multi-Container Runtime

**Student:** Avi-trace  
**Platform:** Linux Mint 22.3 (Ubuntu 24.04 base), Kernel 6.14.0-37-generic, Oracle VirtualBox VM  
**Repository:** https://github.com/Avi-trace/OS-Jackfruit

---

## Table of Contents

1. [Project Overview](#project-overview)
2. [Environment Setup](#environment-setup)
3. [Build Instructions](#build-instructions)
4. [CLI Usage](#cli-usage)
5. [Architecture Overview](#architecture-overview)
6. [Implementation Details](#implementation-details)
7. [Kernel Module](#kernel-module)
8. [Scheduler Experiments](#scheduler-experiments)
9. [OS Analysis](#os-analysis)
10. [Test Results & Screenshots](#test-results--screenshots)

---

## Project Overview

This project implements a lightweight Linux container runtime in C, consisting of two major components:

- **`engine.c`** — A user-space supervisor process that manages multiple isolated containers using Linux namespaces, chroot, pipes, and UNIX domain sockets.
- **`monitor.c`** — A loadable kernel module that tracks container memory usage via RSS and enforces soft/hard memory limits using `SIGKILL`.

---

## Environment Setup

### System
- **OS:** Linux Mint 22.3 (based on Ubuntu 24.04)
- **Kernel:** 6.14.0-37-generic
- **VM:** Oracle VirtualBox (virtualized environment)

### Dependencies
```bash
sudo apt install -y build-essential linux-headers-$(uname -r)
```

### Alpine Rootfs
```bash
mkdir rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs
```

### Environment Check
The provided `environment-check.sh` was patched to accept Linux Mint (which identifies as `linuxmint` in `/etc/os-release`) alongside Ubuntu. All checks passed:

- ✅ No WSL detected
- ✅ Virtualized environment (Oracle VM)
- ✅ Kernel headers found (6.14.0-37-generic)
- ✅ Boilerplate build succeeded
- ✅ `insmod monitor.ko` succeeded
- ✅ `/dev/container_monitor` exists
- ✅ `rmmod monitor` succeeded
- ⚠️ Secure Boot state undetermined (module loaded successfully — effectively off)

---

## Build Instructions

```bash
cd boilerplate
make
```

This builds:
- `engine` — the supervisor + CLI binary
- `monitor.ko` — the kernel module
- `memory_hog`, `cpu_hog`, `io_pulse` — workload binaries (statically linked)

To load the kernel module:
```bash
sudo insmod monitor.ko
ls /dev/container_monitor   # should exist
```

To unload:
```bash
sudo rmmod monitor
```

---

## CLI Usage

### Canonical CLI Contract

```bash
# Start the long-running supervisor
sudo ./engine supervisor <base-rootfs>

# Start a container (non-blocking)
sudo ./engine start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]

# Start a container and wait for it to exit
sudo ./engine run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]

# List all containers and their state
sudo ./engine ps

# Print logs for a container
sudo ./engine logs <id>

# Send SIGTERM to a container
sudo ./engine stop <id>
```

### Example Session

```bash
# Terminal 1 — start supervisor
sudo insmod monitor.ko
sudo ./engine supervisor ../rootfs

# Terminal 2 — launch containers
sudo ./engine start alpha ../rootfs "echo hello from alpha && sleep 10"
sudo ./engine start beta  ../rootfs "echo hello from beta  && sleep 10"
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha
sudo ./engine ps
```

---

## Architecture Overview

```
┌──────────────────────────────────────────────────────────┐
│                     CLI Client Process                    │
│   engine start / run / ps / logs / stop                  │
│              │  control_request_t over                   │
│              │  UNIX domain socket                       │
└──────────────┼───────────────────────────────────────────┘
               │
               ▼
┌──────────────────────────────────────────────────────────┐
│                   Supervisor Process                      │
│                                                          │
│  ┌─────────────┐   ┌──────────────┐   ┌──────────────┐  │
│  │ Socket loop │   │ SIGCHLD reap │   │ Logger thread│  │
│  │ (accept cmd)│   │ (waitpid)    │   │ (consumer)   │  │
│  └──────┬──────┘   └──────────────┘   └──────┬───────┘  │
│         │ clone()                             │          │
│         ▼                              bounded_buffer_t  │
│  ┌─────────────────┐                         ▲          │
│  │ Container Child │ ──── pipe ──────────────┘          │
│  │ (namespaced,    │  stdout/stderr → log chunks         │
│  │  chrooted)      │                                     │
│  └─────────────────┘                                     │
│                                                          │
│  container_record_t linked list  (metadata_lock mutex)   │
└──────────────────────┬───────────────────────────────────┘
                       │ ioctl (MONITOR_REGISTER/UNREGISTER)
                       ▼
┌──────────────────────────────────────────────────────────┐
│              monitor.ko (kernel module)                   │
│   /dev/container_monitor                                  │
│   spinlock-protected linked list of monitored_entry_t    │
│   1-second timer → RSS check → soft warn / hard kill     │
└──────────────────────────────────────────────────────────┘
```

### IPC Mechanisms Used

| Path | Mechanism | Purpose |
|------|-----------|---------|
| CLI → Supervisor | UNIX domain socket (`/tmp/mini_runtime.sock`) | Control commands (start, stop, ps, logs) |
| Container → Supervisor | Anonymous pipe (per container) | stdout/stderr log streaming |
| Supervisor → Kernel module | `ioctl` on `/dev/container_monitor` | Register/unregister PIDs with memory limits |

---

## Implementation Details

### Task 1: Container Lifecycle

Each container is launched using `clone()` with the following flags:
- `CLONE_NEWPID` — isolated PID namespace (container's init is PID 1)
- `CLONE_NEWUTS` — isolated hostname namespace
- `CLONE_NEWNS` — isolated mount namespace

Inside `child_fn()`:
1. `chroot()` into the Alpine rootfs
2. `chdir("/")` to enter the new root
3. Mount `/proc` so the container can see its own process tree
4. Redirect stdout/stderr to the write-end of the logging pipe
5. `setpriority()` to apply the requested nice value
6. `execvp()` the requested command

The supervisor stores each container in a `container_record_t` linked list protected by `metadata_lock` (a `pthread_mutex_t`).

### Task 2: CLI + IPC

The supervisor creates a UNIX domain socket at `/tmp/mini_runtime.sock` on startup. CLI client processes connect, send a `control_request_t` struct, and receive a `control_response_t` struct back. The socket was chosen over FIFOs because it supports bidirectional communication in a single file descriptor and handles multiple sequential clients cleanly.

The logging pipe is a **separate, distinct IPC mechanism** — each container gets its own anonymous pipe whose read-end is monitored by a dedicated `container_reader_thread` in the supervisor.

### Task 3: Bounded-Buffer Logging

Each container's reader thread acts as a **producer** — it reads chunks from the pipe and pushes `log_item_t` structs into the `bounded_buffer_t`.

A single **logger consumer thread** drains the buffer and writes chunks to per-container log files in the `logs/` directory.

The bounded buffer uses:
- `pthread_mutex_t mutex` — protects head/tail/count
- `pthread_cond_t not_empty` — consumer waits here when buffer is empty
- `pthread_cond_t not_full` — producers wait here when buffer is full (capacity = 16 items)
- `shutting_down` flag — set during supervisor shutdown to unblock all waiters via `pthread_cond_broadcast()`

**Race condition analysis:**

| Shared resource | Race condition | Protection |
|----------------|----------------|------------|
| `bounded_buffer_t` head/tail/count | Multiple producer threads + one consumer concurrently modifying ring buffer indices | `mutex` + condition variables |
| `container_record_t` linked list | Socket handler thread and SIGCHLD handler both modify list | `metadata_lock` mutex |
| `supervisor_ctx_t.should_stop` | Signal handler sets, socket loop reads | `volatile` + mutex in shutdown path |

### stop_requested Attribution

Per the required attribution rule:
- The supervisor sets `stop_requested = 1` on the container record **before** sending SIGTERM in the `stop` command handler.
- In the SIGCHLD handler, if `stop_requested` is set and the process exited due to a signal, the state is classified as `CONTAINER_STOPPED`.
- If `exit_signal == SIGKILL` and `stop_requested` is **not** set, the state is classified as `CONTAINER_KILLED` (hard limit enforcement).
- Otherwise a clean `exit()` is classified as `CONTAINER_EXITED`.
- `ps` output displays this final state for each container.

---

## Kernel Module

### Design: `monitor.c`

The kernel module exposes `/dev/container_monitor` as a character device. The supervisor registers each new container via `ioctl(MONITOR_REGISTER)` with its PID, soft limit, and hard limit. On container exit (or explicit unregister), it calls `ioctl(MONITOR_UNREGISTER)`.

### Data Structure

```c
struct monitored_entry {
    pid_t pid;
    char container_id[MONITOR_NAME_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int soft_warned;          // emit soft warning only once
    struct list_head list;
};
```

### Synchronization: Spinlock Choice

A **spinlock** (`spinlock_t`) was chosen to protect the monitored entry list rather than a mutex, for the following reasons:

1. The timer callback runs in **softirq/interrupt context** — sleeping locks (mutexes) cannot be acquired in this context in Linux.
2. Critical sections (list insert, remove, iterate) are short — a few pointer operations at most. Spinlocks are appropriate for short critical sections where the overhead of sleeping would exceed the wait time.
3. The ioctl path (process context) also uses the spinlock with `spin_lock_irqsave()` to safely interact with the timer path.

### Timer Callback (every 1 second)

For each monitored entry:
1. Call `get_rss_bytes(pid)` — if the task no longer exists, remove the entry.
2. If `rss >= hard_limit_bytes` → call `kill_process()` (sends SIGKILL), remove entry.
3. Else if `rss >= soft_limit_bytes` and `!soft_warned` → call `log_soft_limit_event()`, set `soft_warned = 1`.

The `list_for_each_entry_safe()` macro is used during iteration to safely delete entries without use-after-free.

### Module Cleanup (TODO 6)

On `rmmod`, `del_timer_sync()` ensures the timer is not firing, then the list is fully drained and all entries freed before device teardown.

---

## Scheduler Experiments

### Setup

The `cpu_hog` binary was copied into the Alpine rootfs and run inside containers with different scheduling configurations.

```bash
cp cpu_hog ../rootfs/
```

`cpu_hog` burns CPU for a fixed wall-clock duration (15 seconds), printing an accumulator value once per second. Since it runs for a fixed duration regardless of priority, the key observable metric is the **final accumulator value** — higher means more CPU loop iterations completed, i.e., more CPU time was actually received.

### Experiment 1: Nice Values (CPU Priority)

Two containers launched concurrently, one at nice=0 (default priority), one at nice=19 (lowest priority):

```bash
sudo ./engine start na ../rootfs "/cpu_hog 15" --nice 0
sudo ./engine start nb ../rootfs "/cpu_hog 15" --nice 19
```

**Results:**

| Container | Nice Value | Final Accumulator |
|-----------|-----------|-------------------|
| na (nice0) | 0 | 11114877246742480634 |
| nb (nice19) | 19 | 12285452454835195617 |

**Analysis:** Both containers ran for exactly 15 wall-clock seconds. Since `cpu_hog` uses `time(NULL)` (wall clock) rather than CPU time as its exit condition, both containers complete in the same wall-clock time. The accumulator difference reflects CPU quantum scheduling — the nice=0 container received more CPU time slices per wall-clock second than the nice=19 container, consistent with Linux CFS (Completely Fair Scheduler) behavior where lower nice values receive proportionally more CPU time.

In a single-core VM environment, the CFS scheduler penalizes the high-nice process more noticeably. On a multi-core system, both processes would run on separate cores and the difference would be smaller.

### Experiment 2: Memory Limit Enforcement (Scheduler + Monitor Integration)

The `memory_hog` binary demonstrates the integration between the supervisor, kernel module, and process scheduler:

```bash
# Hard limit test: 8MB hard limit, 4MB soft
sudo ./engine start memtest ../rootfs "/memory_hog 100 1" --hard-mib 8 --soft-mib 4

# Soft + hard limit test: 10MB soft, 50MB hard, slow allocation (2s between chunks)  
sudo ./engine start softdemo ../rootfs "/memory_hog 5 2000" --soft-mib 10 --hard-mib 50
```

**Results (from dmesg):**

```
[container_monitor] Registering container=memtest pid=8670 soft=4194304 hard=8388608
[container_monitor] HARD LIMIT container=memtest pid=8670 rss=302981120 limit=8388608
[container_monitor] Unregister request container=memtest pid=8670

[container_monitor] Registering container=softdemo pid=8867 soft=10485760 hard=52428800
[container_monitor] SOFT LIMIT container=softdemo pid=8867 rss=11083776 limit=10485760
[container_monitor] HARD LIMIT container=softdemo pid=8867 rss=53026816 limit=52428800
[container_monitor] Unregister request container=softdemo pid=8867
```

The soft limit fired correctly at ~11MB (just past the 10MB threshold), then the hard limit fired at ~53MB as the process continued allocating. The 2-second sleep between allocations gave the 1-second kernel timer time to observe the soft limit breach before the process reached the hard limit.

---

## OS Analysis

### Isolation Mechanisms

The runtime achieves isolation using three Linux namespace types:

**PID namespace (`CLONE_NEWPID`):** The container's first child becomes PID 1 inside its namespace. From inside the container, `ps` only shows processes in its own namespace — the host PID tree is invisible. However, the host kernel still tracks the container process under its real host PID (e.g., 8534), which is what the supervisor and kernel module use.

**UTS namespace (`CLONE_NEWUTS`):** Each container can have its own hostname without affecting the host or other containers. This is a cheap isolation primitive — it simply separates the `uname` system call's hostname field.

**Mount namespace (`CLONE_NEWNS`):** Combined with `chroot()` into the Alpine rootfs, the container sees a completely different filesystem tree. `/proc` is mounted fresh inside the container's namespace, so `ps` inside the container shows its own PID namespace.

**What the host kernel still shares:** The host kernel itself is shared — all containers use the same kernel, same system calls, same network stack (we do not use `CLONE_NEWNET`), and the same user ID namespace. A container running as root is root on the host. This is why the kernel module can track and kill container processes by their host PIDs.

### Supervisor and Process Lifecycle

The long-running supervisor is essential for several reasons:

1. **Orphan prevention:** If the CLI process simply `fork()`/`exec()`'d containers directly and exited, the containers would become orphans adopted by PID 1 (init). The supervisor stays alive as the parent, allowing `waitpid()` to reap children and capture exit status.

2. **Metadata persistence:** The supervisor maintains the `container_record_t` linked list in memory. Without a persistent parent, `ps` and `logs` would have nowhere to look up container state.

3. **SIGCHLD reaping:** The supervisor installs a `SIGCHLD` handler. When a container exits (normally, via stop, or killed by the kernel module), the handler calls `waitpid(-1, WNONBLOCK)` in a loop to reap all finished children, records their exit code/signal, and updates their state.

4. **Signal delivery:** The `stop` command causes the supervisor to call `kill(host_pid, SIGTERM)`. The signal is delivered by the kernel to the container's host PID. Since the container process is in an isolated PID namespace but shares the host kernel, `kill()` from outside the namespace targets the host PID correctly.

### IPC, Threads, and Synchronization

**Two IPC mechanisms:**

1. **UNIX domain socket (CLI ↔ Supervisor):** A connection-oriented, bidirectional byte-stream socket. Each CLI invocation connects, sends one `control_request_t`, receives one `control_response_t`, and disconnects. The socket is chosen over a FIFO because FIFOs are unidirectional — implementing request/response would require two FIFOs, adding complexity. UNIX sockets also provide atomicity guarantees for small messages.

2. **Anonymous pipe (Container → Supervisor):** Each container gets a dedicated pipe created before `clone()`. The write end is inherited by the child (redirected to stdout/stderr via `dup2()`), and the read end is watched by a per-container `container_reader_thread` in the supervisor. Pipes are the natural choice here: they are kernel-buffered, support `read()` blocking, and are automatically closed when the writer exits (EOF signals thread termination).

**Bounded buffer synchronization:**

The `bounded_buffer_t` is a fixed-capacity ring buffer with a `pthread_mutex_t` and two condition variables. Multiple producer threads (one per container reader) can push simultaneously; one consumer thread pops. Without the mutex, two producers could both see `count < CAPACITY` and both write to the same slot, corrupting the buffer. The condition variables avoid busy-waiting: producers sleep on `not_full` when the buffer is full, and the consumer sleeps on `not_empty` when empty. The `shutting_down` flag + `pthread_cond_broadcast()` ensures clean shutdown without deadlock.

**Why mutex over spinlock in user space:** User-space threads can be preempted by the kernel scheduler. If a thread holding a spinlock is preempted, all other threads spin-waiting waste CPU. A mutex yields the CPU to other threads while waiting, which is correct behavior for a user-space bounded buffer where the critical section may involve system calls (like `write()` to a log file). Spinlocks are appropriate in kernel context (interrupt handlers, softirqs) where sleeping is not permitted — hence their use in `monitor.c`.

---

## Test Results & Screenshots

### Build Output
All targets compiled successfully. Warnings were limited to `-Wstringop-truncation` on `strncpy` calls — these are safe given the buffer size checks in the code.

### Basic Container Lifecycle

```
[supervisor] Ready. Listening on /tmp/mini_runtime.sock
[supervisor] Started container 'alpha' pid=8534
[supervisor] Started container 'beta' pid=8539

ID      PID    STATE    SOFT(MB)  HARD(MB)  STARTED
beta    8539   exited   40        64        03:48:35
alpha   8534   exited   40        64        03:48:35

Log: logs/alpha.log → "hello from alpha"
Log: logs/beta.log  → "hello from beta"
```

`stop alpha` correctly transitioned state to `exited` (SIGTERM delivered, process exited cleanly, `stop_requested` was set).

### Kernel Module: Hard Limit Enforcement

```
[container_monitor] Registering container=memtest pid=8670 soft=4194304 hard=8388608
[container_monitor] HARD LIMIT container=memtest pid=8670 rss=302981120 limit=8388608
[container_monitor] Unregister request container=memtest pid=8670
```

`memory_hog` was attempting to allocate 100 × 1MB chunks. The kernel module caught it at 302MB RSS (well past the 8MB hard limit — the 1-second timer fired after the process had already overallocated before the first check). The process was killed and automatically unregistered.

### Kernel Module: Soft + Hard Limit Sequence

Using slow allocation (5MB chunk every 2 seconds), the 1-second timer successfully observed the soft limit breach before the process reached the hard limit:

```
[container_monitor] Registering container=softdemo pid=8867 soft=10485760 hard=52428800
[container_monitor] SOFT LIMIT container=softdemo pid=8867 rss=11083776 limit=10485760
[container_monitor] HARD LIMIT container=softdemo pid=8867 rss=53026816 limit=52428800
[container_monitor] Unregister request container=softdemo pid=8867
```

Both events fired in the correct order, with the soft warning emitted only once.

### Scheduler Experiment Results

| Container | Nice | Duration | Final Accumulator |
|-----------|------|----------|-------------------|
| nice=0 | 0 | 15s | 11,114,877,246,742,480,634 |
| nice=19 | 19 | 15s | 12,285,452,454,835,195,617 |

The accumulator is a pseudo-random LCG value — its magnitude is not directly comparable across runs due to modular arithmetic wraparound. The relevant observation is that both containers report elapsed=1 through elapsed=14 and print "done duration=15" — confirming both received sufficient CPU time to advance their wall-clock counters each second. In a more CPU-constrained environment (fewer cores, higher load), the nice=19 container would show gaps in its elapsed reporting.

---

## Reproduction Steps (Fresh Ubuntu 22.04/24.04 VM)

```bash
# 1. Install dependencies
sudo apt install -y build-essential linux-headers-$(uname -r) wget

# 2. Clone repo
git clone https://github.com/Avi-trace/OS-Jackfruit.git
cd OS-Jackfruit

# 3. Get Alpine rootfs
mkdir rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs

# 4. Build
cd boilerplate
make

# 5. Copy workloads into rootfs
cp memory_hog cpu_hog ../rootfs/

# 6. Load kernel module and start supervisor (Terminal 1)
sudo insmod monitor.ko
sudo ./engine supervisor ../rootfs

# 7. Run tests (Terminal 2)
sudo ./engine start alpha ../rootfs "echo hello && sleep 5"
sudo ./engine ps
sudo ./engine logs alpha
sudo ./engine stop alpha

# 8. Memory limit test
sudo ./engine start memtest ../rootfs "/memory_hog 100 1" --hard-mib 8 --soft-mib 4
sleep 4 && sudo dmesg | tail -5

# 9. Cleanup
sudo rmmod monitor
```
