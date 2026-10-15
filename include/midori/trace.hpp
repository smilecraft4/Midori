#pragma once

// Tracy profiler markers. With TRACY_ENABLE undefined (configure with -DTRACY_ENABLE=OFF) Tracy's
// macros expand to nothing and the client is not compiled in.
#include <tracy/Tracy.hpp>

// Zone covering the rest of the enclosing scope. `name` must be a string literal.
#define MIDORI_TRACE_SCOPE(name) ZoneScopedN(name)

// Allocation events for the memory view. Every Free must match an earlier Alloc of the same pointer.
#define MIDORI_TRACE_ALLOC(ptr, size) TracyAlloc(ptr, size)
#define MIDORI_TRACE_FREE(ptr)        TracyFree(ptr)

// tracy::SetThreadName is declared either way but only defined when the client is compiled in.
#if defined(TRACY_ENABLE)
#define MIDORI_TRACE_SET_THREAD_NAME(name) ::tracy::SetThreadName(name)
#else
#define MIDORI_TRACE_SET_THREAD_NAME(name) ((void)0)
#endif
