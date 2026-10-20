#pragma once

#include "midori/base.hpp"
#include "midori/color.hpp"
#include "midori/platform.hpp"
#include "midori/viewport.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>
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

enum struct LayerFlags : std::uint8_t {
    None = 0,
    Folder = 1 << 0,
    Locked = 1 << 1,
    Mask = 1 << 2,
    Internal = 1 << 3,
    Deleted = 1 << 4, // out of the order; the save deletes its entry and tile range, then frees its id
    Dirty = 1 << 5,   // changed since the last save
};
template<>
inline constexpr bool kEnumFlags<LayerFlags> = true;

struct LayerCreateInfo {
    float opacity{1.0f};
    Layer id{kLayerInvalid}; // if `kLayerInvalid` get any available id
    Layer height{};
    BlendMode blendMode{};
    LayerFlags flags{};
};

// Indexed by layer id.
struct LayerState {
    float opacity{1.0f};
    std::uint32_t contentGeneration{}; // folders: bumped when anything inside changes how it looks
    std::uint32_t tileCount{};         // tiles in memory or in the file
    Layer descendants{};               // folders: the heights [height - descendants, height) are inside
    BlendMode blendMode{};
    LayerFlags flags{};
};

// One file record per layer id, written with a memcpy.
struct LayerFileEntry {
    std::array<char, kLayerNameLenMax> name;
    float opacity;
    std::uint32_t contentGeneration;
    std::uint32_t tileCount;
    Layer descendants;
    BlendMode blendMode;
    LayerFlags flags; // Deleted and Dirty are never written
};
static_assert(std::is_trivially_copyable_v<LayerFileEntry>);
static_assert(sizeof(LayerFileEntry) == kLayerNameLenMax + 16);

// Ordered layer, mip, y, x from the top bit down, so a layer's tiles are one key range.
using TileKey = std::uint64_t;
constexpr std::size_t kTileXBitSize = 25;
constexpr std::size_t kTileXBitOffset = 0;
constexpr std::size_t kTileYBitSize = 25;
constexpr std::size_t kTileYBitOffset = kTileXBitOffset + kTileXBitSize;
constexpr std::size_t kTileMipBitSize = 2;
constexpr std::size_t kTileMipBitOffset = kTileYBitOffset + kTileYBitSize;
constexpr std::size_t kTileLayerBitSize = 12;
constexpr std::size_t kTileLayerBitOffset = kTileMipBitOffset + kTileMipBitSize;
static_assert((kTileLayerBitOffset + kTileLayerBitSize) <= sizeof(TileKey) * 8);
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
// and report back through the Tile*Done calls on the main thread. Lives in AppState::canvases, indexed by
// its id.
struct CanvasState {
    std::vector<TileState> tiles;
    std::unordered_map<TileKey, std::uint32_t> tileIndices;
    std::vector<TileGridPosState> gridPositions;
    std::unordered_map<TileKey, std::uint32_t> gridPosIndices;
    std::vector<TileAtlasCell> freeAtlasCells;
    std::vector<TileKey> uploads;        // RAM cached tiles waiting for an upload slot
    std::vector<TileRead> reads;         // tiles waiting for a read + decompress + upload job
    std::vector<TileDownload> downloads; // tiles waiting for a download slot
    std::vector<TileKey> cacheSaves;     // evicted tiles saved from their cache entry
    std::vector<TileKey> visibleDirty;   // dirty grid positions shown this frame, not queued yet
    std::vector<TileKey> composites;     // visible dirty grid positions with every layer tile loaded
    std::vector<Layer> layersOrder;      // layersOrder[height] = id, bottom to top; saved as one entry
    std::vector<Layer> freeLayers;       // largest first, so pop_back gives the smallest
    std::array<LayerState, kLayerMax> layers;
    std::array<std::array<char, kLayerNameLenMax>, kLayerMax> layerNames;
    std::string_view filename;
    Layer largestLayer{};
    Canvas id{kCanvasInvalid}; // kCanvasInvalid marks the slot free
    PixelFormat pixelFormat{};
};

[[nodiscard]] Result<Canvas> CanvasCreate(CanvasCreateInfo createInfo) noexcept;
[[nodiscard]] Result<void> CanvasDestroy(Canvas canvas) noexcept;

struct CanvasViewCreateInfo {
    Viewport viewport{};
    int x{-1};      // `-1` for automatic x position
    int y{-1};      // `-1` for automatic y position
    int width{-1};  // `-1` for automatic width
    int height{-1}; // `-1` for automatic height
    Canvas canvas{kCanvasInvalid};
    Window window{kWindowInvalid};
};

// Lives in AppState::canvasViews, indexed by its id. Attached to `canvas` and owned by `window`: it is
// destroyed with the window.
struct CanvasViewState {
    Viewport viewport;
    CanvasView id{kCanvasViewInvalid}; // kCanvasViewInvalid marks the slot free
    Canvas canvas{kCanvasInvalid};
    Window window{kWindowInvalid};
};

[[nodiscard]] Result<CanvasView> CanvasViewCreate(CanvasViewCreateInfo createInfo) noexcept;
[[nodiscard]] Result<void> CanvasViewDestroy(CanvasView canvasView) noexcept;

} // namespace Midori
