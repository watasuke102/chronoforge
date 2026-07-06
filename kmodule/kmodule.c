/* SPDX-License-Identifier: GPL-2.0 OR MIT */
#include "kmodule.h"

#include <linux/cdev.h>
#include <linux/cpuidle.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/tracepoint.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

// clang-format off
#define LOG_ERROR(fmt, ...) printk(KERN_ERR     "[kmodule] error: " fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  printk(KERN_WARNING "[kmodule] warn : " fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  printk(KERN_INFO    "[kmodule] info : " fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) printk(KERN_DEBUG   "[kmodule] debug: " fmt, ##__VA_ARGS__)
// clang-format on

struct KmoduleContextPerCpu {
  pid_t      last_task_id;
  spinlock_t execute_task_lock;

  // basically managed by execute_task()
  // but finally released by module_cleanup()
  struct task_struct* running_task;  // nullable
};
DEFINE_PER_CPU(struct KmoduleContextPerCpu, cpu_local_ctx);

struct SharedContextPerCpu* shm;

static void execute_task(
    struct KmoduleContextPerCpu* ctx, pid_t task_id, int cpu_index) {
  unsigned long flags;
  spin_lock_irqsave(&ctx->execute_task_lock, flags);

  if (ctx->running_task) {
    LOG_WARN(
        "execution requested CPU %d already have running task (pid: %d); "
        "removing ("
        "requested task id: %d)",
        cpu_index, ctx->running_task->pid, task_id);
    put_task_struct(ctx->running_task);
    ctx->running_task = NULL;
  }

  rcu_read_lock();
  struct pid* pid = find_vpid(task_id);
  if (!pid) {
    LOG_ERROR("failed to find pid (%d)\n", task_id);
    goto err;
  }
  struct task_struct* next = pid_task(pid, PIDTYPE_PID);
  // Failed to find the task or task is already running
  if (!next || next->on_cpu || next->__state == TASK_WAKING ||
      next->__state == TASK_RUNNING) {
    if (next) {
      LOG_ERROR("pid %d is already running (on_cpu: %d, __state: %d)", task_id,
          next->on_cpu, next->__state);
    } else {
      LOG_ERROR("requested task (pid: %d) is not found", task_id);
    }
    goto err;
  }

  ctx->running_task = next;
  set_cpus_allowed_ptr(ctx->running_task, cpumask_of(cpu_index));
  get_task_struct(ctx->running_task);
  wake_up_process(ctx->running_task);
  rcu_read_unlock();

  ctx->last_task_id = task_id;
  WRITE_ONCE(shm[cpu_index].is_busy, true);
  WRITE_ONCE(shm[cpu_index].running_task_id, task_id);
  LOG_INFO("executed task %d at cpu %d\n", task_id, cpu_index);
  spin_unlock_irqrestore(&ctx->execute_task_lock, flags);
  return;

err:
  rcu_read_unlock();
  spin_unlock_irqrestore(&ctx->execute_task_lock, flags);
  // re-queue task so that scheduler can detect the execution failure
  WRITE_ONCE(shm[cpu_index].next_task_id, task_id);
}

static void start_scheduling(void) {
  __set_current_state(TASK_INTERRUPTIBLE);
  schedule();
  __set_current_state(TASK_RUNNING);
  LOG_DEBUG("scheduling started for pid %d, current cpu: %d\n", current->pid,
      smp_processor_id());
}
static void end_scheduling(void) {
  const int                    cpu = get_cpu();
  struct KmoduleContextPerCpu* ctx = this_cpu_ptr(&cpu_local_ctx);
  LOG_INFO("task finished: cpu=%d, pid=%d, running_task: %p\n", cpu,
      shm[cpu].running_task_id, ctx->running_task);
  if (!ctx || !ctx->running_task) {
    LOG_WARN("ctx or running_task is NULL at end_scheduling()\n");
    put_cpu();
    return;
  }
  put_task_struct(ctx->running_task);
  ctx->running_task = NULL;
  WRITE_ONCE(shm[cpu].running_task_id, 0);
  put_cpu();
}

