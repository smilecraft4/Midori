#include "win32_graphics.hpp"

#include "midori/log.hpp"

#include <algorithm>
#include <cstddef>
#include <string_view>

#include <tracy/Tracy.hpp>

namespace Midori {

GraphicsState gGraphicsState{};

namespace {

// Premultiplied alpha: white at half opacity.
constexpr float kBackground[4] = {0.5f, 0.5f, 0.5f, 0.5f};
constexpr DXGI_FORMAT kSwapChainFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr UINT kSwapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
// Upper bound on waiting for a frame inside the modal size loop, so a stuck GPU cannot hang the UI.
constexpr DWORD kFrameWaitTimeoutMs = 1000;

template<typename T>
void Release(T*& object) noexcept {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

void LogHresult(const char* what, HRESULT hr) noexcept {
    MIDORI_LOG_ERROR("{} failed: {:#010x}", what, static_cast<std::uint32_t>(hr));
}

[[nodiscard]] bool WindowValid(Window window) noexcept {
    return window < kWindowMax && gGraphicsState.windows[window].swapChain;
}

[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE Rtv(Window window, UINT buffer) noexcept {
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(gGraphicsState.rtvs->GetCPUDescriptorHandleForHeapStart(),
                                         static_cast<INT>((window * kSwapChainBufferCount) + buffer),
                                         gGraphicsState.rtvIncrement);
}

void WaitForFence(std::uint64_t value) noexcept {
    if (gGraphicsState.fence->GetCompletedValue() < value) {
        ZoneScopedN("GraphicsWaitForFence");
        // A null event makes the call block until the value is reached.
        gGraphicsState.fence->SetEventOnCompletion(value, nullptr);
    }
}

void WaitIdle() noexcept {
    WaitForFence(gGraphicsState.fenceValue);
}

// The size of the window's monitor, or of the client area when it is larger (spanning monitors), so
// resizing rarely needs new buffers.
[[nodiscard]] SIZE BufferSize(HWND hwnd, UINT width, UINT height) noexcept {
    MONITORINFO monitor{.cbSize = sizeof(MONITORINFO)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
    return SIZE{
        .cx = std::max(monitor.rcMonitor.right - monitor.rcMonitor.left, static_cast<LONG>(width)),
        .cy = std::max(monitor.rcMonitor.bottom - monitor.rcMonitor.top, static_cast<LONG>(height)),
    };
}

[[nodiscard]] Result<void> AcquireBuffers(Window window) noexcept {
    Win32SwapChain& slot = gGraphicsState.windows[window];
    for (UINT i = 0; i < kSwapChainBufferCount; ++i) {
        if (const HRESULT hr = slot.swapChain->GetBuffer(i, IID_PPV_ARGS(&slot.buffers[i])); FAILED(hr)) {
            LogHresult("IDXGISwapChain::GetBuffer", hr);
            return std::unexpected(Error::GraphicsWindowInitFailed);
        }
        gGraphicsState.device->CreateRenderTargetView(slot.buffers[i], nullptr, Rtv(window, i));
    }
    return {};
}

void ReleaseWindow(Win32SwapChain& slot) noexcept {
    Release(slot.visual);
    Release(slot.target);
    for (ID3D12Resource*& buffer : slot.buffers) {
        Release(buffer);
    }
    Release(slot.swapChain);
    if (slot.frameWait) {
        CloseHandle(slot.frameWait);
    }
    slot = {};
}

// Clears the windows' current back buffers in one command list, then presents them. Every window must
// be ready for a frame.
[[nodiscard]] Result<void> Submit(std::span<const Window> windows) noexcept {
    ZoneScopedN("GraphicsSubmit");
    ZoneValue(windows.size());
    GraphicsState& state = gGraphicsState;

    const UINT frame = state.allocatorIndex;
    WaitForFence(state.allocatorFences[frame]);
    {
        ZoneScopedN("GraphicsRecord");
        state.allocators[frame]->Reset();
        state.list->Reset(state.allocators[frame], nullptr);
        {
            // GPU zones record timestamp queries into the list, so they must close before it does.
            TracyD3D12NamedZone(state.gpuProfiler, gpuFrameZone, state.list, "GpuFrame", true);

            std::array<D3D12_RESOURCE_BARRIER, kWindowMax> toTarget{};
            std::array<D3D12_RESOURCE_BARRIER, kWindowMax> toPresent{};
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kWindowMax> rtvs{};
            for (std::size_t i = 0; i < windows.size(); ++i) {
                const Win32SwapChain& slot = state.windows[windows[i]];
                MIDORI_ASSERT(slot.frameReady, "window {} presented without a frame", windows[i]);
                const UINT buffer = slot.swapChain->GetCurrentBackBufferIndex();
                toTarget[i] = CD3DX12_RESOURCE_BARRIER::Transition(slot.buffers[buffer], D3D12_RESOURCE_STATE_PRESENT,
                                                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                toPresent[i] = CD3DX12_RESOURCE_BARRIER::Transition(
                    slot.buffers[buffer], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
                rtvs[i] = Rtv(windows[i], buffer);
            }

            const auto count = static_cast<UINT>(windows.size());
            state.list->ResourceBarrier(count, toTarget.data());
            for (std::size_t i = 0; i < windows.size(); ++i) {
                TracyD3D12NamedZone(state.gpuProfiler, gpuClearZone, state.list, "GpuClearWindow", true);
                // Only the source region is shown; the rest of the buffer is left as is.
                const Win32SwapChain& slot = state.windows[windows[i]];
                const CD3DX12_RECT rect(0, 0, static_cast<LONG>(slot.width), static_cast<LONG>(slot.height));
                state.list->ClearRenderTargetView(rtvs[i], kBackground, 1, &rect);
            }
            state.list->ResourceBarrier(count, toPresent.data());
        }
        state.list->Close();
    }

    {
        ZoneScopedN("GraphicsExecute");
        ID3D12CommandList* lists[] = {state.list};
        state.queue->ExecuteCommandLists(1, lists);
    }

    Result<void> result{};
    for (const Window window : windows) {
        ZoneScopedN("GraphicsPresent");
        ZoneValue(window);
        Win32SwapChain& slot = state.windows[window];
        slot.needsFrame = false;
        slot.frameReady = false;
        if (const HRESULT hr = slot.swapChain->Present(1, 0);
            hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            LogHresult("IDXGISwapChain::Present", hr);
            MIDORI_LOG_ERROR("device removed reason: {:#010x}",
                             static_cast<std::uint32_t>(state.device->GetDeviceRemovedReason()));
            result = std::unexpected(Error::GraphicsDeviceLost);
        }
    }

    state.queue->Signal(state.fence, ++state.fenceValue);
    state.allocatorFences[frame] = state.fenceValue;
    state.allocatorIndex = (frame + 1) % kGraphicsFramesInFlight;
    {
        // Reads back the timestamps of earlier frames the GPU has finished; never waits.
        ZoneScopedN("GpuProfilerCollect");
        TracyD3D12Collect(state.gpuProfiler);
    }
    FrameMark;
    return result;
}

} // namespace

Result<void> GraphicsInit(GraphicsInitInfo initInfo) noexcept {
    ZoneScopedN("GraphicsInit");
    GraphicsState& state = gGraphicsState;
    const auto fail = [](const char* what, HRESULT hr) {
        LogHresult(what, hr);
        GraphicsTerminate();
        return std::unexpected(Error::GraphicsInitFailed);
    };

    if (initInfo.debugLayer) {
        ZoneScopedN("D3D12EnableDebugLayer");
        ID3D12Debug* debug = nullptr;
        if (const HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debug)); SUCCEEDED(hr)) {
            debug->EnableDebugLayer();
            debug->Release();
        } else {
            MIDORI_LOG_WARN("D3D12 debug layer unavailable: {:#010x}", static_cast<std::uint32_t>(hr));
        }
    }

    {
        ZoneScopedN("CreateDXGIFactory2");
        const UINT factoryFlags = initInfo.debugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0;
        if (const HRESULT hr = CreateDXGIFactory2(factoryFlags, IID_PPV_ARGS(&state.factory)); FAILED(hr)) {
            return fail("CreateDXGIFactory2", hr);
        }
    }
    // The default adapter is the one the user's Windows graphics settings pick for this app, which
    // keeps laptops on the integrated GPU unless told otherwise.
    {
        ZoneScopedN("D3D12CreateDevice");
        if (const HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&state.device));
            FAILED(hr)) {
            return fail("D3D12CreateDevice", hr);
        }
    }

    const D3D12_COMMAND_QUEUE_DESC queueDesc{.Type = D3D12_COMMAND_LIST_TYPE_DIRECT};
    if (const HRESULT hr = state.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&state.queue)); FAILED(hr)) {
        return fail("ID3D12Device::CreateCommandQueue", hr);
    }
    for (ID3D12CommandAllocator*& allocator : state.allocators) {
        if (const HRESULT hr =
                state.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
            FAILED(hr)) {
            return fail("ID3D12Device::CreateCommandAllocator", hr);
        }
    }
    if (const HRESULT hr = state.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, state.allocators[0],
                                                           nullptr, IID_PPV_ARGS(&state.list));
        FAILED(hr)) {
        return fail("ID3D12Device::CreateCommandList", hr);
    }
    // Created open; Submit expects it closed.
    state.list->Close();
    {
        // Allocates its query heap and readback buffer even while no profiler is connected.
        ZoneScopedN("TracyD3D12Context");
        state.gpuProfiler = TracyD3D12Context(state.device, state.queue);
        if (state.gpuProfiler) {
            [[maybe_unused]] constexpr std::string_view kName = "Main queue"; // unused without TRACY_ENABLE
            TracyD3D12ContextName(state.gpuProfiler, kName.data(), static_cast<std::uint16_t>(kName.size()));
        }
    }
    if (const HRESULT hr = state.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state.fence));
        FAILED(hr)) {
        return fail("ID3D12Device::CreateFence", hr);
    }

    const D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{
        .Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
        .NumDescriptors = kWindowMax * kSwapChainBufferCount,
    };
    if (const HRESULT hr = state.device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&state.rtvs)); FAILED(hr)) {
        return fail("ID3D12Device::CreateDescriptorHeap", hr);
    }
    state.rtvIncrement = state.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    {
        ZoneScopedN("DCompositionCreateDevice2");
        if (const HRESULT hr = DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&state.composition)); FAILED(hr)) {
            return fail("DCompositionCreateDevice2", hr);
        }
    }
    return {};
}

