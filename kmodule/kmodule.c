#include "kmodule.h"

#include <linux/cdev.h>
#include <linux/cpuidle.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/tracepoint.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

struct LocalContextPerCpu {
  pid_t last_task_id;

  // basically managed by execute_task()
  // but finally released by module_cleanup()
  struct task_struct* running_task;  // nullable
};
DEFINE_PER_CPU(struct LocalContextPerCpu, cpu_local_ctx);

struct SharedContextPerCpu* shm;

static void execute_task(
    struct LocalContextPerCpu* ctx, pid_t task_id, int cpu_index) {
  if (ctx->running_task) {
    put_task_struct(ctx->running_task);
    ctx->running_task = NULL;
  }
  rcu_read_lock();
  struct pid* pid = find_vpid(task_id);
  if (!pid) {
    rcu_read_unlock();
    return;
  }
  ctx->running_task = pid_task(pid, PIDTYPE_PID);
  get_task_struct(ctx->running_task);
  wake_up_process(ctx->running_task);
  rcu_read_unlock();

  WRITE_ONCE(shm[cpu_index].is_busy, false);
  WRITE_ONCE(shm[cpu_index].next_task_id, 0);
}

static void start_scheduling(void) {
  __set_current_state(TASK_INTERRUPTIBLE);
  schedule();
  __set_current_state(TASK_RUNNING);
}

static void process_ipi_from_scheduler(void) {
  struct LocalContextPerCpu* ctx = this_cpu_ptr(&cpu_local_ctx);
  for (int i = 0; i < KMODULE_SHM_ARRAY_LEN; i++) {
    pid_t next_task_id = READ_ONCE(shm[i].next_task_id);
    if (ctx->running_task->pid == next_task_id) {
      return;
    }
    execute_task(ctx, next_task_id, i);
  }
}

static long module_ioctl(
    struct file* file, unsigned int cmd, unsigned long arg) {
  printk(KERN_INFO "ioctl: %d", cmd);
  switch (cmd) {
    case KMODULE_IOCTL_START:
      start_scheduling();
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
  const int                  cpu = get_cpu();
  struct LocalContextPerCpu* ctx = this_cpu_ptr(&cpu_local_ctx);

  WRITE_ONCE(shm[cpu].is_busy, false);

  if (shm[cpu].next_task_id == 0) {
    put_cpu();
    return index;
  }
  printk(KERN_INFO "next task: (%d)\n", shm[cpu].next_task_id);

  // wait until the next task is requested (up to 8us)
  pid_t latest_next_task_id;
  for (int i = 0; i < 10; i++) {
    latest_next_task_id = READ_ONCE(shm[cpu].next_task_id);
    if (latest_next_task_id != ctx->last_task_id) {
      break;
    }
    udelay(1);
  }

  if (latest_next_task_id != ctx->last_task_id) {
    execute_task(ctx, latest_next_task_id, cpu);
  }
  put_cpu();
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
  struct LocalContextPerCpu* ctx = this_cpu_ptr(&cpu_local_ctx);
  if (next != ctx->running_task) {
    return;
  }
  const int cpu = get_cpu();
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
    printk(KERN_ERR "Failed to register character device region (%d)\n", ret);
    return -1;
  }
  cdev_init(&cdev, &ops);
  ret = cdev_add(&cdev, devno, 1);
  if (ret) {
    printk(KERN_ERR "Failed to add character device (%d)\n", ret);
    return -1;
  }

  shm =
      vmalloc_user(sizeof(struct SharedContextPerCpu) * KMODULE_SHM_ARRAY_LEN);
  if (!shm) {
    printk(KERN_ERR "Failed to allocate shared memory\n");
    return -ENOMEM;
  }
  memset(shm, 0, sizeof(struct SharedContextPerCpu) * KMODULE_SHM_ARRAY_LEN);

  if (hijack_cpuidle() != 0) {
    printk(KERN_ERR "Failed to hijack cpuidle\n");
    return -1;
  }

  regist_sched_switch_tracepoint();

  printk(KERN_INFO "Module initialized successfully\n");
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
    struct LocalContextPerCpu* p = per_cpu_ptr(&cpu_local_ctx, cpu);
    if (p->running_task) {
      put_task_struct(p->running_task);
    }
  }
  unhijack_cpuidle();
  printk(KERN_INFO "Module exited successfully\n");
}

module_init(module_entry);
module_exit(module_cleanup);

MODULE_LICENSE("Dual MIT/GPL");
