#include "midori/app.hpp"
#include "midori/base.hpp"
#include "midori/log.hpp"
#include "midori/trace.hpp"
#include "win32_graphics.hpp"
#include "win32_platform.hpp"

#include <array>

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
    MIDORI_TRACE_SET_THREAD_NAME("Main");

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

    // Sleeps until a message arrives or a window that needs a frame can take one, handles every
    // pending message, then presents what is ready. Ends when the last window is destroyed (WM_QUIT).
    int exitCode = 0;
    for (;;) {
        std::array<HANDLE, kWindowMax> handles{};
        std::array<Window, kWindowMax> windows{};
        const DWORD count = GraphicsFrameWaitables(handles, windows);
        const DWORD wait =
            MsgWaitForMultipleObjectsEx(count, handles.data(), INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_FAILED) {
            MIDORI_LOG_ERROR("MsgWaitForMultipleObjectsEx failed: {}", GetLastError());
            exitCode = 1;
            break;
        }
        if (wait < WAIT_OBJECT_0 + count) {
            GraphicsWindowFrameAcquired(windows[wait - WAIT_OBJECT_0]);
        }

        bool quit = false;
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                exitCode = static_cast<int>(msg.wParam);
                quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
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
    // Destroys the windows, which releases their swap chains, so it runs before GraphicsTerminate.
    PlatformTerminate();
    GraphicsTerminate();
    LogTerminate();

    return exitCode;
}