Result<void> GraphicsUpdate() noexcept {
    ZoneScopedN("GraphicsUpdate");
    std::array<Window, kWindowMax> ready{};
    std::size_t count = 0;
    for (Window window = 0; window < kWindowMax; ++window) {
        Win32SwapChain& slot = gGraphicsState.windows[window];
        if (!slot.swapChain || !slot.needsFrame || slot.width == 0 || slot.height == 0) {
            continue;
        }
        if (!slot.frameReady && WaitForSingleObjectEx(slot.frameWait, 0, TRUE) == WAIT_OBJECT_0) {
            slot.frameReady = true;
        }
        if (slot.frameReady) {
            ready[count++] = window;
        }
    }
    if (count == 0) {
        return {};
    }
    return Submit(std::span(ready.data(), count));
}

void GraphicsTerminate() noexcept {
    ZoneScopedN("GraphicsTerminate");
    GraphicsState& state = gGraphicsState;
    if (state.fence) {
        WaitIdle();
    }
    for (Win32SwapChain& slot : state.windows) {
        ReleaseWindow(slot);
    }
    // Drains its pending queries, so after WaitIdle and before the queue goes.
    if (state.gpuProfiler) {
        TracyD3D12Destroy(state.gpuProfiler);
        state.gpuProfiler = nullptr;
    }
    Release(state.composition);
    Release(state.rtvs);
    Release(state.fence);
    Release(state.list);
    for (ID3D12CommandAllocator*& allocator : state.allocators) {
        Release(allocator);
    }
    Release(state.queue);
    Release(state.device);
    Release(state.factory);
    state = {};
}

