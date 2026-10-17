#include "midori/app.hpp"
#include "midori/log.hpp"

#include <tracy/Tracy.hpp>

#include "midori/version.h"

namespace Midori {

Result<void> AppInit(AppInitInfo /*initInfo*/) noexcept {
    ZoneScopedN("AppInit");
    MIDORI_LOG_INFO("{} v{} {}-{} {}", MIDORI_NAME, MIDORI_VERSION_STRING, MIDORI_COMPILER, MIDORI_ARCHITECTURE,
                    MIDORI_PROFILE);

    // The platform owns the window and destroys it in PlatformTerminate.
    if (const auto res = WindowCreate(WindowCreateInfo{.title = MIDORI_NAME " 1"}); !res) {
        MIDORI_LOG_ERROR("WindowCreate failed: {}", ErrorMessage(res.error()));
        return std::unexpected(res.error());
    }

    // if (const auto res = WindowCreate(WindowCreateInfo{.title = MIDORI_NAME " 2"}); !res) {
    //     MIDORI_LOG_ERROR("WindowCreate failed: {}", ErrorMessage(res.error()));
    //     return std::unexpected(res.error());
    // }

    return {};
}

Result<void> AppEvent(Event event) noexcept {
    ZoneScopedN("AppEvent");
    switch (event.type) {
        case EventType::WindowResize:
            MIDORI_LOG_TRACE("window {}: resize {}x{} scale {}", event.windowResize.window, event.windowResize.newWidth,
                             event.windowResize.newHeight, event.windowResize.dpiScale);
            break;
        case EventType::Key:
            MIDORI_LOG_TRACE("window {}: key {} state {} mods {:#04x}", event.key.window,
                             std::to_underlying(event.key.key), std::to_underlying(event.key.inputState),
                             std::to_underlying(event.key.keyMods));
            break;
        case EventType::Button:
            MIDORI_LOG_TRACE("window {}: device {} button {} state {}", event.button.window, event.button.device,
                             std::to_underlying(event.button.button), std::to_underlying(event.button.inputState));
            break;
        case EventType::Pointer:
            MIDORI_LOG_TRACE("window {}: device {} pointer {:.2f},{:.2f} pressure {:.3f} tilt {:.2f},{:.2f}",
                             event.pointer.window, event.pointer.device, event.pointer.posX, event.pointer.posY,
                             event.pointer.pressure, event.pointer.tiltX, event.pointer.tiltY);
            break;
        case EventType::Unknown:
            break;
    }
    return {};
}

void AppTerminate() noexcept {}

} // namespace Midori
