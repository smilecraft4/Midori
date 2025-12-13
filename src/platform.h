#ifndef MIDORI_PLATFORM_H
#define MIDORI_PLATFORM_H

#include <cstdint>
#include <source_location>
#include <string>

namespace Platform {
enum class Result : uint8_t {
    Success,
    Unknown,
};

Result SetWindowPos(int32_t x, int32_t y);
Result SetWindowSize(int32_t width, int32_t height);
Result SetWindowTitle(const std::string& title);
Result SetWindowFullscreened(bool fullscreen);
Result SetWindowMaximized(bool maximize);
Result WindowRender();

template <typename... Args>
void LogTrace(const std::string& channel, const std::string& fmt, Args&&... args);
template <typename... Args>
void LogError(const std::string& channel, const std::string& fmt, Args&&... args);
template <typename... Args>
void LogCritical(const std::string& channel, const std::string& fmt, Args&&... args);

void Assert(bool condition, std::source_location loc = std::source_location::current());

void MessageBoxInfo(const std::string& title, const std::string& msg);
void MessageBoxWarn(const std::string& title, const std::string& msg);
void MessageBoxError(const std::string& title, const std::string& msg);

int64_t GetTimeNS();

extern "C" {
void* Malloc(size_t size);
void* Calloc(size_t nmemb, size_t size);
void* Realloc(void* mem, size_t size);
void  Free(void* mem);
};

} // namespace Platform

#endif