#pragma once

// Logging and asserts. The only header that includes spdlog; the codebase formats through spdlog and
// never calls fmt directly. SPDLOG_ACTIVE_LEVEL is set per configuration by CMake, so it agrees with
// the precompiled header.

#include "midori/base.hpp"

#include <spdlog/spdlog.h>

#include <utility>

#if defined(_MSC_VER) && !defined(__clang__)
#define MIDORI_COLD __declspec(noinline)
#else
#define MIDORI_COLD [[gnu::cold, gnu::noinline]]
#endif

namespace Midori {

[[nodiscard]] Result<void> LogInit() noexcept;
void LogTerminate() noexcept;

// Flushes the log, breaks into the debugger when one is attached, then aborts.
[[noreturn]] MIDORI_COLD void AssertAbort() noexcept;

// Out of line and cold so each assert costs one compare and branch at the call site. spdlog formats
// short messages (up to 250 bytes) on the stack, so an out-of-memory assert does not allocate.
template<typename... Args>
[[noreturn]] MIDORI_COLD void AssertFail(const char* condition, const char* file, int line,
                                         spdlog::format_string_t<Args...> format, Args&&... args) noexcept {
    spdlog::critical("Assertion failed: {}\n  at {}:{}", condition, file, line);
    spdlog::critical(format, std::forward<Args>(args)...);
    AssertAbort();
}

} // namespace Midori

#define MIDORI_LOG_TRACE(...) SPDLOG_TRACE(__VA_ARGS__)
#define MIDORI_LOG_INFO(...)  SPDLOG_INFO(__VA_ARGS__)
#define MIDORI_LOG_WARN(...)  SPDLOG_WARN(__VA_ARGS__)
#define MIDORI_LOG_ERROR(...) SPDLOG_ERROR(__VA_ARGS__)

// MIDORI_ASSERT(cond, fmt, args...): debug-only, for checks too costly to ship.
// MIDORI_ASSERT_ALWAYS(cond, fmt, args...): every build, for cheap invariants whose violation would
// corrupt data or memory.
// The message is required and checked at compile time like spdlog/std::format; its arguments are only
// evaluated when the check fails.
#define MIDORI_ASSERT_ALWAYS(condition, ...)                                                                           \
    do {                                                                                                               \
        if (!(condition)) [[unlikely]] {                                                                               \
            ::Midori::AssertFail(#condition, __FILE__, __LINE__, __VA_ARGS__);                                         \
        }                                                                                                              \
    } while (false)

#if defined(MIDORI_DEBUG)
#define MIDORI_ASSERT(condition, ...) MIDORI_ASSERT_ALWAYS(condition, __VA_ARGS__)
#else
// Unevaluated, but still type-checked, and variables used only in the condition do not warn as unused.
#define MIDORI_ASSERT(condition, ...) ((void)sizeof(!(condition)))
#endif
