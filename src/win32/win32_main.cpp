#include "midori/app.hpp"
#include "midori/base.hpp"
#include "midori/log.hpp"
#include "win32_graphics.hpp"
#include "win32_platform.hpp"

#include <array>
#include <cstdint>

#include <tracy/Tracy.hpp>

using namespace Midori;

namespace {

#if defined(MIDORI_DEBUG)
constexpr bool kGraphicsDebugLayer = true;
#else
constexpr bool kGraphicsDebugLayer = false;
#endif

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE /*hPrevInstance*/, _In_ LPWSTR /*lpCmdLine*/,
                    _In_ int nShowCmd) {
    // A plain function, only defined when the Tracy client is compiled in.
#if defined(TRACY_ENABLE)
    tracy::SetThreadName("Main");
#endif

    if (!LogInit()) {
        return 1;
    }
    if (!PlatformInit(PlatformInitInfo{.hInstance = hInstance, .nShowCmd = nShowCmd})) {
        LogTerminate();
        return 1;
    }
    if (!GraphicsInit(GraphicsInitInfo{.debugLayer = kGraphicsDebugLayer})) {
        PlatformTerminate();
        LogTerminate();
        return 1;
    }
    if (!AppInit(AppInitInfo{})) {
        PlatformTerminate();
        GraphicsTerminate();
        LogTerminate();
        return 1;
    }

    int exitCode = 0;
    for (;;) {
        std::array<HANDLE, kWindowMax> handles{};
        std::array<Window, kWindowMax> windows{};
        const DWORD count = GraphicsFrameWaitables(handles, windows);
        DWORD wait = WAIT_FAILED;
        wait = MsgWaitForMultipleObjectsEx(count, handles.data(), INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED) {
            MIDORI_LOG_ERROR("MsgWaitForMultipleObjectsEx failed: {}", GetLastError());
            exitCode = 1;
            break;
        }
        if (wait < WAIT_OBJECT_0 + count) {
            GraphicsWindowFrameAcquired(windows[wait - WAIT_OBJECT_0]);
        }

        bool quit = false;
        {
            ZoneScopedN("MainMessages");
            [[maybe_unused]] std::uint32_t handled = 0; // only read by ZoneValue
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    exitCode = static_cast<int>(msg.wParam);
                    quit = true;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
                ++handled;
            }
            ZoneValue(handled);
        }
        if (quit) {
            break;
        }

        if (const auto res = GraphicsUpdate(); !res) {
            MIDORI_LOG_ERROR("GraphicsUpdate failed: {}", ErrorMessage(res.error()));
            exitCode = 1;
            break;
        }
    }

    AppTerminate();
    PlatformTerminate();
    GraphicsTerminate();
    LogTerminate();

    return exitCode;
}
