# Chronoforge

A centralized scheduler for Linux (WIP)

## Components

- `scheduler`: a user-space scheduler
- `kmodule`: a Linux kernel module which manages tasks follows signals by the scheduler
- `clients`: clients managed by scheduler
  - `runtime`: runtime component which is should be linked with clients

## Requirement

C/C++ compiler, make and cmake (>= 3.10) on Linux

## Setup

1. Build `kmodule` and install; `cd kmodule/`

   1. build: run `make` to build
   1. install: run `make install` to install the built kernel module and `make /dev/kmodule` to crate dev file.

1. Build `scheduler` and start; `cd scheduler/`

   1. setup: `cmake -S. -Bbuild -GNinja`
   1. build: `cmake --build build`
   1. execute: `sudo ./build/scheduler`; note that root is required.

1. Build C++ client and start; `cd clients/cpp/`

   1. setup: `cmake -S. -Bbuild -GNinja`
   1. build: `cmake --build build`
   1. execute: `./build/cpp_client`

## License

Dual-licensed; [MIT](LICENSE-MIT) and [GPL-2.0](LICENSE-GPL)

---

## scheduling process

1. A task is launched. Connect with Scheduler via UNIX socket and notify to Kmodule via `ioctl()`.
   1. Scheduler adds the task to runqueue.
2. Kmodule notifies to Scheduler that the CPU core becomes idle by writing shared memory.
3. Scheduler picks a task from runqueue and requests Kmodule via `ioctl()` to execute it.
4. Scheduler checks tasks that is runnning. If a task exceeds its time slice, Scheduler requests Kmodule via `ioctl()` to park it.
