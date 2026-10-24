#pragma once

#include "midori/base.hpp"
#include "midori/color.hpp"
#include "midori/platform.hpp"
#include "midori/viewport.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <type_traits>
#include <unordered_map>
#include <vector>

struct MDBX_env;

namespace Midori {

struct Job;

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
    Internal = 1 << 3, // the root folder: always the top of the order, around every other layer
    Deleted = 1 << 4,  // out of the order, its tiles kept while history can bring it back
    Dirty = 1 << 5,    // changed since the last save
    Hidden = 1 << 6,
    Purged = 1 << 7,   // Deleted for good, its tiles gone: the save deletes its entry and tile range, then frees its id
};
template<>
inline constexpr bool kEnumFlags<LayerFlags> = true;

// Lands in front of layersOrder[height]: a folder at height f holds [f - descendants, f], so height f puts the layer
// inside it on top, f + 1 above it.
struct LayerCreateInfo {
    float opacity{1.0f};
    Layer id{kLayerInvalid}; // if `kLayerInvalid` get any available id
    Layer height{};
    std::array<char, kLayerNameLenMax> name{};
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
    LayerFlags flags; // Dirty and Purged are never written; a Deleted one is purged when opened
};
static_assert(std::is_trivially_copyable_v<LayerFileEntry>);
static_assert(sizeof(LayerFileEntry) == kLayerNameLenMax + 16);

constexpr std::uint32_t kCanvasFileMagic = 0x4F44494D; // "MIDO"
// 3: a root folder tops the order. 2: tiles are QOI. Versions 1 (raw tiles) and 2 still read.
constexpr std::uint32_t kCanvasFileVersion = 3;

// Keys of the meta table.
enum struct CanvasFileMeta : std::uint32_t {
    Header,      // CanvasFileHeader
    LayersOrder, // CanvasState::layersOrder, raw
};

// Written with a memcpy.
struct CanvasFileHeader {
    std::uint32_t magic;
    std::uint32_t version;
    Layer largestLayer;
    PixelFormat pixelFormat;
    std::uint8_t reserved; // zero
};
static_assert(std::is_trivially_copyable_v<CanvasFileHeader>);
static_assert(sizeof(CanvasFileHeader) == 12);

// The open canvas file, kept open (and locked) while the canvas lives. Tables: meta keyed by CanvasFileMeta,
// layers keyed by layer id, tiles keyed by TileKey.
struct CanvasFile {
    MDBX_env* env{}; // null while the canvas has no file
    std::uint32_t metaDbi{};
    std::uint32_t layersDbi{};
    std::uint32_t tilesDbi{};
};

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
    Dirty = 1 << 2,      // Composite: what it is composited from changed since
    Modified = 1 << 3,   // differs from the saved version
    CachedRam = 1 << 4,  // the current version is in the RAM cache
    CachedDisk = 1 << 5, // the current version is in the disk cache
    Saved = 1 << 6,      // some version is in the canvas file
    Composite = 1 << 7,  // a folder's: never saved, cached or loaded; evicted, it is Dirty and composited again
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

// The tile pool every canvas shares: up to kTileAtlasMax atlases of kTileAtlasPages pages, created as needed. A
// page is one 256^2 slice with 4 mips, numbered atlas * kTileAtlasPages + slice. Mirrored in atlas.hlsli.
using TileAtlas = std::uint8_t;
constexpr TileAtlas kTileAtlasMax = 16;
using TileAtlasPage = std::uint16_t;
constexpr TileAtlasPage kTileAtlasPages = 64;
constexpr TileAtlasPage kTileAtlasPageMax = kTileAtlasMax * kTileAtlasPages;
// Holds the transparent cell new tiles start from. Never handed out.
constexpr TileAtlasPage kTileAtlasPageClear = 0;

[[nodiscard]] constexpr TileAtlas TileAtlasPageAtlas(TileAtlasPage page) noexcept {
    return static_cast<TileAtlas>(page / kTileAtlasPages);
}

[[nodiscard]] constexpr TileAtlasPage TileAtlasPageSlice(TileAtlasPage page) noexcept {
    return page % kTileAtlasPages;
}

// A page is 64 blocks of 32^2 in Morton order. A level-k tile takes an aligned cell of 256 >> k, a run of
// 64 >> 2k blocks; the page's mips give its coarser levels, and box mips stay inside the cell.
constexpr std::uint8_t kTileAtlasBlocks = 64;

