#pragma once

#ifdef __cplusplus
extern "C" {
#endif

int runtime_start(int (*entrypoint)(void*), void* arg);

#ifdef __cplusplus
}
#endif
