#pragma once

#include "win32_platform.hpp" // <windows.h> with the project's defines

// DirectX-Headers (vendors/DirectX-Headers) before any SDK header, so its d3d12.h is the one used.
#include <directx/d3d12.h>
#include <directx/d3dx12_barriers.h>
#include <directx/d3dx12_core.h>
#include <directx/d3dx12_root_signature.h> // also holds the descriptor handle helpers

#include <dcomp.h>
#include <dxgi1_6.h>

// GPU zones; includes <d3d12.h> itself, so it must come after DirectX-Headers. Without TRACY_ENABLE
// its macros expand to nothing and TracyD3D12Ctx is a null void*.
#include <tracy/TracyD3D12.hpp>

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
// SRVs for UI textures. A context holds its font atlas, plus the replaced one while the atlas grows.
constexpr UINT kUiTextureMax = 4 * kWindowMax;

// A composition swap chain shown in the window through DirectComposition, so it can blend with the
// desktop (premultiplied alpha). Its buffers are exactly the client area: composition swap chains
// must use DXGI_SCALING_STRETCH, which scales a smaller source region (SetSourceSize) up to the whole
// buffer, so oversized buffers would not show pixels 1:1.
struct Win32SwapChain {
    std::array<ID3D12Resource*, kSwapChainBufferCount> buffers;
    IDXGISwapChain4* swapChain; // null marks the slot free
    IDCompositionTarget* target;
    IDCompositionVisual2* visual;
    ImGuiContext* ui; // its DX12 renderer backend was initialized by GraphicsWindowInit
    HANDLE frameWait; // frame latency waitable, signaled when the swap chain can take a frame
    UINT bufferWidth; // the last non-empty client area; kept while minimized
    UINT bufferHeight;
    UINT width;       // the client area, 0 while minimized
    UINT height;
    bool needsFrame;  // content changed since the last present
    bool frameReady;  // `frameWait` was consumed and no frame presented since
};

// The win32 graphics backend's state. Owned by the main thread, like PlatformState. A window id
// indexes `windows`; window `w` owns RTVs [w * kSwapChainBufferCount, (w + 1) * kSwapChainBufferCount).
// `views` holds the tile pool SRV at 0, then the UI texture SRVs; `uiTextures` marks the used ones.
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
    ID3D12RootSignature* tileRootSignature; // embedded in tile.vs.hlsl
    ID3D12PipelineState* tilePipeline;
    ID3D12DescriptorHeap* views;            // shader visible: the tile pool SRV, then UI textures
    ID3D12Resource* tilePool;               // Texture2DArray, one slice per tile
    IDCompositionDesktopDevice* composition;
    TracyD3D12Ctx gpuProfiler;              // timestamp queries on `queue`, collected after each submit
    std::uint64_t fenceValue;               // last value signaled on `queue`
    UINT rtvIncrement;
    UINT viewIncrement;
    UINT allocatorIndex;
    std::array<bool, kUiTextureMax> uiTextures;
};

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