// One allocation: page << 8 | Morton index of its first block << 2 | level.
using TileAtlasCell = std::uint32_t;
constexpr TileAtlasCell kTileAtlasCellInvalid = std::numeric_limits<TileAtlasCell>::max();

[[nodiscard]] constexpr TileAtlasCell TileAtlasCellMake(TileAtlasPage page, std::uint8_t block,
                                                        std::uint8_t level) noexcept {
    return (TileAtlasCell{page} << 8) | (TileAtlasCell{block} << 2) | level;
}

[[nodiscard]] constexpr TileAtlasPage TileAtlasCellPage(TileAtlasCell cell) noexcept {
    return static_cast<TileAtlasPage>(cell >> 8);
}

[[nodiscard]] constexpr std::uint8_t TileAtlasCellBlock(TileAtlasCell cell) noexcept {
    return static_cast<std::uint8_t>((cell >> 2) & 63);
}

[[nodiscard]] constexpr std::uint8_t TileAtlasCellLevel(TileAtlasCell cell) noexcept {
    return static_cast<std::uint8_t>(cell & 3);
}

[[nodiscard]] constexpr std::uint8_t TileAtlasLevelBlocks(std::uint8_t level) noexcept {
    return static_cast<std::uint8_t>(kTileAtlasBlocks >> (2 * level));
}

constexpr std::uint32_t kTileAtlasBlockSize = 32; // texels

// Texel origin of a cell in its page, from the Morton index of its first block. Mirrored in atlas.hlsli.
// Morton index of a 32^2 block to its column and row in a page or a level-0 tile, and back. x takes the even bits.
[[nodiscard]] constexpr std::uint32_t TileBlockX(std::uint32_t i) noexcept {
    return (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4);
}

[[nodiscard]] constexpr std::uint32_t TileBlockY(std::uint32_t i) noexcept {
    return ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4);
}

[[nodiscard]] constexpr std::uint8_t TileBlockIndex(std::uint32_t x, std::uint32_t y) noexcept {
    return static_cast<std::uint8_t>((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2)
                                     | ((y & 4) << 3));
}

[[nodiscard]] constexpr std::uint32_t TileAtlasCellX(TileAtlasCell cell) noexcept {
    return TileBlockX(TileAtlasCellBlock(cell)) * kTileAtlasBlockSize;
}

[[nodiscard]] constexpr std::uint32_t TileAtlasCellY(TileAtlasCell cell) noexcept {
    return TileBlockY(TileAtlasCellBlock(cell)) * kTileAtlasBlockSize;
}

constexpr TileAtlasCell kTileAtlasCellClear = TileAtlasCellMake(kTileAtlasPageClear, 0, 0);

// Who holds a block. A tile is found through its canvas's tileIndices; kCanvasInvalid marks a block held by
// no tile (the clear cell, a stroke backup), which is never evicted.
struct TileAtlasOwner {
    TileKey key;
    Canvas canvas{kCanvasInvalid};
    std::uint8_t level; // of the owner's whole cell, which may span more blocks than one lookup scans
};

// Owned by the main thread, in AppState. Only the pages of created atlases are in use.
struct TileAtlasState {
    std::array<std::uint64_t, kTileAtlasPageMax> used; // per page, bit i set: Morton block i is taken
    std::array<std::array<TileAtlasOwner, kTileAtlasBlocks>, kTileAtlasPageMax> owners;
    TileAtlas count{};                                 // atlases created by the graphics backend, [0, count)
};

// A tile not visible in the current frame may lose its atlas cell; the lowest `visibleFrame` loses it first.
struct TileState {
    TileKey key;
    std::uint32_t edits;        // bumped by every edit; a finished download only counts if it saw the latest
    std::uint32_t cacheEdits;   // `edits` when the cache in flight was requested
    std::uint32_t saveEdits;    // `edits` when the save in flight was requested
    std::uint32_t visibleFrame; // last frame any window showed its grid position
    TileAtlasCell cell;         // valid while Loaded or waiting in `creates`; evicted tiles have none
    TileFlags flags;
    TileTransfer transfer;
};

// A grid position some view shows this frame where a layer has a tile.
struct CompositeWant {
    TileKey gridPos;    // level-0 key, layer bits zero
    std::uint8_t level; // the finest level a view shows it at
};

