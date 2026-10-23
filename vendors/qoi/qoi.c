#include <stddef.h>

// Every stream comes from the calling thread's buffer (canvas.cpp) and lives until that thread's next call.
void* MidoriQoiBuffer(size_t size);
#define QOI_MALLOC(sz) MidoriQoiBuffer(sz)
#define QOI_FREE(p) ((void)(p))

#define QOI_NO_STDIO
#define QOI_IMPLEMENTATION
#include "qoi.h"
