#pragma once

#include "midori/base.hpp"
#include "midori/color.hpp"
#include "midori/platform.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace Midori {

using Layer = std::uint16_t;
constexpr Layer kLayerMax = 0b111111111111 + 1;
constexpr Layer kLayerInvalid = std::numeric_limits<Layer>::max();
constexpr std::size_t kLayerNameLenMax = 64;

using Canvas = std::uint8_t;
constexpr Canvas kCanvasMax = 16;
constexpr Canvas kCanvasInvalid = std::numeric_limits<Canvas>::max();

using CanvasView = std::uint8_t;
constexpr CanvasView kCanvasViewMax = 16;
constexpr CanvasView kCanvasViewInvalid = std::numeric_limits<CanvasView>::max();

struct LayerCreateInfo {
    float opacity{1.0f};
    Layer id{kLayerInvalid}; // if `kLayerInvalid` get any available id
    Layer height{};
    Layer childsNum{};       // if `childsNum > 0` this layer is a folder
    BlendMode blendMode{};
    bool locked{};
    bool mask{};
    bool internal_{};
    bool persisted_{};
    bool dirty_{};
};

struct LayerState {
    float opacity{1.0f};
    Layer id{kLayerInvalid};
    Layer height{};
    Layer childsNum{}; // if `childsNum > 0` this layer is a folder
    BlendMode blendMode{};
    bool locked{};
    bool mask{};
    bool internal_{};
    bool persisted_{};
    bool dirty_{};
};

// Ordered mip, y, x, layer from the top bit down, so a grid position's layers are adjacent in sorted order.
using TileKey = std::uint64_t;
constexpr std::size_t kTileLayerBitSize = 12;
constexpr std::size_t kTileLayerBitOffset = 0;
constexpr std::size_t kTileXBitSize = 25;
constexpr std::size_t kTileXBitOffset = kTileLayerBitOffset + kTileLayerBitSize;
constexpr std::size_t kTileYBitSize = 25;
constexpr std::size_t kTileYBitOffset = kTileXBitOffset + kTileXBitSize;
constexpr std::size_t kTileMipBitSize = 2;
constexpr std::size_t kTileMipBitOffset = kTileYBitOffset + kTileYBitSize;
static_assert((kTileMipBitOffset + kTileMipBitSize) <= sizeof(TileKey) * 8);
constexpr TileKey kTileLayerMask = ((TileKey{1} << kTileLayerBitSize) - 1) << kTileLayerBitOffset;

// Flags only describe finished work; work in flight is in TileTransfer.
enum struct TileFlags : std::uint8_t {
    None = 0,
    Loaded = 1 << 0,     // in its atlas cell; GPU work recorded from now on may read it
    Editable = 1 << 1,   // Loaded at mip 0
    Dirty = 1 << 2,      // edited since its grid position was last composited
    Modified = 1 << 3,   // differs from the saved version
    CachedRam = 1 << 4,  // the current version is in the RAM cache
    CachedDisk = 1 << 5, // the current version is in the disk cache
    Saved = 1 << 6,      // some version is in the canvas file
};
template<>
inline constexpr bool kEnumFlags<TileFlags> = true;

enum struct TileTransfer : std::uint8_t {
    None = 0,
    Load = 1 << 0,  // read, decompress and upload
    Cache = 1 << 1, // download, compress and write to the cache
    Save = 1 << 2,  // download, compress and write to the canvas file
};
template<>
inline constexpr bool kEnumFlags<TileTransfer> = true;

using TileAtlasCell = std::uint16_t;
constexpr TileAtlasCell kTileAtlasCellInvalid = std::numeric_limits<TileAtlasCell>::max();

