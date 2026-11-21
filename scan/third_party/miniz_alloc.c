#include "miniz_common.h"

void* miniz_def_alloc_func(void* opaque, size_t items, size_t size)
{
    (void)opaque;
    return MZ_MALLOC(items * size);
}

void miniz_def_free_func(void* opaque, void* address)
{
    (void)opaque;
    MZ_FREE(address);
}

void* miniz_def_realloc_func(void* opaque, void* address, size_t items, size_t size)
{
    (void)opaque;
    return MZ_REALLOC(address, items * size);
}

void mz_free(void* p)
{
    MZ_FREE(p);
}
