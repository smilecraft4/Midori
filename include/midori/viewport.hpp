#pragma once

namespace Midori {

// The range the zoom limits of the settings take.
constexpr float kViewportZoomMin = 1.0f / 64.0f;
constexpr float kViewportZoomMax = 64.0f;
// |pos| stays within it: half the tile key's range, so a brush near the limit never reaches a tile it cannot address.
constexpr double kViewportPosMax = 2147483648.0;

// The canvas point (posX, posY) shows at the center of the view. Double, so a canvas point keeps sub-texel precision
// anywhere in the tile key's range; the renderer subtracts the view's tile before anything reaches the GPU. One canvas
// unit spans |zoom| DIPs on screen, and a negative zoom mirrors that screen axis. `rot` turns the canvas clockwise on
// screen, in radians, within [-pi, pi]. Canvas to view: view = ((canvas - pos) * R(rot)) * S(zoomX, zoomY), so flips
// act on screen axes whatever the rotation.
struct Viewport {
    double posX{0.0};
    double posY{0.0};
    float zoomX{1.0f};
    float zoomY{1.0f};
    float rot{0.0f};
};

// Slides the canvas by an offset in view DIPs: the canvas point under the pointer follows it. |pos| is clamped to
// kViewportPosMax.
[[nodiscard]] Viewport ViewportMove(Viewport viewport, float viewX, float viewY) noexcept;
// Scales around the view center; a negative amount flips that axis. |zoom| is clamped to [zoomMin, zoomMax].
[[nodiscard]] Viewport ViewportZoom(Viewport viewport, float amountX, float amountY, float zoomMin,
                                    float zoomMax) noexcept;
// Turns clockwise on screen around the view center.
[[nodiscard]] Viewport ViewportRotate(Viewport viewport, float amount) noexcept;
// Back to `zoom` and no rotation around the view center; flips are kept.
[[nodiscard]] Viewport ViewportResetZoomRotation(Viewport viewport, float zoom = 1.0f) noexcept;
// Moves pos by less than a physical pixel so the whole canvas point at it lands on a whole pixel from the view center,
// which the renderer floors to a pixel too. Where zoom and rotation map texels to whole pixels, every texel then
// covers whole pixels and stays sharp. Anchored next to pos rather than at the canvas origin, so the rounding of the
// rotation does not grow with the distance from it.
[[nodiscard]] Viewport ViewportSnap(Viewport viewport, float dpiScale) noexcept;

} // namespace Midori
