#ifndef ONETOOL_LIBS_SRAPI_VM_THREADPOOL_H
#define ONETOOL_LIBS_SRAPI_VM_THREADPOOL_H

#include <stdint.h>

typedef void (*srapi_vm_tile_fn_t)(void *user, uint32_t tile_index);

void srapi_vm_threadpool_dispatch(uint32_t tile_count, srapi_vm_tile_fn_t fn, void *user);

#endif
