#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <mutex>
#include <thread>

#include "kmodule.h"
#include "scheduler.h"

#define READ_ONCE(a)         (*(const volatile typeof(a)*)&(a))
#define WRITE_ONCE(dst, val) ((*(volatile typeof(dst)*)&(dst)) = (val))

// scheduling target that corresponding a client
class Task {
 public:
  Task(pid_t id, int fd) : task_id_(id), socket_fd_(fd) {
  }
  ~Task() {
    if (socket_fd_ != 0) {
      close(socket_fd_);
    }
  }

  pid_t task_id() const {
    return task_id_;
  }

 private:
  pid_t task_id_;
  int   socket_fd_;
};

// Context that shared between scheduling thread and socket thread
struct Ctx {
  int                  kmodule_fd;
  int                  socket_fd;
  int                  epoll_fd;
  uint32_t             cycles_per_us;
  std::thread          socket_thread;
  SharedContextPerCpu* shm;
  std::list<Task>      runqueue, running_tasks;
  std::mutex           runqueue_mutex;
};

namespace {
constexpr uint32_t EPOLL_IDENTIFIER = 0x02c0'ffee;
constexpr uint64_t MAX_CPU          = 256;

uint64_t rdtsc() {
  uint32_t l, h;
  asm volatile("rdtsc" : "=a"(l), "=d"(h));
  return ((uint64_t)h << 32) | l;
}

// Poll incoming connections from clients
void poll(Ctx* ctx) {
  int                ret;
  struct epoll_event ev;

  while (true) {
    do {
      ret = epoll_wait(ctx->epoll_fd, &ev, 1, -1);
    } while (ret == -1 && errno == EINTR);
    if (ret == -1) {
      std::perror("epoll_wait error");
      break;
    }
    if (ev.data.u32 != EPOLL_IDENTIFIER) {
      continue;
    }
    const int fd = accept(ctx->socket_fd, nullptr, nullptr);
    ucred     optval;
    socklen_t len = sizeof(optval);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &optval, &len) == -1) {
      std::printf("Failed to get socket credentials (%s)\n", strerror(errno));
      close(fd);
      continue;
    }
    std::printf("[debug] new task: pid=%d\n", optval.pid);
    {
      std::lock_guard<std::mutex> lock(ctx->runqueue_mutex);
      ctx->runqueue.emplace_back(optval.pid, fd);
    }
  }
}

/// Take a task from runqueue and request kmodule to execute the next task
/// If there is no task in the runqueue, do nothing
void enqueue_execute_next_task(Ctx* ctx, int cpu) {
  if (ctx->runqueue.size() == 0) {
    return;
  }
  pid_t next_task_id;
  {
    std::lock_guard<std::mutex> lock(ctx->runqueue_mutex);
    next_task_id = ctx->runqueue.front().task_id();
    ctx->running_tasks.splice(
        ctx->running_tasks.end(), ctx->runqueue, ctx->runqueue.begin());
  }
  WRITE_ONCE(ctx->shm[cpu].next_task_id, next_task_id);
  std::printf("[debug] scheduled task %d on cpu %d\n", next_task_id, cpu);
}
/// Request kmodule to park the currently running task on the specified CPU
void enqueue_park_task(Ctx* ctx, int cpu) {
  const pid_t task_id = READ_ONCE(ctx->shm[cpu].running_task_id);
  {
    std::lock_guard<std::mutex> lock(ctx->runqueue_mutex);
    auto it = std::find_if(ctx->running_tasks.begin(), ctx->running_tasks.end(),
        [task_id](const Task& t) {
          return t.task_id() == task_id;
        });
    if (it == ctx->running_tasks.end()) {
      std::printf("[error] failed to find task %d to park\n", task_id);
      return;
    }
    ctx->runqueue.splice(ctx->runqueue.end(), ctx->running_tasks, it);
  }
  WRITE_ONCE(ctx->shm[cpu].is_park_requested, true);
  std::printf("[debug] request park for task %d on cpu %d\n", task_id, cpu);
}

