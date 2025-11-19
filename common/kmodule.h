#pragma once

#ifdef __cplusplus
#include <cstdint>
#else
#include <linux/types.h>
#endif

#define KMODULE_IOCTL_START 0
#define KMODULE_IOCTL_INTR  1
#define KMODULE_IOCTL_PARK  2

struct SharedContextPerCpu {
  bool     is_busy;
  uint64_t task_started_at;  // rdtsc() value
  // written true by scheduler, false by kmodule
  bool     is_park_requested;
  // becomes 0 when the task is executed by kmodule
  pid_t    next_task_id;
  // written by kmodule
  pid_t    running_task_id;
};

#define KMODULE_SHM_ARRAY_LEN 256
