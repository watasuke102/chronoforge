#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "kmodule.h"
#include "scheduler.h"

namespace {
int kmodule_fd = -1;

// handle SIGUSR1 (park request)
void handle_sigusr1(int signum) {
  std::putchar('.');  // debug
  ioctl(kmodule_fd, KMODULE_IOCTL_PARK);
}
}  // namespace

extern "C" int runtime_start(int (*entrypoint)(void*), void* arg) {
  // setup signal handler
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handle_sigusr1;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  if (sigaction(SIGUSR1, &sa, nullptr) == -1) {
    std::perror("Failed to set up SIGUSR2 handler");
    return EXIT_FAILURE;
  }

  // connect with scheduler
  sockaddr_un addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, SOCKET_PATH, sizeof(SOCKET_PATH));
  int scheduler_sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (scheduler_sock_fd < 0) {
    std::perror("Failed to create socket");
    return EXIT_FAILURE;
  }
  std::puts("[debug] connect()");
  if (connect(scheduler_sock_fd, reinterpret_cast<sockaddr*>(&addr),
          sizeof(addr)) < 0) {
    std::perror("Failed to connect to scheduler");
    close(scheduler_sock_fd);
    return EXIT_FAILURE;
  }

  // connect with kmodule
  kmodule_fd = open("/dev/kmodule", O_RDWR);
  std::puts("[debug] open()");
  if (kmodule_fd < 0) {
    std::perror("Failed to open /dev/kmodule");
    close(scheduler_sock_fd);
    return EXIT_FAILURE;
  }
  std::puts("[debug] ioctl() start");
  ioctl(kmodule_fd, KMODULE_IOCTL_START);
  std::puts("[debug] ioctl() end");

  // start client task
  const int ret = entrypoint(arg);

  if (kmodule_fd >= 0) {
    close(kmodule_fd);
  }

  return ret;
}
