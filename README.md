# OneWo zepLinux — A New Zephyr OS with Linux API for MCU, Developed by OneWo-rtLinux Team

Implementing a Linux-compatible interface layer on Zephyr RTOS, enabling Linux applications to run on ARM Cortex-M4 and ANSILIC RISC-V 32 microcontrollers with minimal modifications.

## Latest Release: v0.6

**OneWo zepLinux v0.6** was released. This release focuses on the per-process signal mechanism, keyboard interrupt handling, job control, and VFS ramfs integration. The v0.6 release content corresponds to commit `da6e7f4e` on the `main` branch, and all related functional code has been merged.

### Per-Process Signal Mechanism and Keyboard Interrupts

Release contents:

- Replaced global event broadcast with a per-process signal delivery model, closely following the Linux signal model.
- Added per-process signal state tracking, including pending signals, blocked signal masks, and custom signal handlers.
- Added foreground/background process group management and targeted signal delivery via `kill()`.
- Implemented `signal()`, `sigblock()`, and `sigunblock()` with POSIX-compatible `SIGINT`, `SIGTERM`, and related constants.
- Added Ctrl+C keyboard interrupt support for both AS32x601 and RocketPi shells: pressing Ctrl+C sends `SIGINT` to the foreground process only and is consumed rather than echoed to the input buffer.

Test validation:

- Verified that `kill()` delivers a signal to the targeted process without affecting other running processes.
- Verified custom signal handler registration and invocation via `signal()`.
- Verified that Ctrl+C interrupts a running foreground process on AS32x601 EVB and RocketPi hardware.
- Verified signal blocking and unblocking with `sigblock()`/`sigunblock()`.
- Verified POSIX `kill`, `signal`, `SIGINT`, and `SIGTERM` behavior in the AS32x601 shell environment.

### Job Control and VFS Integration

Release contents:

- Ported complete job control (Ctrl+D suspend/resume) from QEMU Cortex-M3 to both AS32x601 EVB and RocketPi.
- Added `SIGSTOP`/`SIGCONT` signal delivery for suspending and resuming foreground processes via Ctrl+D.
- Added VFS ramfs support to both `rocket_pi_shell_process` and `as32x601_shell_process` samples, enabled via `CONFIG_VFS_CORE=y` and `CONFIG_RAMFS=y`.
- Added `vfs_commands.c` mounting ramfs at `/tmp` through `SYS_INIT` at application level, and wired up `ls`, `mkdir`, and `cat` shell commands against the VFS layer.
- Fixed ramfs driver initialization ordering, resolved `EEXIST` handling, raised `MAX_COMMANDS` limits, and corrected `ls` argument count handling on AS32x601.

Test validation:

- Verified Ctrl+D suspends the foreground process and returns the shell prompt on AS32x601 EVB and RocketPi.
- Verified that a suspended process can be resumed and continues execution correctly.
- Verified `mkdir /tmp/dir`, `cat /tmp/file`, and `ls /tmp` operate correctly against the ramfs mount point on both boards.
- Verified that remounting an already-existing path returns gracefully without crashing.
- Verified that AS32x601 `ls` correctly handles zero and one path arguments after the argument-count fix.

## Core Highlights