static void process_ipi_from_scheduler(void) {
  int cpu = 0;
  for_each_online_cpu(cpu) {
    struct KmoduleContextPerCpu* ctx = per_cpu_ptr(&cpu_local_ctx, cpu);
    if (ctx->running_task && READ_ONCE(shm[cpu].is_park_requested)) {
      LOG_DEBUG(
          "(ipi) parking task %d on cpu %d\n", ctx->running_task->pid, cpu);
      // just send signal, actual park request is sent from runtime via ioctl()
      send_sig(SIGUSR1, ctx->running_task, 0);
      WRITE_ONCE(shm[cpu].is_park_requested, false);
      continue;
    }
    pid_t next_task_id = READ_ONCE(shm[cpu].next_task_id);
    if (next_task_id != 0 &&
        (ctx->running_task == NULL || ctx->running_task->pid != next_task_id)) {
      if (cmpxchg(&shm[cpu].next_task_id, next_task_id, 0) != next_task_id) {
        LOG_ERROR("(ipi) cmpxchg() fail on CPU %d", cpu);
        continue;
      }
      LOG_DEBUG("(ipi) switching task %d on cpu %d\n", next_task_id, cpu);
      execute_task(ctx, next_task_id, cpu);
    }
  }
}

static void park_task(void) {
  const int                    cpu = get_cpu();
  struct KmoduleContextPerCpu* ctx = this_cpu_ptr(&cpu_local_ctx);
  if (!ctx->running_task) {
    LOG_ERROR("tried to park but ctx->running_task is NULL (CPU: %d)", cpu);
    put_cpu();
    return;
  }
  pid_t pid = ctx->running_task->pid;
  put_task_struct(ctx->running_task);
  ctx->running_task = NULL;
  WRITE_ONCE(shm[cpu].is_park_requested, false);
  WRITE_ONCE(shm[cpu].is_busy, false);
  WRITE_ONCE(shm[cpu].running_task_id, 0);
  WRITE_ONCE(shm[cpu].parked_task_id, pid);
  LOG_DEBUG("start parking task on CPU %d", cpu);
  put_cpu();

  __set_current_state(TASK_INTERRUPTIBLE);
  schedule();
  __set_current_state(TASK_RUNNING);
  LOG_DEBUG("task awaked; pid %d, current cpu: %d\n", current->pid,
      smp_processor_id());
}

static long module_ioctl(
    struct file* file, unsigned int cmd, unsigned long arg) {
  switch (cmd) {
    case KMODULE_IOCTL_START:
      start_scheduling();
      break;
    case KMODULE_IOCTL_END:
      end_scheduling();
      break;
    case KMODULE_IOCTL_PARK:
      park_task();
      break;
    case KMODULE_IOCTL_INTR:
      process_ipi_from_scheduler();
      break;
  }
  return 0;
}

static int module_mmap(struct file* file, struct vm_area_struct* vma) {
  if (capable(CAP_SYS_ADMIN)) {
    return remap_vmalloc_range(vma, (void*)shm, vma->vm_pgoff);
  }
  return -EACCES;
}

static int module_open(struct inode* inode, struct file* file) {
  return 0;
}
static int module_release(struct inode* inode, struct file* file) {
  return 0;
}

static struct file_operations ops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = module_ioctl,
    .mmap           = module_mmap,
    .open           = module_open,
    .release        = module_release,
};

static int handle_idle_enter(
    struct cpuidle_device* device, struct cpuidle_driver* driver, int index) {
  const int                    cpu = get_cpu();
  struct KmoduleContextPerCpu* ctx = this_cpu_ptr(&cpu_local_ctx);

  if (ctx->running_task) {
    put_cpu();
    return index;
  }
  WRITE_ONCE(shm[cpu].is_busy, false);

  // wait until the next task is requested (up to 8us)
  pid_t latest_next_task_id;
  for (int i = 0; i < 10; i++) {
    latest_next_task_id = READ_ONCE(shm[cpu].next_task_id);
    if (latest_next_task_id != 0 && latest_next_task_id != ctx->last_task_id) {
      break;
    }
    udelay(1);
  }

  if (latest_next_task_id == 0) {
    put_cpu();
    return index;
  }

  if (latest_next_task_id != 0 && latest_next_task_id != ctx->last_task_id) {
    if (cmpxchg(&shm[cpu].next_task_id, latest_next_task_id, 0) !=
        latest_next_task_id) {
      LOG_ERROR("(idle handler) cmpxchg() fail on CPU %d", cpu);
      put_cpu();
      return index;
    }
    LOG_INFO("(idle handler) next task: %d\n", latest_next_task_id);
    execute_task(ctx, latest_next_task_id, cpu);
  }
  put_cpu();
  LOG_INFO("(idle handler) end handling on CPU %d", cpu);
  return index;
}

