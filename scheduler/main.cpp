#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "kmodule.h"

int main(void) {
  const int fd = open("/dev/kmodule", O_RDWR);
  if (fd < 0) {
    printf("Failed to open /dev/kmodule (%d)\n", fd);
    return -1;
  }
  const int ret = ioctl(fd, KMODULE_IOCTL_SETUP_SCHEDULER, nullptr);
  printf("ioctl() returned %d\n", ret);
  close(fd);
  return 0;
}