void schedule(Ctx* ctx) {
  constexpr uint32_t TASK_QUANTUM_US = 10;

  const auto now = rdtsc();
  for (int i = 0; i < KMODULE_SHM_ARRAY_LEN; i++) {
    if (READ_ONCE(ctx->shm[i].is_busy)) {
      // task is running; check time slice
      const auto task_started_at = READ_ONCE(ctx->shm[i].task_started_at);
      std::printf("[debug] time slice for cpu %d (elapsed: %ld)\n", i,
          now - task_started_at);
      // time slice exceeded
      if (now - task_started_at > ctx->cycles_per_us * TASK_QUANTUM_US) {
        // request to park the task and schedule the next task
        enqueue_park_task(ctx, i);
        enqueue_execute_next_task(ctx, i);
      }
    } else if (ctx->runqueue.size() > 0) {
      // cpu is idle; schedule the next task
      enqueue_execute_next_task(ctx, i);
    }
  }
  // dispatch enqueued requests
  ioctl(ctx->kmodule_fd, KMODULE_IOCTL_INTR);
}
}  // namespace

int main(void) {
  int         exit_status = EXIT_FAILURE;
  sockaddr_un addr;
  Ctx*        ctx = new Ctx();

  {  // estimate CPU frequency
    timespec t_start, t_end;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_start);
    const auto start = rdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_end);
    const auto     end = rdtsc();
    const uint64_t ns  = ((t_end.tv_sec - t_start.tv_sec) * 1E9) +
                        (t_end.tv_nsec - t_start.tv_nsec);
    const double secs  = static_cast<double>(ns) / 1000.0;
    ctx->cycles_per_us = (end - start) / secs;
    std::printf("[info] CPU frequency: %.2u cycles/us\n", ctx->cycles_per_us);
  }

  // connect to kmodule
  ctx->kmodule_fd = open("/dev/kmodule", O_RDWR);
  if (ctx->kmodule_fd < 0) {
    std::printf("Failed to open /dev/kmodule (%s)\n", strerror(errno));
    goto delete_ctx;
  }
  ctx->shm = static_cast<SharedContextPerCpu*>(
      mmap(NULL, sizeof(SharedContextPerCpu) * KMODULE_SHM_ARRAY_LEN,
          PROT_READ | PROT_WRITE, MAP_SHARED, ctx->kmodule_fd, 0));
  if (ctx->shm == MAP_FAILED) {
    std::printf("Failed to mmap /dev/kmodule (%s)\n", strerror(errno));
    goto close_kmodule_fd;
  }
  std::memset(ctx->shm, 0, sizeof(SharedContextPerCpu) * KMODULE_SHM_ARRAY_LEN);

  // create socket for clients
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, SOCKET_PATH, sizeof(SOCKET_PATH));
  assert(addr.sun_path[0] == '\0');  // Ensure it's an abstract socket
  ctx->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ctx->socket_fd < 0) {
    std::printf("Failed to create socket (%s)\n", strerror(errno));
    goto munmap_shm;
  }
  if (bind(ctx->socket_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
      0) {
    std::printf("Failed to bind socket (%s)\n", strerror(errno));
    goto close_socket;
  }
  if (listen(ctx->socket_fd, 5) < 0) {
    std::printf("Failed to listen on socket (%s)\n", strerror(errno));
    goto close_socket;
  }

  ctx->epoll_fd = epoll_create1(0);
  if (ctx->epoll_fd < 0) {
    std::printf("Failed to create epoll instance (%s)\n", strerror(errno));
    goto close_socket;
  }
  struct epoll_event ev;
  ev.events   = EPOLLIN | EPOLLERR;
  ev.data.u32 = EPOLL_IDENTIFIER;
  if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->socket_fd, &ev) < 0) {
    std::printf("Failed to add socket to epoll (%s)\n", strerror(errno));
    goto close_epoll_fd;
  }

  ctx->socket_thread = std::thread([&ctx] {
    poll(ctx);
  });

  // start scheduling
  std::puts("[info] Scheduler started");
  while (true) {
    schedule(ctx);
    std::this_thread::sleep_for(std::chrono::microseconds(10));
  }

  exit_status = EXIT_SUCCESS;

close_epoll_fd:
  close(ctx->epoll_fd);
close_socket:
  close(ctx->socket_fd);
munmap_shm:
  munmap(ctx->shm, sizeof(SharedContextPerCpu) * 64);
close_kmodule_fd:
  close(ctx->kmodule_fd);
delete_ctx:
  delete ctx;

  return exit_status;
}