struct TileRead {
    TileKey key;
    TileFlags source; // CachedDisk or Saved
};

struct TileDownload {
    TileKey key;
    TileTransfer purpose; // Cache or Save
};

// The other version of one 32^2 block of a level-0 tile, in a slot of the history store (HistoryPoolState): from
// before the edit while it can be undone, from after it once undone.
struct HistoryBlock {
    TileKey key;
    std::uint32_t slot;
    std::uint8_t block; // Morton index in the tile
};

// A level-0 tile the edit created. While the edit is undone the tile is gone and `cell` keeps it, held by no tile so
// it is never evicted; kTileAtlasCellInvalid otherwise.
struct HistoryTile {
    TileKey key;
    TileAtlasCell cell;
};

// A layer a layer op changed, as it was before and after.
struct HistoryLayer {
    LayerState before;
    LayerState after;
    std::array<char, kLayerNameLenMax> beforeName;
    std::array<char, kLayerNameLenMax> afterName;
    Layer id;
};

// One edit: HistoryState::blocks[firstBlock, firstBlock + blockCount) and tiles[firstTile, firstTile + tileCount) for a
// stroke; layers[firstLayer, firstLayer + layerCount) and the layersOrder before then after it, from
// orders[firstOrder], for a layer op.
struct HistoryRecord {
    std::uint64_t sequence; // orders records across canvases, the oldest goes first when the store is full
    std::uint32_t firstBlock;
    std::uint32_t blockCount;
    std::uint32_t firstTile;
    std::uint32_t tileCount;
    std::uint32_t firstLayer;
    std::uint32_t layerCount;
    std::uint32_t firstOrder;
    std::uint32_t orderBeforeCount;
    std::uint32_t orderAfterCount;
};

// records[0, current) can be undone, the rest redone. Undo and redo swap each block with its tile's on the GPU, and
// delete or insert the created tiles; a layer op puts its layers and order back as they were before, or after. A
// Deleted layer no record names any more is purged.
struct HistoryState {
    std::vector<HistoryBlock> blocks;
    std::vector<HistoryTile> tiles;
    std::vector<HistoryLayer> layers;
    std::vector<Layer> orders;
    std::vector<HistoryRecord> records;
    std::uint32_t current{};
    std::uint32_t pendingFrame{}; // gAppState.frame when `pending` last left 0 or a step was applied
    std::int32_t pending{};       // steps asked for and not applied yet: negative undoes, positive redoes
};

// A merge in progress: no edit of its canvas until it is done. Each grid position's result tile is composited once the
// sources' tiles there are loaded; then the result takes the place of the sources, which are Deleted.
struct LayerMergeState {
    std::vector<TileKey> remaining; // level-0 keys of the grid positions left, layer bits zero
    std::vector<TileKey> queued;    // grid positions whose result tile the next submit composites
    Layer result{kLayerInvalid};    // kLayerInvalid: no merge
    Layer upper{kLayerInvalid};     // merged down into `lower`, or the folder flattened
    Layer lower{kLayerInvalid};     // kLayerInvalid when flattening a folder
};

// Owned and touched by the main thread only: jobs and the graphics backend drain the tile request queues
// and report back through the Tile*Done calls on the main thread. Lives in AppState::canvases, indexed by
// its id.
struct CanvasState {
    std::filesystem::path path; // empty until first saved
    std::vector<TileState> tiles;
    std::unordered_map<TileKey, std::uint32_t> tileIndices;
    std::vector<TileKey> creates;              // new tiles waiting for their atlas cell to be filled
    std::vector<TileKey> mipBuilds;            // level-0 tiles whose new level 1-3 tiles the next submit builds
    std::vector<TileKey> tileDeletes;          // tiles in the file deleted since the last save
    std::vector<TileKey> uploads;              // RAM cached tiles waiting for an upload slot
    std::vector<TileRead> reads;               // tiles waiting for a read + decompress + upload job
    std::vector<TileDownload> downloads;       // tiles waiting for a download slot
    std::vector<TileKey> cacheSaves;           // evicted tiles saved from their cache entry
    std::vector<CompositeWant> compositeWants; // sorted by grid position, rebuilt every frame by TilesMarkVisible
    std::vector<TileKey> composites;   // level-0 folder tiles the next submit composites, children before parents
    std::vector<Layer> layersOrder;    // layersOrder[height] = id, bottom to top, the root last; saved as one entry
    std::vector<Layer> freeLayers;     // largest first, so pop_back gives the smallest
    std::vector<Layer> selectedLayers; // picked together, any order; may hold layers gone since, see LayersSelected
    std::array<LayerState, kLayerMax> layers;
    std::array<std::array<char, kLayerNameLenMax>, kLayerMax> layerNames;
    // Derived from layersOrder, rebuilt after every change of it; kLayerInvalid for a layer out of the order, and for
    // the root's parent.
    std::array<Layer, kLayerMax> layerHeights;
    std::array<Layer, kLayerMax> layerParents;
    LayerMergeState merge;
    Color::Srgb backgroundColor{.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f}; // under every layer, not saved yet
    CanvasFile file;
    CanvasFile cache; // a temporary file holding evicted tiles, only its tiles table used; null when not created
    HistoryState history;
    Layer largestLayer{};
    Layer activeLayer{kLayerInvalid}; // strokes paint into it; kLayerInvalid when none
    Canvas id{kCanvasInvalid};        // kCanvasInvalid marks the slot free
    PixelFormat pixelFormat{};
    bool modified{};                  // changed since the last save, or since created
};

