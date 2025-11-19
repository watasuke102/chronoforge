#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "runtime.h"

namespace {
int task(void* /*arg*/) {
  std::puts("[info ] initialized");

  int      passed_sec = 0;
  timespec t_start, t_end, t_now;
  clock_gettime(CLOCK_MONOTONIC_RAW, &t_start);
  while (passed_sec < 10) {
    clock_gettime(CLOCK_MONOTONIC_RAW, &t_now);
    if ((t_now.tv_sec - t_start.tv_sec) >= (passed_sec + 1)) {
      passed_sec++;
      std::printf("[debug] waited: %d sec (real: %ld sec)\n", passed_sec,
          (t_now.tv_sec - t_start.tv_sec));
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds(100));
  }
  clock_gettime(CLOCK_MONOTONIC_RAW, &t_end);

  std::printf("[info ] elapsed time: %ld sec\n", t_end.tv_sec - t_start.tv_sec);
  return EXIT_SUCCESS;
}
}  // namespace

int main() {
  return runtime_start(task, nullptr);
}
