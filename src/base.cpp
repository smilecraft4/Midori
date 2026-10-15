#include "midori/base.hpp"
#include "midori/log.hpp"

#include <memory>

#include <spdlog/sinks/msvc_sink.h>

// AssertAbort and Heap are platform code: see win32/win32_base.cpp.

namespace Midori {

Result<void> LogInit() noexcept {
    auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("midori", std::move(sink));
    logger->set_level(static_cast<spdlog::level::level_enum>(SPDLOG_ACTIVE_LEVEL));
    spdlog::set_default_logger(std::move(logger));
    return {};
}

void LogTerminate() noexcept {
    spdlog::shutdown();
}

} // namespace Midori
