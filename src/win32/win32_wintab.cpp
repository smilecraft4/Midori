#include "win32_platform.hpp"

#include "midori/log.hpp"

#include <cmath>
#include <iterator>
#include <numbers>

// Packet layout: absolute positions, tip pressure, orientation (tilt), button bits and status (for
// the eraser bit). Buttons are absolute too: the current bit set, compared against the last packet.
#define PACKETDATA (PK_X | PK_Y | PK_NORMAL_PRESSURE | PK_ORIENTATION | PK_BUTTONS | PK_STATUS)
#define PACKETMODE 0
#include <pktdef.h>

namespace Midori {

namespace {

constexpr int kQueueSize = 128;

template<typename F>
[[nodiscard]] F Load(HMODULE dll, const char* name) noexcept {
    // Through void* so the compiler does not warn about converting between function pointer types.
    return reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(dll, name)));
}

void Unload(const char* reason) noexcept {
    MIDORI_LOG_INFO("Wintab unavailable ({}): pen input falls back to the mouse", reason);
    if (gPlatformState.wintab.dll) {
        FreeLibrary(gPlatformState.wintab.dll);
    }
    gPlatformState.wintab = {};
}

void DispatchButton(Window window, Device device, Button button, bool down) noexcept {
    Event event{};
    event.type = EventType::Button;
    event.button = ButtonEvent{
        .window = window,
        .device = device,
        .button = button,
        .inputState = down ? InputState::Press : InputState::Release,
    };
    PlatformDispatch(event);
}

} // namespace

void WintabInit() noexcept {
    Wintab& wintab = gPlatformState.wintab;
    wintab.dll = LoadLibraryW(L"Wintab32.dll");
    if (!wintab.dll) {
        Unload("Wintab32.dll not found");
        return;
    }
    wintab.info = Load<decltype(wintab.info)>(wintab.dll, "WTInfoW");
    wintab.open = Load<decltype(wintab.open)>(wintab.dll, "WTOpenW");
    wintab.close = Load<decltype(wintab.close)>(wintab.dll, "WTClose");
    wintab.packetsGet = Load<decltype(wintab.packetsGet)>(wintab.dll, "WTPacketsGet");
    wintab.overlap = Load<decltype(wintab.overlap)>(wintab.dll, "WTOverlap");
    wintab.queueSizeSet = Load<decltype(wintab.queueSizeSet)>(wintab.dll, "WTQueueSizeSet");
    if (!wintab.info || !wintab.open || !wintab.close || !wintab.packetsGet || !wintab.overlap
        || !wintab.queueSizeSet) {
        Unload("Wintab32.dll is missing entry points");
        return;
    }
    // WTInfo(0, 0, nullptr) is non-zero when the tablet service is running.
    if (!wintab.info(0, 0, nullptr)) {
        Unload("no tablet service");
        return;
    }

    // The default system context keeps the system cursor following the pen.
    LOGCONTEXTW& context = wintab.context;
    if (!wintab.info(WTI_DEFSYSCTX, 0, &context)) {
        Unload("no default context");
        return;
    }

    AXIS pressure{};
    wintab.info(WTI_DEVICES, DVC_NPRESSURE, &pressure);
    wintab.maxPressure = pressure.axMax > 0 ? static_cast<float>(pressure.axMax) : 1.0f;

    AXIS orientation[3]{};
    if (wintab.info(WTI_DEVICES, DVC_ORIENTATION, orientation) && orientation[0].axMax > 0
        && orientation[1].axMax > 0) {
        wintab.maxAzimuth = static_cast<float>(orientation[0].axMax);
        wintab.maxAltitude = static_cast<float>(orientation[1].axMax);
    }

    // The screen area the driver maps the tablet to, in screen pixels.
    wintab.screenX = static_cast<float>(context.lcSysOrgX);
    wintab.screenY = static_cast<float>(context.lcSysOrgY);
    wintab.screenWidth = static_cast<float>(context.lcSysExtX);
    wintab.screenHeight = static_cast<float>(context.lcSysExtY);

    // Report the full tablet resolution instead of screen pixels, for sub-pixel positions. A negative
    // Y extent flips the axis so Y grows downwards like the screen.
    context.lcOptions |= CXO_MESSAGES | CXO_SYSTEM;
    context.lcPktData = PACKETDATA;
    context.lcPktMode = PACKETMODE;
    context.lcMoveMask = PACKETDATA;
    context.lcBtnUpMask = context.lcBtnDnMask;
    context.lcOutOrgX = 0;
    context.lcOutOrgY = 0;
    context.lcOutExtX = context.lcInExtX;
    context.lcOutExtY = -context.lcInExtY;
    wintab.tabletWidth = static_cast<float>(context.lcInExtX);
    wintab.tabletHeight = static_cast<float>(context.lcInExtY);

    MIDORI_LOG_INFO("Wintab: {} pressure levels, tilt {}", pressure.axMax + 1, wintab.maxAltitude > 0 ? "yes" : "no");
}

