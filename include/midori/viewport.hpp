#pragma once

namespace Midori {

constexpr float kViewportZoomMin = 1.0f / 64.0f;
constexpr float kViewportZoomMax = 64.0f;

// The canvas point (posX, posY) shows at the center of the view. One canvas unit spans |zoom| DIPs on
// screen, and a negative zoom mirrors that screen axis. `rot` turns the canvas clockwise on screen, in
// radians, within [-pi, pi]. Canvas to view: view = ((canvas - pos) * R(rot)) * S(zoomX, zoomY), so
// flips act on screen axes whatever the rotation.
struct Viewport {
    float posX{0.0f};
    float posY{0.0f};
    float zoomX{1.0f};
    float zoomY{1.0f};
    float rot{0.0f};
};

// Slides the canvas by an offset in view DIPs: the canvas point under the pointer follows it.
[[nodiscard]] Viewport ViewportMove(Viewport viewport, float viewX, float viewY) noexcept;
// Scales around the view center; a negative amount flips that axis. |zoom| is clamped to
// [kViewportZoomMin, kViewportZoomMax].
[[nodiscard]] Viewport ViewportZoom(Viewport viewport, float amountX, float amountY) noexcept;
// Turns clockwise on screen around the view center.
[[nodiscard]] Viewport ViewportRotate(Viewport viewport, float amount) noexcept;

} // namespace Midori
