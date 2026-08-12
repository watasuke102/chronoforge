#include "scheduler.h"

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <list>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#include "kmodule.h"

#define READ_ONCE(a)         (*(const volatile typeof(a)*)&(a))
#define WRITE_ONCE(dst, val) ((*(volatile typeof(dst)*)&(dst)) = (val))

#define LOG_ERROR(fmt, ...) printf("\033[31m[error]\033[0m " fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  printf("\033[33m[warn ]\033[0m " fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  printf("\033[32m[info ]\033[0m " fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) printf("\033[34m[debug]\033[0m " fmt, ##__VA_ARGS__)

// scheduling target that corresponding a client
class Task {
 public:
  Task(pid_t id, int fd) : task_id_(id), socket_fd_(fd) {
  }
  Task(const Task&)            = delete;
  Task& operator=(const Task&) = delete;
  Task(Task&& other) noexcept
      : task_id_(other.task_id_), socket_fd_(other.socket_fd_) {
    other.task_id_   = 0;
    other.socket_fd_ = 0;
  }
  Task& operator=(Task&& other) noexcept {
    if (this == &other) {
      return *this;
    }
    if (socket_fd_ != 0) {
      close(socket_fd_);
    }
    task_id_         = other.task_id_;
    socket_fd_       = other.socket_fd_;
    other.task_id_   = 0;
    other.socket_fd_ = 0;
    return *this;
  }
  ~Task() {
    if (socket_fd_ != 0) {
      close(socket_fd_);
    }
  }

  pid_t task_id() const {
    return task_id_;
  }
  int socket_fd() const {
    return socket_fd_;
  }

 private:
  pid_t task_id_;
  int   socket_fd_;
};

struct CoreState {
  std::optional<Task> running_task;
  std::optional<Task> execution_requested_task;
};

// Context that shared between scheduling thread and socket thread
struct Ctx {
  int                  kmodule_fd;
  int                  socket_fd;
  int                  epoll_fd;
  uint32_t             cycles_per_us;
  std::thread          socket_thread;
  SharedContextPerCpu* shm;

  std::list<Task>                              runqueue;
  std::array<CoreState, KMODULE_SHM_ARRAY_LEN> core_states;
  std::unordered_map<pid_t, Task>              park_pending_tasks;
  std::mutex                                   mutex;
};

namespace {
constexpr uint32_t                 EPOLL_IDENTIFIER = 0x02c0'ffee;
std::bitset<KMODULE_SHM_ARRAY_LEN> active_cpus;

uint64_t rdtsc() {
  uint32_t l, h;
  asm volatile("rdtsc" : "=a"(l), "=d"(h));
  return ((uint64_t)h << 32) | l;
}

void add_new_task(Ctx* ctx, int fd) {
  ucred     optval;
  socklen_t len = sizeof(optval);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &optval, &len) == -1) {
    LOG_ERROR("Failed to get socket credentials (%s)\n", strerror(errno));
    close(fd);
    return;
  }

  struct epoll_event ev;
  ev.events  = EPOLLIN;
  ev.data.fd = fd;
  if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
    LOG_ERROR("Failed to add socket to epoll: %s", strerror(errno));
    close(fd);
    return;
  }

  LOG_DEBUG("new task: pid=%d\n", optval.pid);
  {
    std::lock_guard<std::mutex> lock(ctx->mutex);
    ctx->runqueue.emplace_back(optval.pid, fd);
  }
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
      LOG_ERROR("epoll_wait error: %s", strerror(errno));
      break;
    }
    LOG_DEBUG("epoll ev = %#x, data: %#x\n", ev.events, ev.data.u32);
    if (ev.data.u32 == EPOLL_IDENTIFIER) {
      const int fd = accept(ctx->socket_fd, nullptr, nullptr);
      if (fd < 0) {
        LOG_ERROR("Failed to accept connection: %s", strerror(errno));
        continue;
      }
      add_new_task(ctx, fd);
      continue;
    }
    if ((ev.events & EPOLLHUP) == 0) {
      continue;
    }
    // task finished
    // task is expected to be in running_tasks
    std::lock_guard<std::mutex> lock(ctx->mutex);
    auto it = std::find_if(ctx->core_states.begin(), ctx->core_states.end(),
        [ev](const CoreState& s) {
          return s.running_task.has_value() &&
                 s.running_task->task_id() == ev.data.u64;
        });
    if (it == ctx->core_states.end()) {
      LOG_ERROR(
          "Failed to find finished task %d from running_tasks\n", ev.data.fd);
      auto rq_task = std::find_if(
          ctx->runqueue.begin(), ctx->runqueue.end(), [ev](const Task& t) {
            return t.socket_fd() == ev.data.fd;
          });
      if (rq_task != ctx->runqueue.end()) {
        LOG_DEBUG("(debug) task %ld is on runqueue\n", ev.data.u64);
      }
      continue;
    }
    LOG_DEBUG("task finished: fd=%d, task_id=%d\n", ev.data.fd,
        it->running_task->task_id());
    it->running_task.reset();
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, ev.data.fd, nullptr) < 0) {
      LOG_ERROR(
          "[error] Failed to remove socket from epoll: %s", strerror(errno));
    }
    close(ev.data.fd);
  }
}

