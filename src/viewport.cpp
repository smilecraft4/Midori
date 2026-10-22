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
    viewport.posX = std::clamp(viewport.posX - ((x * c) + (y * s)), -kViewportPosMax, kViewportPosMax);
    viewport.posY = std::clamp(viewport.posY - ((y * c) - (x * s)), -kViewportPosMax, kViewportPosMax);
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

Viewport ViewportResetZoomRotation(Viewport viewport) noexcept {
    viewport.zoomX = std::copysign(1.0f, viewport.zoomX);
    viewport.zoomY = std::copysign(1.0f, viewport.zoomY);
    viewport.rot = 0.0f;
    return viewport;
}

} // namespace Midori
