#pragma once

#include "win32_platform.hpp" // <windows.h> with the project's defines

#include <d3d12.h>
#include <dcomp.h>
#include <dxgi1_6.h>

#include <array>
#include <cstdint>
#include <span>

#include "midori/graphics.hpp" // IWYU pragma: export

namespace Midori {

struct GraphicsInitInfo {
    bool debugLayer; // D3D12 debug layer: validates every call, slows the CPU side
};

constexpr UINT kGraphicsFramesInFlight = 2;
constexpr UINT kSwapChainBufferCount = 2; // latency is bounded by the frame waitable, not the count

// A composition swap chain shown in the window through DirectComposition, so it can blend with the
// desktop (premultiplied alpha). Buffers are allocated at least at the size of the window's monitor;
// resizing within them only changes the source size, so it never reallocates.
struct Win32SwapChain {
    std::array<ID3D12Resource*, kSwapChainBufferCount> buffers;
    IDXGISwapChain4* swapChain; // null marks the slot free
    IDCompositionTarget* target;
    IDCompositionVisual2* visual;
    HANDLE frameWait; // frame latency waitable, signaled when the swap chain can take a frame
    UINT bufferWidth;
    UINT bufferHeight;
    UINT width;      // source size: the client area, 0 while minimized
    UINT height;
    bool needsFrame; // content changed since the last present
    bool frameReady; // `frameWait` was consumed and no frame presented since
};

// The win32 graphics backend's state. Owned by the main thread, like PlatformState. A window id
// indexes `windows`; window `w` owns RTVs [w * kSwapChainBufferCount, (w + 1) * kSwapChainBufferCount).
struct GraphicsState {
    std::array<Win32SwapChain, kWindowMax> windows;
    std::array<ID3D12CommandAllocator*, kGraphicsFramesInFlight> allocators;
    std::array<std::uint64_t, kGraphicsFramesInFlight> allocatorFences; // fence value of each allocator's last use
    IDXGIFactory7* factory;
    ID3D12Device* device;
    ID3D12CommandQueue* queue;
    ID3D12GraphicsCommandList* list; // closed between submits
    ID3D12Fence* fence;
    ID3D12DescriptorHeap* rtvs;
    IDCompositionDesktopDevice* composition;
    std::uint64_t fenceValue; // last value signaled on `queue`
    UINT rtvIncrement;
    UINT allocatorIndex;
};

extern GraphicsState gGraphicsState;

// Fills `handles` with the frame waitables of the windows that need a frame and are not ready for one
// yet, and `windows` with their ids. Returns the count. The message loop waits on them with messages.
[[nodiscard]] DWORD GraphicsFrameWaitables(std::span<HANDLE, kWindowMax> handles,
                                           std::span<Window, kWindowMax> windows) noexcept;
// The message loop's wait returned this window's waitable, consuming it.
void GraphicsWindowFrameAcquired(Window window) noexcept;
// Call on every WM_SIZE, in pixels. Presents right away: inside the modal size loop the message loop
// does not run.
[[nodiscard]] Result<void> GraphicsWindowResize(Window window, UINT width, UINT height) noexcept;

} // namespace Midori
