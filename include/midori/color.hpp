#pragma once

#include <array>
#include <cstdint>
#include <utility>

namespace Midori {

enum struct PixelFormat : std::uint8_t {
    RGBA8,
    R8,
    RGBA16,
    R16,
    RGBA16_HDR,
    R16_HDR,
    Count_,
};

constexpr std::array<std::uint8_t, std::to_underlying(PixelFormat::Count_)> kPixelFormatTexelBytes{4, 1, 8, 2, 8, 2};

[[nodiscard]] constexpr std::uint32_t PixelFormatTexelBytes(PixelFormat format) noexcept {
    return kPixelFormatTexelBytes[std::to_underlying(format)];
}

enum struct BlendMode : std::uint8_t {
    Normal,
    Multiply,
    Add,
    Screen,
    NormalOkLab, // Normal with the colors mixed in OkLab
    Erase,       // each of the source's channels is how much of that channel below goes
    Min,
    Max,
    Count_,
};

namespace Color {

// Plain aggregates; convert with the free functions below. Linear is linear-light with sRGB primaries,
// OkLab is Björn Ottosson's perceptual space. Alpha is carried through unchanged.

struct Srgb {
    float r{};
    float g{};
    float b{};
    float a{};
};

struct OkLab {
    float L{};
    float a{};
    float b{};
    float alpha{};
};

struct Linear {
    float r{};
    float g{};
    float b{};
    float a{};
};

[[nodiscard]] Linear ToLinear(Srgb srgb) noexcept;
[[nodiscard]] Linear ToLinear(OkLab okLab) noexcept;
[[nodiscard]] Srgb ToSrgb(Linear linear) noexcept;
[[nodiscard]] Srgb ToSrgb(OkLab okLab) noexcept;
[[nodiscard]] OkLab ToOkLab(Linear linear) noexcept;
[[nodiscard]] OkLab ToOkLab(Srgb srgb) noexcept;

} // namespace Color

} // namespace Midori
