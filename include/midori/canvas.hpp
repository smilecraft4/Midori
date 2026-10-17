#pragma once

#include "midori/base.hpp"
#include "midori/color.hpp"
#include "midori/platform.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

namespace Midori {

using Layer = std::uint16_t;
constexpr Layer kLayerMax = 0b111111111111 + 1;
constexpr Layer kLayerInvalid = std::numeric_limits<Layer>::max();
constexpr std::size_t kLayerNameLenMax = 64;

using Canvas = std::uint8_t;
constexpr Canvas kCanvasMax = 16;
constexpr Canvas kCanvasInvalid = std::numeric_limits<Canvas>::max();

using CanvasView = std::uint8_t;
constexpr CanvasView kCanvasViewMax = 16;
constexpr CanvasView kCanvasViewInvalid = std::numeric_limits<CanvasView>::max();

struct LayerCreateInfo {
    float opacity{1.0f};
    Layer id{kLayerInvalid}; // if `kLayerInvalid` get any available id
    Layer height{};
    Layer childsNum{};       // if `childsNum > 0` this layer is a folder
    BlendMode blendMode{};
    bool locked{};
    bool mask{};
    bool internal_{};
    bool persisted_{};
    bool dirty_{};
};

struct LayerState {
    float opacity{1.0f};
    Layer id{kLayerInvalid};
    Layer height{};
    Layer childsNum{}; // if `childsNum > 0` this layer is a folder
    BlendMode blendMode{};
    bool locked{};
    bool mask{};
    bool internal_{};
    bool persisted_{};
    bool dirty_{};
};

struct CanvasCreateInfo {
    std::string_view filename;
    PixelFormat pixelFormat{};
};

struct CanvasState {
    std::string_view filename;
    std::array<CanvasView, kCanvasViewMax> canvasViews{}; // the first `canvasViewsNum` are valid
    std::uint8_t canvasViewsNum{};
    PixelFormat pixelFormat{};
};

[[nodiscard]] Result<Canvas> CanvasCreate(CanvasCreateInfo createInfo) noexcept;
[[nodiscard]] Result<void> CanvasDestroy(Canvas canvas) noexcept;

struct Viewport {
    float posX{0.0f};
    float posY{0.0f};
    float zoomX{1.0f};
    float zoomY{1.0f};
    float rot{0.0f};
};

[[nodiscard]] Viewport ViewportMove(Viewport viewport, float amountX, float amountY) noexcept;
[[nodiscard]] Viewport ViewportZoom(Viewport viewport, float amountX, float amountY) noexcept;
[[nodiscard]] Viewport ViewportRotate(Viewport viewport, float amount) noexcept;

struct CanvasViewCreateInfo {
    Viewport viewport{};
    int x{-1};      // `-1` for automatic x position
    int y{-1};      // `-1` for automatic y position
    int width{-1};  // `-1` for automatic width
    int height{-1}; // `-1` for automatic height
    Canvas canvas{kCanvasInvalid};
    Window window{kWindowInvalid};
};

struct CanvasViewState {
    Viewport viewport;
    CanvasView id{kCanvasViewInvalid};
    Canvas canvas{kCanvasInvalid};
    Window window{kWindowInvalid};
};

[[nodiscard]] Result<CanvasView> CanvasViewCreate(CanvasViewCreateInfo createInfo) noexcept;
[[nodiscard]] Result<void> CanvasViewDestroy(CanvasView canvasView) noexcept;

struct PaintStrokePoint {
    Color::Linear color{};
    float posX{};
    float posY{};
    float radiusX{};
    float radiusY{};
    float rot{};
    float hardness{};
    float flow{};
};

struct EraseStrokePoint {
    float posX{};
    float posY{};
    float radiusX{};
    float radiusY{};
    float rot{};
    float hardness{};
    float flow{};
    float opacity{};
};

} // namespace Midori