Result<void> GraphicsWindowInit(GraphicsWindowInitInfo initInfo) noexcept {
    ZoneScopedN("GraphicsWindowInit");
    const Window window = initInfo.window;
    if (window >= kWindowMax || !gPlatformState.windows[window].hwnd || gGraphicsState.windows[window].swapChain) {
        return std::unexpected(Error::WindowInvalid);
    }
    GraphicsState& state = gGraphicsState;
    Win32SwapChain& slot = state.windows[window];
    const auto fail = [&slot](const char* what, HRESULT hr) {
        LogHresult(what, hr);
        ReleaseWindow(slot);
        return std::unexpected(Error::GraphicsWindowInitFailed);
    };

    const HWND hwnd = gPlatformState.windows[window].hwnd;
    RECT client{};
    GetClientRect(hwnd, &client);
    slot.width = static_cast<UINT>(client.right - client.left);
    slot.height = static_cast<UINT>(client.bottom - client.top);
    const SIZE bufferSize = BufferSize(hwnd, slot.width, slot.height);
    slot.bufferWidth = static_cast<UINT>(bufferSize.cx);
    slot.bufferHeight = static_cast<UINT>(bufferSize.cy);

    // Composition swap chains must stretch; the source size then picks the region shown 1:1.
    const DXGI_SWAP_CHAIN_DESC1 desc{
        .Width = slot.bufferWidth,
        .Height = slot.bufferHeight,
        .Format = kSwapChainFormat,
        .SampleDesc = {.Count = 1},
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT,
        .BufferCount = kSwapChainBufferCount,
        .Scaling = DXGI_SCALING_STRETCH,
        .SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD,
        .AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED,
        .Flags = kSwapChainFlags,
    };
    IDXGISwapChain1* swapChain1 = nullptr;
    {
        ZoneScopedN("CreateSwapChainForComposition");
        if (const HRESULT hr = state.factory->CreateSwapChainForComposition(state.queue, &desc, nullptr, &swapChain1);
            FAILED(hr)) {
            return fail("IDXGIFactory2::CreateSwapChainForComposition", hr);
        }
    }
    const HRESULT queried = swapChain1->QueryInterface(IID_PPV_ARGS(&slot.swapChain));
    swapChain1->Release();
    if (FAILED(queried)) {
        return fail("IDXGISwapChain1::QueryInterface(IDXGISwapChain4)", queried);
    }

    // One frame queued at most: the waitable then signals when the next frame can start.
    slot.swapChain->SetMaximumFrameLatency(1);
    slot.frameWait = slot.swapChain->GetFrameLatencyWaitableObject();
    if (const auto acquired = AcquireBuffers(window); !acquired) {
        ReleaseWindow(slot);
        return std::unexpected(acquired.error());
    }
    if (slot.width > 0 && slot.height > 0) {
        slot.swapChain->SetSourceSize(slot.width, slot.height);
    }

    {
        ZoneScopedN("DCompositionBind");
        if (const HRESULT hr = state.composition->CreateTargetForHwnd(hwnd, TRUE, &slot.target); FAILED(hr)) {
            return fail("IDCompositionDesktopDevice::CreateTargetForHwnd", hr);
        }
        if (const HRESULT hr = state.composition->CreateVisual(&slot.visual); FAILED(hr)) {
            return fail("IDCompositionDevice2::CreateVisual", hr);
        }
        slot.visual->SetContent(slot.swapChain);
        slot.target->SetRoot(slot.visual);
        if (const HRESULT hr = state.composition->Commit(); FAILED(hr)) {
            return fail("IDCompositionDevice2::Commit", hr);
        }
    }

    slot.needsFrame = true;
    return {};
}

