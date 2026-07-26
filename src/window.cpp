#include "window.hpp"

#include "SDL3/SDL_video.h"
#include "app.hpp"

namespace Midori {

Window::Window(App* app, const CreateInfo& createInfo) : app(app) {
    x = createInfo.x.value_or(SDL_WINDOWPOS_UNDEFINED);
    y = createInfo.y.value_or(SDL_WINDOWPOS_UNDEFINED);
    width = createInfo.width.value_or(1280);
    height = createInfo.height.value_or(720);
    name = createInfo.name;
}

Window::~Window() {
    for (const auto& id : canvasViewIds) {
        if (!id.has_value()) {
            continue;
        }
    
        SDL_assert(!app->canvasViews.contains(id.value()));
    }
}

} // namespace Midori
