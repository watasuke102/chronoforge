#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "kmodule.h"
#include "scheduler.h"

namespace {
constexpr uint32_t EPOLL_IDENTIFIER = 0x02c0'ffee;
}

struct Ctx {
  int kmodule_fd;
  int socket_fd;
  int epoll_fd;
};

void poll(Ctx* ctx) {
  int                ret;
  struct epoll_event ev;

  while (true) {
    do {
      ret = epoll_wait(ctx->epoll_fd, &ev, 1, -1);
    } while (ret == -1 && errno == EINTR);
    if (ret != -1) {
      std::printf("epoll_wait error (ret: %d)\n", ret);
      break;
    }
    if (ev.data.u32 == EPOLL_IDENTIFIER) {
      continue;
    }
  }
}

int main(void) {
  int         exit_status = EXIT_FAILURE;
  sockaddr_un addr;
  Ctx*        ctx = new Ctx();

  ctx->kmodule_fd = open("/dev/kmodule", O_RDWR);
  if (ctx->kmodule_fd < 0) {
    std::printf("Failed to open /dev/kmodule (%s)\n", strerror(errno));
    goto delete_ctx;
  }

  std::memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path));
  assert(addr.sun_path[0] == '\0');  // Ensure it's an abstract socket
  ctx->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ctx->socket_fd < 0) {
    std::printf("Failed to create socket (%s)\n", strerror(errno));
    goto close_kmodule_fd;
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

  ioctl(ctx->kmodule_fd, KMODULE_IOCTL_SETUP_SCHEDULER, nullptr);

  poll(ctx);

  exit_status = EXIT_SUCCESS;

close_epoll_fd:
  close(ctx->epoll_fd);
close_socket:
  close(ctx->socket_fd);
close_kmodule_fd:
  close(ctx->kmodule_fd);
delete_ctx:
  delete ctx;

  return exit_status;
}
