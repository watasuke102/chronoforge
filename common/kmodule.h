#pragma once

#ifdef _cplusplus
#include <cstdint>
#else
#include <linux/types.h>
#endif

#define KMODULE_IOCTL_SETUP_SCHEDULER 0
#define KMODULE_IOCTL_SETUP_CLIENT    1
#define KMODULE_IOCTL_START           2
#define KMODULE_IOCTL_INTR            3

struct SharedContextPerCpu {
  bool     is_busy;
  uint64_t task_started_at;  // rdtsc() value
  // becomes 0 when the task is executed by kmodule
  pid_t    next_task_id;
};

#define KMODULE_SHM_ARRAY_LEN 256
