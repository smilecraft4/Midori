#include "win32_platform.hpp"

#include "midori/base.hpp"
#include "midori/log.hpp"

#include <bit>
#include <cstdlib>
#include <malloc.h>
#include <new>

#include <tracy/Tracy.hpp>

namespace {

// Without a debugger, DebugBreak() would crash through an unhandled exception instead of aborting.
[[noreturn]] void BreakAndAbort() noexcept {
    if (IsDebuggerPresent()) {
        DebugBreak();
    }
    std::abort();
}

} // namespace

namespace Midori {

void AssertAbort() noexcept {
    spdlog::default_logger_raw()->flush();
    BreakAndAbort();
}

// Running out of memory is a system error: log and abort rather than hand null to the caller.
// Zero sizes are rejected because _aligned_realloc(ptr, 0) frees the block and returns null, which
// would read as out of memory.
void* Heap::Allocate(std::size_t size, std::size_t alignment) noexcept {
    MIDORI_ASSERT(size != 0, "zero-sized allocation");
    MIDORI_ASSERT(std::has_single_bit(alignment), "alignment {} is not a power of two", alignment);
    void* ptr = _aligned_malloc(size, alignment);
    MIDORI_ASSERT_ALWAYS(ptr, "out of memory allocating {} bytes (alignment {})", size, alignment);
    TracyAlloc(ptr, size);
    return ptr;
}

void Heap::Deallocate(void* ptr) noexcept {
    if (ptr) {
        TracyFree(ptr);
    }
    _aligned_free(ptr);
}

// _aligned_realloc can grow or shrink the block in place and only copies when it has to move it.
void* Heap::Reallocate(void* ptr, std::size_t newSize, std::size_t alignment) noexcept {
    MIDORI_ASSERT(newSize != 0, "zero-sized reallocation; use Deallocate");
    void* newPtr = _aligned_realloc(ptr, newSize, alignment);
    MIDORI_ASSERT_ALWAYS(newPtr, "out of memory reallocating to {} bytes (alignment {})", newSize, alignment);
    // Reported after the call, but free before alloc: the block may not have moved.
    if (ptr) {
        TracyFree(ptr);
    }
    TracyAlloc(newPtr, newSize);
    return newPtr;
}

} // namespace Midori

// Global new/delete, replaced so every C++ allocation (the STL's and spdlog's included) shows up in
// Tracy's memory view. Only the base forms are defined: the standard library's array, sized and
// nothrow-delete forms forward to them. The nothrow new forms are defined too, because the default
// ones call the throwing form, which aborts here instead of throwing. Aligned forms must use
// _aligned_malloc/_aligned_free: free() cannot release an aligned block.
//
// Performance: while a profiler is connected, every TracyAlloc/TracyFree (here and in Heap) takes
// Tracy's serial lock, so all threads' allocations are serialized through one mutex. If a capture
// shows freezes, stalls or contention around allocation-heavy code, look here first: compare a run
// with -DTRACY_ENABLE=OFF, or stop tracking the global new/delete and keep only Heap.
//
// Exit: the plain TracyAlloc/TracyFree assume Tracy's profiler is alive. Frees that run during
// static destruction, after the profiler has shut down, can touch a dead profiler. If the app
// crashes on exit inside Tracy, switch these to TracySecureAlloc/TracySecureFree, which check that
// the profiler is still available first (at the cost of one extra check per call).

namespace {

void* NewUnaligned(std::size_t size) noexcept {
    // malloc(0) may return null; new must return a unique pointer.
    void* ptr = std::malloc(size ? size : 1);
    if (ptr) {
        TracyAlloc(ptr, size);
    }
    return ptr;
}

void* NewAligned(std::size_t size, std::align_val_t alignment) noexcept {
    void* ptr = _aligned_malloc(size ? size : 1, static_cast<std::size_t>(alignment));
    if (ptr) {
        TracyAlloc(ptr, size);
    }
    return ptr;
}

// Out of memory inside operator new: logging through spdlog could allocate and recurse, so report
// straight to the debugger output.
void* NewOrAbort(void* ptr) noexcept {
    if (!ptr) [[unlikely]] {
        OutputDebugStringA("Midori: out of memory in operator new\n");
        BreakAndAbort();
    }
    return ptr;
}

} // namespace

void* operator new(std::size_t size) {
    return NewOrAbort(NewUnaligned(size));
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return NewOrAbort(NewAligned(size, alignment));
}

void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return NewUnaligned(size);
}
void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return NewUnaligned(size);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t& /*tag*/) noexcept {
    return NewAligned(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& /*tag*/) noexcept {
    return NewAligned(size, alignment);
}

void operator delete(void* ptr) noexcept {
    if (ptr) {
        TracyFree(ptr);
        std::free(ptr);
    }
}
void operator delete(void* ptr, std::align_val_t /*alignment*/) noexcept {
    if (ptr) {
        TracyFree(ptr);
        _aligned_free(ptr);
    }
}
