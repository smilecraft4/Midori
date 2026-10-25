#pragma once

#include <cstdint>
#include <expected>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Midori {

enum struct Error : std::uint8_t {
    Unknown,
    PlatformInitFailed,
    WindowLimitReached,
    WindowCreateFailed,
    WindowInvalid,
    GraphicsInitFailed,
    GraphicsWindowInitFailed,
    GraphicsDeviceLost,
    CanvasLimitReached,
    CanvasViewLimitReached,
    TileAtlasFull,
    CanvasOpenFailed,
    CanvasSaveFailed,
    GraphicsTransferFailed,
    TileReadFailed,
    TileCacheFailed,
    DialogFailed,
    CanvasBusy,
    LayerOpInvalid,
    PreferencesFailed,
    FontNotFound,
    KeyBindingInvalid,
    ClipboardFailed,
    ImageDecodeFailed,
};

[[nodiscard]] constexpr std::string_view ErrorMessage(Error error) noexcept {
    switch (error) {
        case Error::Unknown:
            return "unknown error";
        case Error::PlatformInitFailed:
            return "platform initialization failed";
        case Error::WindowLimitReached:
            return "window limit reached";
        case Error::WindowCreateFailed:
            return "window creation failed";
        case Error::WindowInvalid:
            return "invalid window";
        case Error::GraphicsInitFailed:
            return "graphics initialization failed";
        case Error::GraphicsWindowInitFailed:
            return "graphics window initialization failed";
        case Error::GraphicsDeviceLost:
            return "graphics device lost";
        case Error::CanvasLimitReached:
            return "canvas limit reached";
        case Error::CanvasViewLimitReached:
            return "canvas view limit reached";
        case Error::TileAtlasFull:
            return "tile atlas full";
        case Error::CanvasOpenFailed:
            return "canvas open failed";
        case Error::CanvasSaveFailed:
            return "canvas save failed";
        case Error::GraphicsTransferFailed:
            return "graphics transfer failed";
        case Error::TileReadFailed:
            return "tile read failed";
        case Error::TileCacheFailed:
            return "tile cache failed";
        case Error::DialogFailed:
            return "dialog failed";
        case Error::CanvasBusy:
            return "canvas busy merging layers";
        case Error::LayerOpInvalid:
            return "layer operation not allowed";
        case Error::PreferencesFailed:
            return "preferences not read or written";
        case Error::FontNotFound:
            return "font not found";
        case Error::KeyBindingInvalid:
            return "key binding not understood";
        case Error::ClipboardFailed:
            return "the clipboard could not be read";
        case Error::ImageDecodeFailed:
            return "image not decoded";
    }
    return "unknown error";
}

template<typename T>
using Result = std::expected<T, Error>;

template<typename E>
inline constexpr bool kEnumFlags = false;

template<typename E>
concept EnumFlags = std::is_enum_v<E> && kEnumFlags<E>;

template<EnumFlags E>
[[nodiscard]] constexpr E operator|(E a, E b) noexcept {
    return static_cast<E>(std::to_underlying(a) | std::to_underlying(b));
}

template<EnumFlags E>
[[nodiscard]] constexpr E operator&(E a, E b) noexcept {
    return static_cast<E>(std::to_underlying(a) & std::to_underlying(b));
}

template<EnumFlags E>
[[nodiscard]] constexpr E operator^(E a, E b) noexcept {
    return static_cast<E>(std::to_underlying(a) ^ std::to_underlying(b));
}

template<EnumFlags E>
[[nodiscard]] constexpr E operator~(E a) noexcept {
    return static_cast<E>(static_cast<std::underlying_type_t<E>>(~std::to_underlying(a)));
}

template<EnumFlags E>
constexpr E& operator|=(E& a, E b) noexcept {
    return a = a | b;
}

template<EnumFlags E>
constexpr E& operator&=(E& a, E b) noexcept {
    return a = a & b;
}

template<EnumFlags E>
constexpr E& operator^=(E& a, E b) noexcept {
    return a = a ^ b;
}

template<EnumFlags E>
[[nodiscard]] constexpr bool HasAllFlags(E value, E flags) noexcept {
    return (value & flags) == flags;
}

template<EnumFlags E>
[[nodiscard]] constexpr bool HasAnyFlag(E value, E flags) noexcept {
    return std::to_underlying(value & flags) != 0;
}

} // namespace Midori
