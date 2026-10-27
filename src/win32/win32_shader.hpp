#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Midori {

// A source file, in memory.
struct Win32ShaderSource {
    std::string_view name; // as the #include spells it
    std::string_view text;
};

struct Win32Shader {
    std::vector<std::byte> code; // empty on failure
    std::string log;             // the compiler's errors and warnings
};

// `source` as a cs_5_1 compute shader, entry CSMain, with MIDORI_RUNTIME defined, by FXC (d3dcompiler_47.dll, part of
// Windows 10 and later). Its #includes are found among `includes` only. Thread-safe.
Win32Shader Win32ComputeShaderCompile(const Win32ShaderSource& source,
                                      std::span<const Win32ShaderSource> includes) noexcept;

} // namespace Midori
