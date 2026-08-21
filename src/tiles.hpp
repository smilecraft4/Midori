#pragma once

#include "base.hpp"
#include "defines.hpp"
#include "errors.hpp"
#include "colors.hpp"
#include <SDL3/SDL_assert.h>
#include <cstdint>

namespace Midori {

// How to notify when a tile downloaded comes empty ?
// Ideally it should be deleted
// This checks happens in tileTransfers

enum class TileState : uint8_t {
    None = 0,
    Loaded = 1 << 0,   // has a gpu texture and can be drawn
    Modified = 1 << 1, // differs from the canvas file, needs downloading when saving
    Staged = 1 << 2,   // a copy is cached in the stage file
    Deleted = 1 << 3,  // is staged as deleted
    All = Loaded | Modified | Staged | Deleted,
};

constexpr bool HasAll(TileState state, TileState flags) noexcept {
    return (state & flags) == flags;
}

constexpr bool HasAny(TileState state, TileState flags) noexcept {
    return (state & flags) != TileState::None;
}

enum class TileWriteState : uint8_t {
    None,
    Pending,
    Downloaded,
    Encoding,
    Encoded,
    Written,
    Error,
    Done,
};

enum class TileReadState : uint8_t {
    None,
    Pending,
    Read,
    Decoded,
    Uploaded,
    Error,
    Done,
};

struct TileInfo {
    TileState state;
    uint32_t frame;
};

struct Tile {
    CanvasFileID canvasFile;
    TileMip mip;
    Layer layer;
    TileAxis y;
    TileAxis x;

    constexpr bool operator==(const Tile& other) const noexcept {
        return layer == other.layer && y == other.y && x == other.x && canvasFile == other.canvasFile
            && mip == other.mip;
    }
};

constexpr uint64_t PackTileKey(Tile tile) noexcept {
    SDL_assert(tile.mip < (1ull << TILE_KEY_MIP_BITS) && "Out of bit range");
    SDL_assert(tile.x < TILE_KEY_X_BIAS && "Out of bit range");
    SDL_assert(tile.x >= -TILE_KEY_X_BIAS && "Out of bit range");
    SDL_assert(tile.y < TILE_KEY_Y_BIAS && "Out of bit range");
    SDL_assert(tile.y >= -TILE_KEY_Y_BIAS && "Out of bit range");
    SDL_assert(tile.layer < (1ull << TILE_KEY_LAYER_BITS) && "Out of bit range");

    const uint64_t tileKey = ((uint64_t(tile.mip) << TILE_KEY_MIP_BITS_OFFSET) & TILE_KEY_MIP_BITS_MASK)
                           | ((uint64_t(tile.layer) << TILE_KEY_LAYER_BITS_OFFSET) & TILE_KEY_LAYER_BITS_MASK)
                           | ((uint64_t(tile.y + TILE_KEY_Y_BIAS) << TILE_KEY_Y_BITS_OFFSET) & TILE_KEY_Y_BITS_MASK)
                           | ((uint64_t(tile.x + TILE_KEY_X_BIAS) << TILE_KEY_X_BITS_OFFSET) & TILE_KEY_X_BITS_MASK);

    return tileKey;
}

constexpr Tile UnpackTileKey(uint64_t tileKey, CanvasFileID canvasFile) noexcept {
    const Tile tile{
        .canvasFile = canvasFile,
        .mip = (TileMip)((tileKey & TILE_KEY_MIP_BITS_MASK) >> TILE_KEY_MIP_BITS_OFFSET),
        .layer = (Layer)((tileKey & TILE_KEY_LAYER_BITS_MASK) >> TILE_KEY_LAYER_BITS_OFFSET),
        .y = (TileAxis)((tileKey & TILE_KEY_Y_BITS_MASK) >> TILE_KEY_Y_BITS_OFFSET) - TILE_KEY_Y_BIAS,
        .x = (TileAxis)((tileKey & TILE_KEY_X_BITS_MASK) >> TILE_KEY_X_BITS_OFFSET) - TILE_KEY_X_BIAS,
    };

    return tile;
}

static_assert(UnpackTileKey(PackTileKey(Tile{.mip = 3, .layer = LAYER_MAX, .y = 0x7fffff, .x = -0x800000}), 0)
              == Tile{.canvasFile = 0, .mip = 3, .layer = LAYER_MAX, .y = 0x7fffff, .x = -0x800000});

struct TileCommand {
    enum class Type : uint8_t {
        None,
        New,
        Load,
        Duplicate,
        Paint,
        Erase,
        Merge,
        Render,
        Stage,
        Destroy,
        Delete,
    };

    Type type;
    uint32_t seq;
    Tile srcTile{};
    Tile dstTile{};

    /// Snapshot of the source layer taken when the command is recorded, so executing it does not need that layer to
    /// still be alive. Merge only.
    // Use a tagged union instead if more are added
    BlendMode blendMode{};
    float opacity{1.0f};
};

struct TileStageBufferSlot {
    uint8_t buf;
    uint8_t slot;
};

struct TileTransfer {
    using ID = uint16_t;

    /// Marks a WriteCommand that is not tied to a tile transfer (layer, metadata and staged deletions)
    static constexpr ID NONE = UINT16_MAX;

    std::atomic<TileReadState> read;
    std::atomic<TileWriteState> write;
    Error error;
    bool canceled;
    TileStageBufferSlot slot;
    Tile tile;
};

} // namespace Midori

template<>
struct std::hash<Midori::Tile> {
    size_t operator()(const Midori::Tile& t) const {
        return Midori::Splitmix64(PackTileKey(t) ^ (uint64_t(t.canvasFile) << 62));
    }
};
