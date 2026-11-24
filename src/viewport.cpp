#include "midori/viewport.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Midori {

namespace {

[[nodiscard]] float ZoomClamp(float zoom, float zoomMin, float zoomMax) noexcept {
    return std::copysign(std::clamp(std::abs(zoom), zoomMin, zoomMax), zoom);
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

Viewport ViewportZoom(Viewport viewport, float amountX, float amountY, float zoomMin, float zoomMax) noexcept {
    viewport.zoomX = ZoomClamp(viewport.zoomX * amountX, zoomMin, zoomMax);
    viewport.zoomY = ZoomClamp(viewport.zoomY * amountY, zoomMin, zoomMax);
    return viewport;
}

Viewport ViewportRotate(Viewport viewport, float amount) noexcept {
    viewport.rot = std::remainder(viewport.rot + amount, 2.0f * std::numbers::pi_v<float>);
    return viewport;
}

Viewport ViewportSnap(Viewport viewport, float dpiScale) noexcept {
    const double c = std::cos(static_cast<double>(viewport.rot));
    const double s = std::sin(static_cast<double>(viewport.rot));
    const double scaleX = static_cast<double>(viewport.zoomX) * dpiScale;
    const double scaleY = static_cast<double>(viewport.zoomY) * dpiScale;
    const double anchorX = std::floor(viewport.posX);
    const double anchorY = std::floor(viewport.posY);
    const double dx = anchorX - viewport.posX;
    const double dy = anchorY - viewport.posY;
    // The anchor in physical pixels from the view center, as Viewport maps it, rounded, then mapped back.
    const double pixelX = std::round(((dx * c) - (dy * s)) * scaleX) / scaleX;
    const double pixelY = std::round(((dx * s) + (dy * c)) * scaleY) / scaleY;
    viewport.posX = anchorX - ((pixelX * c) + (pixelY * s));
    viewport.posY = anchorY - ((pixelY * c) - (pixelX * s));
    return viewport;
}

Viewport ViewportResetZoomRotation(Viewport viewport, float zoom) noexcept {
    viewport.zoomX = std::copysign(zoom, viewport.zoomX);
    viewport.zoomY = std::copysign(zoom, viewport.zoomY);
    viewport.rot = 0.0f;
    return viewport;
}

} // namespace Midori
