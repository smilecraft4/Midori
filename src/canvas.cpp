#include "midori/canvas.hpp"

namespace Midori {

Viewport ViewportMove(Viewport viewport, float amountX, float amountY) noexcept {
    viewport.posX += amountX;
    viewport.posY += amountY;
    return viewport;
}

Viewport ViewportZoom(Viewport viewport, float amountX, float amountY) noexcept {
    viewport.zoomX *= amountX;
    viewport.zoomY *= amountY;
    return viewport;
}

Viewport ViewportRotate(Viewport viewport, float amount) noexcept {
    viewport.rot += amount;
    return viewport;
}

} // namespace Midori
