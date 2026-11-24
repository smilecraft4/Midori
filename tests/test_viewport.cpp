#include "test.hpp"

#include "midori/viewport.hpp"

#include <cmath>
#include <numbers>

namespace Midori {

void TestViewport(TestContext& t) noexcept {
    const Viewport turned{.posX = 10.0, .posY = -5.0, .zoomX = 2.0f, .zoomY = 2.0f, .rot = 0.7f};
    // Sliding there and back is no move.
    const Viewport back = ViewportMove(ViewportMove(turned, 30.0f, -12.0f), -30.0f, 12.0f);
    MIDORI_CHECK_NEAR(t, back.posX, turned.posX, 1e-4);
    MIDORI_CHECK_NEAR(t, back.posY, turned.posY, 1e-4);
    // Unturned at zoom 2, 20 DIPs right is 10 canvas units: the canvas follows the pointer.
    const Viewport straight{.posX = 10.0, .posY = 0.0, .zoomX = 2.0f, .zoomY = 2.0f, .rot = 0.0f};
    MIDORI_CHECK_NEAR(t, ViewportMove(straight, 20.0f, 0.0f).posX, 0.0, 1e-6);

    // Zoom keeps within its limits, and a negative amount flips that axis only.
    MIDORI_CHECK(t, ViewportZoom(straight, 1000.0f, 1000.0f, kViewportZoomMin, kViewportZoomMax).zoomX
                        == kViewportZoomMax);
    MIDORI_CHECK(t, ViewportZoom(straight, 1e-6f, 1e-6f, kViewportZoomMin, kViewportZoomMax).zoomY == kViewportZoomMin);
    const Viewport flipped = ViewportZoom(straight, -1.0f, 1.0f, kViewportZoomMin, kViewportZoomMax);
    MIDORI_CHECK(t, flipped.zoomX == -2.0f && flipped.zoomY == 2.0f);

    // Rotation wraps into -pi..pi.
    const float wrapped = ViewportRotate(straight, (2.0f * std::numbers::pi_v<float>)+0.1f).rot;
    MIDORI_CHECK_NEAR(t, wrapped, 0.1f, 1e-5);
    MIDORI_CHECK(t, std::abs(ViewportRotate(straight, 3.5f).rot) <= std::numbers::pi_v<float>);

    // A reset keeps the flips.
    const Viewport reset =
        ViewportResetZoomRotation({.posX = 0.0, .posY = 0.0, .zoomX = -3.0f, .zoomY = 2.0f, .rot = 1.0f});
    MIDORI_CHECK(t, reset.zoomX == -1.0f && reset.zoomY == 1.0f && reset.rot == 0.0f);
    const Viewport resetHalf =
        ViewportResetZoomRotation({.posX = 0.0, .posY = 0.0, .zoomX = -3.0f, .zoomY = 2.0f, .rot = 1.0f}, 0.5f);
    MIDORI_CHECK(t, resetHalf.zoomX == -0.5f && resetHalf.zoomY == 0.5f && resetHalf.rot == 0.0f);

    // Snapped, the point at the view center lands on a whole physical pixel.
    const Viewport offPixel{.posX = 10.3, .posY = 4.0, .zoomX = 1.0f, .zoomY = 1.0f, .rot = 0.0f};
    MIDORI_CHECK_NEAR(t, ViewportSnap(offPixel, 1.0f).posX, 10.0, 1e-9);
    const Viewport doubled{.posX = 10.3, .posY = 4.0, .zoomX = 2.0f, .zoomY = 2.0f, .rot = 0.0f};
    const double half = ViewportSnap(doubled, 1.0f).posX * 2.0;
    MIDORI_CHECK_NEAR(t, half, std::round(half), 1e-9);
}

} // namespace Midori