static struct cpuidle_state original_state;
static int                  original_state_count;
static int                  hijack_cpuidle(void) {
  struct cpuidle_driver* driver = cpuidle_get_driver();
  if (!driver || driver->state_count <= 0) {
    return 1;
  }

  cpuidle_pause_and_lock();
  original_state          = driver->states[0];
  original_state_count    = driver->state_count;
  driver->states[0].enter = handle_idle_enter;
  driver->states[0].flags = CPUIDLE_FLAG_NONE;
  driver->state_count     = 1;
  try_module_get(driver->owner);
  cpuidle_resume_and_unlock();
  return 0;
}
static void unhijack_cpuidle(void) {
  struct cpuidle_driver* driver = cpuidle_get_driver();
  if (!driver) {
    return;
  }

  cpuidle_pause_and_lock();
  driver->states[0]   = original_state;
  driver->state_count = original_state_count;
  module_put(driver->owner);
  cpuidle_resume_and_unlock();
}

static void handle_sched_switch(void* data, bool preempt,
    struct task_struct* prev, struct task_struct* next) {
  struct KmoduleContextPerCpu* ctx = this_cpu_ptr(&cpu_local_ctx);
  if (next != ctx->running_task) {
    return;
  }
  const int cpu = get_cpu();
  // FIXME: this does not always work; should be moved to client?
  WRITE_ONCE(shm[cpu].task_started_at, rdtsc());
  put_cpu();
}
static struct tracepoint* sched_switch_tp;
static void handle_for_each_tracepoint(struct tracepoint* tp, void* data) {
  if (strncmp(tp->name, "sched_switch", strlen("sched_switch")) == 0) {
    sched_switch_tp = tp;
  }
}
static void regist_sched_switch_tracepoint(void) {
  for_each_kernel_tracepoint(handle_for_each_tracepoint, NULL);
  if (sched_switch_tp) {
    tracepoint_probe_register(
        sched_switch_tp, (void*)handle_sched_switch, NULL);
  }
}
static void unregist_sched_switch_tracepoint(void) {
  if (sched_switch_tp) {
    tracepoint_probe_unregister(
        sched_switch_tp, (void*)handle_sched_switch, NULL);
  }
}

static struct cdev cdev;
static int         module_entry(void) {
  dev_t devno;
  int   ret = alloc_chrdev_region(&devno, 0, 1, "kmodule");
  if (ret) {
    LOG_ERROR("Failed to register character device region (%d)\n", ret);
    return -1;
  }
  cdev_init(&cdev, &ops);
  ret = cdev_add(&cdev, devno, 1);
  if (ret) {
    LOG_ERROR("Failed to add character device (%d)\n", ret);
    return -1;
  }

  shm =
      vmalloc_user(sizeof(struct SharedContextPerCpu) * KMODULE_SHM_ARRAY_LEN);
  if (!shm) {
    LOG_ERROR("Failed to allocate shared memory\n");
    return -ENOMEM;
  }
  memset(shm, 0, sizeof(struct SharedContextPerCpu) * KMODULE_SHM_ARRAY_LEN);

  if (hijack_cpuidle() != 0) {
    LOG_ERROR("Failed to hijack cpuidle\n");
    return -1;
  }

  {
    int cpu;
    for_each_online_cpu(cpu) {
      struct KmoduleContextPerCpu* ctx = per_cpu_ptr(&cpu_local_ctx, cpu);
      spin_lock_init(&ctx->execute_task_lock);
    }
  }

  regist_sched_switch_tracepoint();

  LOG_INFO("Module initialized successfully\n");
  return 0;
}
static void module_cleanup(void) {
  cdev_del(&cdev);
  unregister_chrdev_region(cdev.dev, 1);
  if (shm) {
    vfree(shm);
  }
  unregist_sched_switch_tracepoint();
  int cpu;
  for_each_online_cpu(cpu) {
    struct KmoduleContextPerCpu* p = per_cpu_ptr(&cpu_local_ctx, cpu);
    if (p->running_task) {
      put_task_struct(p->running_task);
    }
  }
  unhijack_cpuidle();
  LOG_INFO("Module exited successfully\n");
}

module_init(module_entry);
module_exit(module_cleanup);

MODULE_LICENSE("Dual MIT/GPL");