// These never ask the user anything; the App* calls wrap them with dialogs.
[[nodiscard]] Result<Canvas> CanvasNew() noexcept;
// Tiles stay in the file until visible.
[[nodiscard]] Result<Canvas> CanvasOpen(const std::filesystem::path& path) noexcept;
// Writes what changed since the last save into the canvas's file, in one transaction. The canvas must have one.
// Transparent level-0 tiles are deleted, from the file and the canvas, with their level 1-3 tiles. Both saves return
// Error::CanvasBusy while the canvas merges layers.
[[nodiscard]] Result<void> CanvasSave(Canvas canvas) noexcept;
// Writes the whole canvas into a new file that then replaces `path`, and keeps it as the canvas's file.
[[nodiscard]] Result<Canvas> CanvasSaveAs(Canvas canvas, const std::filesystem::path& path) noexcept;
// Unsaved changes are lost. Waits for every job.
void CanvasDestroy(Canvas canvas) noexcept;
// Once per update, after the tiles shown this frame have `visibleFrame` set; never waits. Takes the finished jobs
// and GPU copies, queues the root composites the views need (wanting the layer tiles they read), then sends the
// loaded tiles no longer visible whose content is stored nowhere to their canvas's
// cache (which makes them evictable), and reads the visible tiles that are not loaded, from the cache or else the
// file. Above the occupancy watermark it evicts the tiles shown the longest ago. Transfers are bounded by the free
// slots; visible tiles past what the atlas holds stay unloaded.
void CanvasTilesStream() noexcept;
// The JobExecuteFn of tile reads and writes, on a job thread. A transparent level-0 tile is not written but flagged
// `empty`, and the main thread deletes it.
[[nodiscard]] Result<void> CanvasJobExecute(Job& job) noexcept;

// How views rebuild the canvas between its texels, sharpest last. The wider ones stay sharp where bilinear blurs:
// rotated, or between zoom steps. Mirrors kFilter* in tile.ps.hlsl.
enum struct CanvasFilter : std::uint8_t {
    Nearest,  // 1 tap: hard texels
    Bilinear, // 2x2: soft
    Bicubic,  // 4x4 Catmull-Rom: sharp, a slight halo at hard edges
    Mitchell, // 4x4 Mitchell-Netravali: smooth, almost no halo
    Lanczos2, // 4x4: sharp
    Lanczos3, // 6x6: sharpest, a little ringing at hard edges
    Count_,
};

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
    // Navigation history, apart from the canvas's: earlier viewports, the most recent last, and the ones stepped back
    // from. Each pan, zoom or rotation that ended somewhere else, reset or panel edit is one step.
    std::vector<Viewport> viewportUndo;
    std::vector<Viewport> viewportRedo;
    Viewport viewport;
    CanvasView id{kCanvasViewInvalid}; // kCanvasViewInvalid marks the slot free
    Canvas canvas{kCanvasInvalid};
    Window window{kWindowInvalid};
};

[[nodiscard]] Result<CanvasView> CanvasViewCreate(CanvasViewCreateInfo createInfo) noexcept;
[[nodiscard]] Result<void> CanvasViewDestroy(CanvasView canvasView) noexcept;

} // namespace Midori
