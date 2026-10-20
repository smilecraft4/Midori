#include "midori/viewport.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Midori {

namespace {

[[nodiscard]] float ZoomClamp(float zoom) noexcept {
    return std::copysign(std::clamp(std::abs(zoom), kViewportZoomMin, kViewportZoomMax), zoom);
}

} // namespace

Viewport ViewportMove(Viewport viewport, float viewX, float viewY) noexcept {
    // Back through S^-1 then R^T to a canvas offset.
    const float c = std::cos(viewport.rot);
    const float s = std::sin(viewport.rot);
    const float x = viewX / viewport.zoomX;
    const float y = viewY / viewport.zoomY;
    viewport.posX -= (x * c) + (y * s);
    viewport.posY -= (y * c) - (x * s);
    return viewport;
}

Viewport ViewportZoom(Viewport viewport, float amountX, float amountY) noexcept {
    viewport.zoomX = ZoomClamp(viewport.zoomX * amountX);
    viewport.zoomY = ZoomClamp(viewport.zoomY * amountY);
    return viewport;
}

Viewport ViewportRotate(Viewport viewport, float amount) noexcept {
    viewport.rot = std::remainder(viewport.rot + amount, 2.0f * std::numbers::pi_v<float>);
    return viewport;
}

} // namespace Midori
