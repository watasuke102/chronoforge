#pragma once

#ifdef __cplusplus
#include <sys/types.h>

#include <cstdint>
#else
// kmodule
#include <linux/types.h>
#endif

// sent from runtime
#define KMODULE_IOCTL_START 0
#define KMODULE_IOCTL_END   1
// sent from scheduler
#define KMODULE_IOCTL_INTR  2
#define KMODULE_IOCTL_PARK  3

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