void WintabTerminate() noexcept {
    if (gPlatformState.wintab.dll) {
        FreeLibrary(gPlatformState.wintab.dll);
    }
    gPlatformState.wintab = {};
}

void WintabOpen(Window window) noexcept {
    const Wintab& wintab = gPlatformState.wintab;
    if (!wintab.dll) {
        return;
    }
    Win32Window& slot = gPlatformState.windows[window];
    LOGCONTEXTW context = wintab.context;
    slot.tablet = wintab.open(slot.hwnd, &context, TRUE);
    if (!slot.tablet) {
        MIDORI_LOG_WARN("WTOpen failed for window {}: no pen input there", window);
        return;
    }
    // The default queue holds a handful of packets; a fast pen overflows it between messages.
    if (!wintab.queueSizeSet(slot.tablet, kQueueSize)) {
        MIDORI_LOG_WARN("WTQueueSizeSet({}) failed: packets may be dropped", kQueueSize);
    }
}

void WintabClose(Window window) noexcept {
    Win32Window& slot = gPlatformState.windows[window];
    if (slot.tablet) {
        gPlatformState.wintab.close(slot.tablet);
        slot.tablet = nullptr;
    }
}

void WintabActivate(Window window) noexcept {
    // The focused window's context goes on top, so it is the one receiving packets.
    const Win32Window& slot = gPlatformState.windows[window];
    if (slot.tablet) {
        gPlatformState.wintab.overlap(slot.tablet, TRUE);
    }
}

void WintabHandlePackets(Window window) noexcept {
    const Wintab& wintab = gPlatformState.wintab;
    Win32Window& slot = gPlatformState.windows[window];
    if (!slot.tablet) {
        return;
    }

    // Drain the whole queue at once: one WT_PACKET can stand for many packets.
    PACKET packets[kQueueSize];
    const int count = wintab.packetsGet(slot.tablet, kQueueSize, packets);

    POINT clientOrigin{};
    ClientToScreen(slot.hwnd, &clientOrigin);
    const float originX = static_cast<float>(clientOrigin.x) - wintab.screenX;
    const float originY = static_cast<float>(clientOrigin.y) - wintab.screenY;
    const float scaleX = wintab.screenWidth / wintab.tabletWidth;
    const float scaleY = wintab.screenHeight / wintab.tabletHeight;

    for (int i = 0; i < count; ++i) {
        const PACKET& packet = packets[i];
        const Device device = (packet.pkStatus & TPS_INVERT) ? kDeviceEraser : kDevicePen;

        float tiltX = 0.0f;
        float tiltY = 0.0f;
        if (wintab.maxAltitude > 0.0f) {
            // Azimuth: clockwise from the tablet's top. Altitude: from the tablet's plane, 90 deg = upright.
            const float azimuth = static_cast<float>(packet.pkOrientation.orAzimuth) / wintab.maxAzimuth * 2.0f
                                * std::numbers::pi_v<float>;
            const float altitude = static_cast<float>(std::abs(packet.pkOrientation.orAltitude)) / wintab.maxAltitude
                                 * 0.5f * std::numbers::pi_v<float>;
            const float lean = std::cos(altitude);
            tiltX = lean * std::sin(azimuth);
            tiltY = -lean * std::cos(azimuth);
        }

        Event event{};
        event.type = EventType::Pointer;
        event.pointer = PointerEvent{
            .posX = static_cast<float>(packet.pkX) * scaleX - originX,
            .posY = static_cast<float>(packet.pkY) * scaleY - originY,
            .pressure = static_cast<float>(packet.pkNormalPressure) / wintab.maxPressure,
            .tiltX = tiltX,
            .tiltY = tiltY,
            .window = window,
            .device = device,
        };
        PlatformDispatch(event);

        // Bit 0 is the tip, bits 1 and 2 the barrel buttons.
        const DWORD changed = packet.pkButtons ^ slot.tabletButtons;
        constexpr Button kButtons[] = {Button::Left, Button::Right, Button::Middle};
        for (DWORD bit = 0; bit < std::size(kButtons); ++bit) {
            if (changed & (1u << bit)) {
                DispatchButton(window, device, kButtons[bit], (packet.pkButtons & (1u << bit)) != 0);
            }
        }
        slot.tabletButtons = packet.pkButtons;
    }
}

} // namespace Midori