/// Take a task from runqueue and request kmodule to execute the next task
/// If there is no task in the runqueue, do nothing
void enqueue_execute_next_task(Ctx* ctx, int cpu) {
  std::lock_guard<std::mutex> lock(ctx->mutex);
  if (ctx->runqueue.empty()) {
    return;
  }
  auto&& next_task = std::move(ctx->runqueue.front());
  ctx->runqueue.pop_front();
  WRITE_ONCE(ctx->shm[cpu].next_task_id, next_task.task_id());
  LOG_INFO(
      "execution enqueued for task %d at cpu %d\n", next_task.task_id(), cpu);
  ctx->core_states[cpu].execution_requested_task.emplace(std::move(next_task));
}
/// Request kmodule to park the currently running task on the specified CPU
void enqueue_park_task(Ctx* ctx, int cpu) {
  std::lock_guard<std::mutex> lock(ctx->mutex);
  if (!ctx->core_states[cpu].running_task.has_value()) {
    return;
  }
  WRITE_ONCE(ctx->shm[cpu].is_park_requested, true);
  auto pid = ctx->core_states[cpu].running_task->task_id();
  LOG_INFO("park enqueued for task %d at cpu %d\n", pid, cpu);
  ctx->park_pending_tasks.emplace(
      pid, std::move(ctx->core_states[cpu].running_task.value()));
  ctx->core_states[cpu].running_task.reset();
}

void finalize_enqueued_task(Ctx* ctx, int cpu) {
  std::lock_guard<std::mutex> lock(ctx->mutex);
  auto&                       core_state = ctx->core_states[cpu];

  if (core_state.execution_requested_task.has_value()) {
    auto&       pending           = core_state.execution_requested_task;
    const pid_t requested_task_id = pending->task_id();
    const int   requested_fd      = pending->socket_fd();
    const pid_t next_task_id      = READ_ONCE(ctx->shm[cpu].next_task_id);
    const pid_t running_task_id   = READ_ONCE(ctx->shm[cpu].running_task_id);

    // execution request was confirmed
    if (next_task_id == 0) {
      if (running_task_id == requested_task_id) {
        // the task was successfully executed
        core_state.running_task = std::move(pending);
        pending.reset();
        LOG_INFO(
            "execution confirmed: task %d on cpu %d\n", requested_task_id, cpu);
      } else {
        // kmodule could not find the target PID, so discard it
        pending.reset();
        if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, requested_fd, nullptr) <
                0 &&
            errno != ENOENT && errno != EBADF) {
          LOG_ERROR("Failed to remove discarded task from epoll: %s",
              strerror(errno));
        }
        LOG_WARN("discarded task %d on cpu %d because PID was not found\n",
            requested_task_id, cpu);
      }
      // always stop finalization when next_task_id was cleared
      return;
    }

    if (next_task_id == requested_task_id) {
      // kmodule kept next_task_id unchanged, so execution failed for a reason
      // other than missing PID. Return the task to runqueue and retry later
      WRITE_ONCE(ctx->shm[cpu].next_task_id, 0);
      ctx->runqueue.emplace_back(std::move(*pending));
      pending.reset();
      LOG_WARN("execution failed for task %d on cpu %d; requeued\n",
          requested_task_id, cpu);
      return;
    }

    // Unexpected failure; requeue the task
    ctx->runqueue.emplace_back(std::move(*pending));
    pending.reset();
    LOG_WARN(
        "inconsistent pending state on cpu %d (pending=%d, next=%d); "
        "requeued\n",
        cpu, requested_task_id, next_task_id);
  }
}