// A tile not visible in the current frame may lose its atlas cell; the lowest `visibleFrame` loses it first.
struct TileState {
    TileKey key;
    std::uint32_t edits;        // bumped by every edit; a finished download only counts if it saw the latest
    std::uint32_t cacheEdits;   // `edits` when the cache in flight was requested
    std::uint32_t saveEdits;    // `edits` when the save in flight was requested
    std::uint32_t visibleFrame; // last frame any window showed its grid position
    TileAtlasCell cell;         // valid while Loaded or loading
    TileFlags flags;
    TileTransfer transfer;
};

struct TileGridPosState {
    TileKey key;
    std::uint32_t visibleFrame;
    bool dirty;  // a layer tile was edited since the last composite
    bool queued; // in `CanvasState::composites`
};

struct TileRead {
    TileKey key;
    TileFlags source; // CachedDisk or Saved
};

struct TileDownload {
    TileKey key;
    TileTransfer purpose; // Cache or Save
};

struct CanvasCreateInfo {
    std::string_view filename;
    PixelFormat pixelFormat{};
};

// Owned and touched by the main thread only: jobs and the graphics backend drain the tile request queues
// and report back through the Tile*Done calls on the main thread.
struct CanvasState {
    std::vector<TileState> tiles;
    std::unordered_map<TileKey, std::uint32_t> tileIndices;
    std::vector<TileGridPosState> gridPositions;
    std::unordered_map<TileKey, std::uint32_t> gridPosIndices;
    std::vector<TileAtlasCell> freeAtlasCells;
    std::vector<TileKey> uploads;                         // RAM cached tiles waiting for an upload slot
    std::vector<TileRead> reads;                          // tiles waiting for a read + decompress + upload job
    std::vector<TileDownload> downloads;                  // tiles waiting for a download slot
    std::vector<TileKey> cacheSaves;                      // evicted tiles saved from their cache entry
    std::vector<TileKey> visibleDirty;                    // dirty grid positions shown this frame, not queued yet
    std::vector<TileKey> composites;                      // visible dirty grid positions with every layer tile loaded
    std::string_view filename;
    std::array<CanvasView, kCanvasViewMax> canvasViews{}; // the first `canvasViewsNum` are valid
    std::uint8_t canvasViewsNum{};
    PixelFormat pixelFormat{};
};

[[nodiscard]] Result<Canvas> CanvasCreate(CanvasCreateInfo createInfo) noexcept;
[[nodiscard]] Result<void> CanvasDestroy(Canvas canvas) noexcept;

struct Viewport {
    float posX{0.0f};
    float posY{0.0f};
    float zoomX{1.0f};
    float zoomY{1.0f};
    float rot{0.0f};
};

[[nodiscard]] Viewport ViewportMove(Viewport viewport, float amountX, float amountY) noexcept;
[[nodiscard]] Viewport ViewportZoom(Viewport viewport, float amountX, float amountY) noexcept;
[[nodiscard]] Viewport ViewportRotate(Viewport viewport, float amount) noexcept;

struct CanvasViewCreateInfo {
    Viewport viewport{};
    int x{-1};      // `-1` for automatic x position
    int y{-1};      // `-1` for automatic y position
    int width{-1};  // `-1` for automatic width
    int height{-1}; // `-1` for automatic height
    Canvas canvas{kCanvasInvalid};
    Window window{kWindowInvalid};
};

struct CanvasViewState {
    Viewport viewport;
    CanvasView id{kCanvasViewInvalid};
    Canvas canvas{kCanvasInvalid};
    Window window{kWindowInvalid};
};

[[nodiscard]] Result<CanvasView> CanvasViewCreate(CanvasViewCreateInfo createInfo) noexcept;
[[nodiscard]] Result<void> CanvasViewDestroy(CanvasView canvasView) noexcept;

struct PaintStrokePoint {
    Color::Linear color{};
    float posX{};
    float posY{};
    float radiusX{};
    float radiusY{};
    float rot{};
    float hardness{};
    float flow{};
};

struct EraseStrokePoint {
    float posX{};
    float posY{};
    float radiusX{};
    float radiusY{};
    float rot{};
    float hardness{};
    float flow{};
    float opacity{};
};

} // namespace Midori