- 4 Linux-style schedulers (DL/RT/CFS/Idle) deeply integrated into the Zephyr kernel, truly driving `k_thread` scheduling decisions
- Currently documented 39 Linux/POSIX compatible interfaces, covering threads, processes, scheduling, signals, memory, devices, and I/O multiplexing modules
- Supported development boards: [Rocket-Pi](https://www.rocketpi.club/) (`STM32F401RE`, ARM Cortex-M4) and [ANSIC-EVB601](https://ansilic.com/product-center/ansic-evb601) (`AS32X601`, RISC-V 32)

## Supported Development Boards

- ARM: [Rocket-Pi](https://www.rocketpi.club/), based on `STM32F401RE`
- RISC-V: [ANSIC-EVB601](https://ansilic.com/product-center/ansic-evb601), based on `AS32X601`

## Directory Structure

```
OneWo-zepLinux/
├── README.md                       # Project overview (this file)
├── docs/                           # English documentation
│   ├── zephyr-linux-api-reference.md
│   ├── zephyr-linux-interface-definition.md
│   ├── zepLinux-interface-and-validation.md
│   ├── zephyr-linux-build-and-dev.md
│   └── test-demos/                 # Board validation, device demos, benchmarks, scheduler tests
├── docs/zh/                        # Chinese Version
│   ├── README.zh.md                # Project overview (Chinese Version)
│   ├── zephyr-linux-api-reference.zh.md
│   ├── zephyr-linux-build-and-dev.zh.md
│   ├── zephyr-linux-interface-definition.zh.md
│   ├── zepLinux-interface-and-validation.zh.md
│   └── test-demos/                 #
├── modules/
│   └── hal/
│       ├── ansilic/                # ANSILIC RISC-V 32 HAL
│       └── stm32/                  # STM32 HAL (placeholder)
└── zephyr/                         # Built-in Zephyr source tree and kernel implementation
    ├── kernel/                     # Kernel scheduling and thread core logic
    ├── include/                    # Public header files
    ├── lib/                        # Base libraries
    ├── subsys/                     # Various subsystems
    ├── tests/                      # Zephyr built-in tests
    ├── samples/                    # Zephyr samples
    └── boards/others/              # Custom board support (rocket_pi, stm32f401_mini)
```

## Scheduler Architecture

Enabled through the `CONFIG_SCHED_LINUX` Kconfig option, serving as Zephyr's 4th pluggable ready queue implementation (alongside SIMPLE/SCALABLE/MULTIQ).

```
┌─────────────────────────────────────────┐
│        Zephyr Kernel Scheduler          │
│  _priq_run_add / remove / best / yield  │
└──────────────┬──────────────────────────┘
               │ CONFIG_SCHED_LINUX
               ▼
┌─────────────────────────────────────────┐
│         z_priq_linux_best()             │
│  DL(EDF/CBS) → RT(FIFO/RR) → CFS → Idle│
└──────────────┬──────────────────────────┘
               │
    ┌──────────┼──────────┬───────────┐
    ▼          ▼          ▼           ▼
  DL Queue   RT Queue   CFS Queue   Idle
  Sorted by  bitmap     Sorted by   (Zephyr
  deadline   O(1)       vruntime    built-in)
  ascending  lookup     ascending
```

**Scheduling Class Priority**: DL > RT > CFS > Idle

| Sched Class | Policy | Sort Criteria | Features |
|-------------|--------|---------------|----------|
| DL | SCHED_DEADLINE | Absolute deadline ascending | EDF + CBS throttling/replenishment |
| RT | SCHED_FIFO / SCHED_RR | Priority 1-99 bitmap | FIFO no timeslice, RR 10 tick round-robin |
| CFS | SCHED_NORMAL | vruntime ascending | nice -20..19 weight table, fair allocation |
| Idle | SCHED_IDLE | — | Zephyr idle thread fallback |

## Per-Process Signal Architecture

Each process carries its own `signal_state` — pending set, blocked mask, and handler table — so signals are delivered to a specific process rather than broadcast globally.

```
  Keyboard Input                        Explicit kill()
  ┌──────────────────┐  ┌────────────────────┐  ┌──────────────────┐
  │  Ctrl+C (UART)   │  │  Ctrl+D (UART)     │  │  kill(pid, sig)  │
  │  → SIGINT        │  │  → SIGSTOP/SIGCONT │  │  (shell command) │
  └────────┬─────────┘  └─────────┬──────────┘  └────────┬─────────┘
           │                   │                        │
           └───────────────────┴────────────────────────┘
                               │
                               ▼
  ┌─────────────────────────────────────────────┐
  │              Signal Delivery                │
  │  resolve foreground PID (process group)     │
  │  look up z_process via pid_table[]          │
  └─────────────────────┬───────────────────────┘
                        │
                        ▼
  ┌─────────────────────────────────────────────┐
  │           z_process.signal_state            │
  │  ┌─────────────────┬─────────────────────┐  │
  │  │  pending_mask   │   blocked_mask      │  │
  │  ├─────────────────┴─────────────────────┤  │
  │  │  handlers[]                           │  │
  │  │    SIGINT  → fn / SIG_DFL / SIG_IGN   │  │
  │  │    SIGTERM → fn / SIG_DFL / SIG_IGN   │  │
  │  │    SIGSTOP → suspend (job control)    │  │
  │  │    SIGCONT → resume  (job control)    │  │
  │  └───────────────────────────────────────┘  │
  └─────────────────────┬───────────────────────┘
                        │ signal not in blocked_mask?
                        ▼
  ┌─────────────────────────────────────────────┐
  │              Handler Dispatch               │
  │  SIG_DFL  →  default action (terminate /    │
  │               suspend / ignore by signal)   │
  │  SIG_IGN  →  discard silently               │
  │  fn ptr   →  call registered handler        │
  └─────────────────────────────────────────────┘
```

Only the targeted process receives the signal; other processes running concurrently are unaffected.

## Modified Zephyr Kernel Files

| File | Modification |
|------|--------------|
| `include/zephyr/kernel_structs.h` | Added `struct _priq_linux`, extended `_ready_q` |
| `include/zephyr/kernel/thread.h` | Added scheduling class metadata fields to `_thread_base` |
| `kernel/include/priority_q.h` | Added `CONFIG_SCHED_LINUX` macro + override `z_sched_prio_cmp` |
| `kernel/Kconfig` | Added `CONFIG_SCHED_LINUX` option |
| `kernel/thread.c` | New threads default initialized to CFS class |
| `kernel/timeslicing.c` | Added `z_linux_sched_tick()` hook |
| `kernel/CMakeLists.txt` | Added `linux_sched/` source files |

## Build and Test

### Using Docker (Recommended)

We provide pre-built Docker images with Zephyr SDK 0.17.4 for easy development:

```bash
# Pull the Docker image
docker pull zhouzhouyi/zephyr-sdk:latest
# Or specific version
docker pull zhouzhouyi/zephyr-sdk:0.17.4

# Run the container with your project mounted
docker run -it --rm \
  -v $(pwd):/workspace \
  -w /workspace \
  zhouzhouyi/zephyr-sdk:latest

# Inside the container, build your project
west build -b rocket_pi/stm32f401xe zephyr/tests/kernel/sched/schedule_api

# Flash the firmware (requires USB device passthrough)
docker run -it --rm \
  --privileged \
  -v /dev:/dev \
  -v $(pwd):/workspace \
  -w /workspace \
  zhouzhouyi/zephyr-sdk:latest \
  west flash
```

**Docker Image Features:**
- Ubuntu 22.04 base
- Zephyr SDK 0.17.4 pre-installed
- All required build dependencies (CMake, Ninja, device-tree-compiler, etc.)
- West tool and Python dependencies
- Ready to use, no manual SDK installation needed

### Native Build

```bash
# Build kernel integration test (real thread scheduling)
west build -b rocket_pi/stm32f401xe zephyr/tests/kernel/sched/schedule_api
```

## Interface Category Overview

| Category | Interface Count | Examples |
|----------|-----------------|----------|
| Thread Management | 7 | pthread_create, pthread_join, pthread_cancel, pthread_mutex_lock, pthread_mutex_unlock, pthread_cond_wait, pthread_cond_signal |
| Process Management | 3 | fork, execve, exit |
| Scheduling & Priority | 3 | sched_yield, sched_setparam, sched_getparam |
| Signals & Timers | 8 | signal, kill, alarm, timer_create, sleep, usleep, nanosleep, timer_settime |
| Memory Management | 3 | malloc, realloc, free |
| Device Management | 5 | open, close, read, write, ioctl |
| IPC & Environment | 3 | pipe, getenv, getuid |
| Time & Process Control | 4 | clock_gettime, wait, waitpid, posix_spawn |
| I/O Multiplexing | 3 | select, poll, epoll |

## Documentation

### English

- Interface API Reference: `docs/zephyr-linux-api-reference.md`
- Interface Design and Kernel Mapping: `docs/zephyr-linux-interface-definition.md`
- Interface and Validation Overview: `docs/zepLinux-interface-and-validation.md`
- Build and Development Guide: `docs/zephyr-linux-build-and-dev.md`
- Test Demo Documentation: `docs/test-demos/`

### Chinese Version

- README: `docs/zh/README.zh.md`
- Interface API Reference:  `docs/zh/zephyr-linux-api-reference.zh.md`
- Interface Design and Kernel Mapping: `docs/zh/zephyr-linux-interface-definition.zh.md`
- Interface and Validation Overview: `docs/zh/zepLinux-interface-and-validation.zh.md`
- Build and Development Guide: `docs/zh/zephyr-linux-build-and-dev.zh.md`
- Test Demo Documentation: `docs/zh/test-demos/`

## License

Apache-2.0