Result<void> GraphicsWindowPresent(Window window) noexcept {
    ZoneScopedN("GraphicsWindowPresent");
    if (!WindowValid(window)) {
        return std::unexpected(Error::WindowInvalid);
    }
    Win32SwapChain& slot = gGraphicsState.windows[window];
    if (slot.width == 0 || slot.height == 0) {
        return {};
    }
    if (!slot.frameReady) {
        ZoneScopedN("GraphicsWaitForFrame");
        if (WaitForSingleObjectEx(slot.frameWait, kFrameWaitTimeoutMs, TRUE) != WAIT_OBJECT_0) {
            MIDORI_LOG_WARN("window {}: no frame after {} ms, present skipped", window, kFrameWaitTimeoutMs);
            return {};
        }
        slot.frameReady = true;
    }
    const std::array windows{window};
    return Submit(windows);
}

Result<void> GraphicsWindowTerminate(Window window) noexcept {
    ZoneScopedN("GraphicsWindowTerminate");
    if (window >= kWindowMax) {
        return std::unexpected(Error::WindowInvalid);
    }
    // A window whose GraphicsWindowInit failed or never ran has nothing to release.
    if (!gGraphicsState.windows[window].swapChain) {
        return {};
    }
    WaitIdle();
    ReleaseWindow(gGraphicsState.windows[window]);
    return {};
}