void finalize_parked_tasks(Ctx* ctx) {
  for (int cpu = 0; cpu < KMODULE_SHM_ARRAY_LEN; cpu++) {
    if (!active_cpus.test(cpu)) {
      continue;
    }
    pid_t parked_pid = READ_ONCE(ctx->shm[cpu].parked_task_id);
    if (parked_pid == 0) {
      continue;
    }
    LOG_INFO("Recognized: task %d is parked by kmodule on cpu %d\n", parked_pid,
        cpu);
    // Move the task from park_pending_tasks back to runqueue
    std::lock_guard<std::mutex> lock(ctx->mutex);
    auto                        it = ctx->park_pending_tasks.find(parked_pid);
    if (it != ctx->park_pending_tasks.end()) {
      ctx->runqueue.emplace_back(std::move(it->second));
      ctx->park_pending_tasks.erase(it);
    } else {
      LOG_WARN("Parked task %d not found in park_pending_tasks\n", parked_pid);
    }
    // Clear the notification flag in shm
    WRITE_ONCE(ctx->shm[cpu].parked_task_id, 0);
  }
}

void schedule(Ctx* ctx) {
  constexpr uint32_t TASK_QUANTUM_US = 10;

  const auto now = rdtsc();
  for (int i = 0; i < KMODULE_SHM_ARRAY_LEN; i++) {
    if (!active_cpus.test(i)) {
      continue;
    }
    if (READ_ONCE(ctx->shm[i].is_park_requested)) {
      return;
    }
    uint32_t runqueue_size = 0;
    {
      std::lock_guard<std::mutex> lock(ctx->mutex);
      runqueue_size = ctx->runqueue.size();
    }
    if (READ_ONCE(ctx->shm[i].running_task_id) != 0) {
      // task is running and park is not requested; check time slice
      const auto task_started_at = READ_ONCE(ctx->shm[i].task_started_at);
      // time slice exceeded
      if (runqueue_size > 0 &&
          now - task_started_at > ctx->cycles_per_us * TASK_QUANTUM_US) {
        // request to park the task and schedule the next task
        enqueue_park_task(ctx, i);
      }
    } else if (runqueue_size > 0) {
      // cpu is idle; schedule the next task
      enqueue_execute_next_task(ctx, i);
    }
  }
  // dispatch enqueued requests
  ioctl(ctx->kmodule_fd, KMODULE_IOCTL_INTR);

  // Move tasks from pending state based on kmodule execution result.
  for (int i = 0; i < KMODULE_SHM_ARRAY_LEN; i++) {
    if (!active_cpus.test(i)) {
      continue;
    }
    finalize_enqueued_task(ctx, i);
  }

  // Check and finalize parked tasks
  finalize_parked_tasks(ctx);
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
    LOG_INFO("CPU frequency: %.2u cycles/us\n", ctx->cycles_per_us);
  }

  // connect to kmodule
  ctx->kmodule_fd = open("/dev/kmodule", O_RDWR);
  if (ctx->kmodule_fd < 0) {
    LOG_ERROR("Failed to open /dev/kmodule (%s)\n", strerror(errno));
    goto delete_ctx;
  }
  ctx->shm = static_cast<SharedContextPerCpu*>(
      mmap(NULL, sizeof(SharedContextPerCpu) * KMODULE_SHM_ARRAY_LEN,
          PROT_READ | PROT_WRITE, MAP_SHARED, ctx->kmodule_fd, 0));
  if (ctx->shm == MAP_FAILED) {
    LOG_ERROR("Failed to mmap /dev/kmodule (%s)\n", strerror(errno));
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
    LOG_ERROR("Failed to create socket (%s)\n", strerror(errno));
    goto munmap_shm;
  }
  if (bind(ctx->socket_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) <
      0) {
    LOG_ERROR("Failed to bind socket (%s)\n", strerror(errno));
    goto close_socket;
  }
  if (listen(ctx->socket_fd, 5) < 0) {
    LOG_ERROR("Failed to listen on socket (%s)\n", strerror(errno));
    goto close_socket;
  }

  ctx->epoll_fd = epoll_create1(0);
  if (ctx->epoll_fd < 0) {
    LOG_ERROR("Failed to create epoll instance (%s)\n", strerror(errno));
    goto close_socket;
  }

  {
    // onine list (e.g. "0-3,8")
    std::ifstream online_cpu_file("/sys/devices/system/cpu/online");
    if (!online_cpu_file) {
      LOG_ERROR("Failed to open /sys/devices/system/cpu/online\n");
      goto close_epoll_fd;
    }

    std::string online_cpu_list;
    if (!std::getline(online_cpu_file, online_cpu_list)) {
      LOG_ERROR("Failed to read /sys/devices/system/cpu/online\n");
      goto close_epoll_fd;
    }

    std::stringstream cpu_tokens(online_cpu_list);
    std::string       token;
    active_cpus.reset();
    const auto parse_uint64_strict = [](const std::string& s,
                                         uint64_t*         out) -> bool {
      if (s.empty()) {
        return false;
      }
      size_t pos = 0;
      try {
        const uint64_t v = std::stoull(s, &pos, 10);
        if (pos != s.size()) {
          return false;
        }
        *out = v;
        return true;
      } catch (...) {
        return false;
      }
    };

    while (std::getline(cpu_tokens, token, ',')) {
      token.erase(std::remove_if(token.begin(), token.end(),
                      [](unsigned char c) {
                        return std::isspace(c) != 0;
                      }),
          token.end());
      if (token.empty()) {
        continue;
      }

      const auto dash_pos  = token.find('-');
      uint64_t   first_cpu = 0;
      uint64_t   last_cpu  = 0;
      const bool has_range = (dash_pos != std::string::npos);
      if (!has_range) {
        if (!parse_uint64_strict(token, &first_cpu)) {
          LOG_ERROR("Failed to parse cpu list: %s\n", online_cpu_list.c_str());
          goto close_epoll_fd;
        }
        last_cpu = first_cpu;
      } else {
        const auto invalid_extra_dash = token.find('-', dash_pos + 1);
        if (invalid_extra_dash != std::string::npos || dash_pos == 0 ||
            dash_pos + 1 >= token.size()) {
          LOG_ERROR("Failed to parse cpu list: %s\n", online_cpu_list.c_str());
          goto close_epoll_fd;
        }
        const std::string first_str = token.substr(0, dash_pos);
        const std::string last_str  = token.substr(dash_pos + 1);
        if (!parse_uint64_strict(first_str, &first_cpu) ||
            !parse_uint64_strict(last_str, &last_cpu)) {
          LOG_ERROR("Failed to parse cpu list: %s\n", online_cpu_list.c_str());
          goto close_epoll_fd;
        }
        if (last_cpu < first_cpu) {
          LOG_ERROR("Failed to parse cpu range: %s\n", online_cpu_list.c_str());
          goto close_epoll_fd;
        }
      }

      if (last_cpu >= KMODULE_SHM_ARRAY_LEN) {
        LOG_ERROR("cpu index out of range: %llu (max=%d)\n",
            static_cast<unsigned long long>(last_cpu),
            KMODULE_SHM_ARRAY_LEN - 1);
        goto close_epoll_fd;
      }
      for (uint64_t cpu = first_cpu; cpu <= last_cpu; ++cpu) {
        active_cpus.set(static_cast<size_t>(cpu));
      }
    }

    if (active_cpus.none()) {
      LOG_ERROR("No online cpu found in /sys/devices/system/cpu/online\n");
      goto close_epoll_fd;
    }

    LOG_INFO("active cpu bitmask=%s (total: %lu)\n",
        active_cpus.to_string().c_str(), active_cpus.count());
  }

  struct epoll_event ev;
  ev.events   = EPOLLIN;
  ev.data.u32 = EPOLL_IDENTIFIER;
  if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->socket_fd, &ev) < 0) {
    LOG_ERROR("Failed to add socket to epoll (%s)\n", strerror(errno));
    goto close_epoll_fd;
  }

  ctx->socket_thread = std::thread([&ctx] {
    poll(ctx);
  });

  // start scheduling
  LOG_INFO("Scheduler started\n");
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
  munmap(ctx->shm, sizeof(SharedContextPerCpu) * KMODULE_SHM_ARRAY_LEN);
close_kmodule_fd:
  close(ctx->kmodule_fd);
delete_ctx:
  delete ctx;

  return exit_status;
}