DWORD GraphicsFrameWaitables(std::span<HANDLE, kWindowMax> handles, std::span<Window, kWindowMax> windows) noexcept {
    DWORD count = 0;
    for (Window window = 0; window < kWindowMax; ++window) {
        const Win32SwapChain& slot = gGraphicsState.windows[window];
        if (slot.swapChain && slot.needsFrame && !slot.frameReady && slot.width > 0 && slot.height > 0) {
            handles[count] = slot.frameWait;
            windows[count] = window;
            ++count;
        }
    }
    return count;
}

void GraphicsWindowFrameAcquired(Window window) noexcept {
    MIDORI_ASSERT(WindowValid(window), "frame acquired for invalid window {}", window);
    gGraphicsState.windows[window].frameReady = true;
}

Result<void> GraphicsWindowResize(Window window, UINT width, UINT height) noexcept {
    ZoneScopedN("GraphicsWindowResize");
    // WM_SIZE also arrives during CreateWindowExW, before GraphicsWindowInit.
    if (!WindowValid(window)) {
        return {};
    }
    Win32SwapChain& slot = gGraphicsState.windows[window];
    slot.width = width;
    slot.height = height;
    if (width == 0 || height == 0) {
        return {};
    }

    if (width > slot.bufferWidth || height > slot.bufferHeight) {
        ZoneScopedN("GraphicsResizeBuffers");
        WaitIdle();
        for (ID3D12Resource*& buffer : slot.buffers) {
            Release(buffer);
        }
        const SIZE bufferSize = BufferSize(gPlatformState.windows[window].hwnd, width, height);
        if (const HRESULT hr =
                slot.swapChain->ResizeBuffers(kSwapChainBufferCount, static_cast<UINT>(bufferSize.cx),
                                              static_cast<UINT>(bufferSize.cy), DXGI_FORMAT_UNKNOWN, kSwapChainFlags);
            FAILED(hr)) {
            LogHresult("IDXGISwapChain::ResizeBuffers", hr);
            return std::unexpected(Error::GraphicsDeviceLost);
        }
        slot.bufferWidth = static_cast<UINT>(bufferSize.cx);
        slot.bufferHeight = static_cast<UINT>(bufferSize.cy);
        if (const auto acquired = AcquireBuffers(window); !acquired) {
            return std::unexpected(acquired.error());
        }
    }

    slot.swapChain->SetSourceSize(width, height);
    slot.needsFrame = true;
    return GraphicsWindowPresent(window);
}

} // namespace Midori
