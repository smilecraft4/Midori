#pragma once

#include "base.hpp"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace Midori {

/// Types

using CanvasFileID = uint8_t;
using CanvasViewID = uint8_t;
using WindowID = uint8_t;

using TileAxis = int32_t;
static_assert(std::is_signed<TileAxis>());

using TileMip = uint8_t;
static_assert(std::is_unsigned<TileMip>());

using Layer = uint16_t;
static_assert(std::is_unsigned<Layer>());

using LayerHeight = Layer;
static_assert(std::numeric_limits<LayerHeight>::max() >= std::numeric_limits<Layer>::max());

struct Version {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
};

/// App

constexpr size_t WINDOW_MAX = 16;
static_assert(std::numeric_limits<WindowID>::max() > WINDOW_MAX);

constexpr size_t CANVASVIEW_MAX = 16; // drives the max number of loaded tiles per canvas file
static_assert(std::numeric_limits<CanvasViewID>::max() > CANVASVIEW_MAX);

constexpr size_t CANVASFILE_MAX = 16;
static_assert(std::numeric_limits<CanvasFileID>::max() > CANVASFILE_MAX);

/// Window

constexpr size_t WINDOW_WIDTH_MIN = 0;
constexpr size_t WINDOW_HEIGHT_MIN = 0;
constexpr size_t WINDOW_WIDTH_MAX = 7680;  // drives the max number of loaded tiles per canvas file
constexpr size_t WINDOW_HEIGHT_MAX = 4320; // drives the max number of loaded tiles per canvas file
static_assert(WINDOW_WIDTH_MIN < WINDOW_WIDTH_MAX);
static_assert(WINDOW_HEIGHT_MIN < WINDOW_HEIGHT_MAX);

/// Viewport

constexpr float VIEWPORT_ZOOM_SNAP_INCREMENT = 0.5f;
constexpr float VIEWPORT_ZOOM_EXP_MAX = 4.0f;
constexpr float VIEWPORT_ZOOM_EXP_MIN = -3.0f; // drives the max number of loaded tiles per canvas file
constexpr float VIEWPORT_ZOOM_MAX = Pow(2.0f, VIEWPORT_ZOOM_EXP_MAX);
constexpr float VIEWPORT_ZOOM_MIN = Pow(2.0f, VIEWPORT_ZOOM_EXP_MIN);
constexpr float VIEWPORT_ROTATION_SNAP_INCREMENT = 22.5f * std::numbers::pi_v<float> / 180.0f;
static_assert(VIEWPORT_ZOOM_EXP_MIN < VIEWPORT_ZOOM_EXP_MAX);
static_assert(Mod(VIEWPORT_ZOOM_EXP_MIN, VIEWPORT_ZOOM_SNAP_INCREMENT) == 0.0f);
static_assert(Mod(VIEWPORT_ZOOM_EXP_MAX, VIEWPORT_ZOOM_SNAP_INCREMENT) == 0.0f);

/// Tiles

constexpr size_t TILE_WIDTH = 256;                                       // px
constexpr size_t TILE_HEIGHT = 256;                                      // px
constexpr size_t TILE_PIXEL_SIZE = 4;                                    // B
constexpr size_t TILE_SIZE = TILE_WIDTH * TILE_HEIGHT * TILE_PIXEL_SIZE; // B/tile
constexpr size_t TILE_MAX_UPLOAD_BUFFER = 3;
constexpr size_t TILE_UPLOAD_BUFFER_CAPACITY = 32;
constexpr size_t TILE_MAX_DOWNLOAD_BUFFERS = 3;
constexpr size_t TILE_DOWNLOAD_BUFFER_CAPACITY = 32;

/// Tools

constexpr size_t STROKE_MAX_POINTS = 8192;

/// File

constexpr Version CANVASFILE_VERSION = {.major = 0, .minor = 1, .patch = 0};
constexpr int CANVASFILE_FILE_MODE = 0664;
constexpr size_t CANVASFILE_MAP_SIZE = 1_GiB; // B
constexpr unsigned int CANVASFILE_DBI_COUNT = 3;
constexpr char CANVASFILE_DBI_METADATA[] = "metadata";
constexpr char CANVASFILE_DBI_LAYERS[] = "layers";
constexpr char CANVASFILE_DBI_TILES[] = "tiles";
constexpr char CANVASFILE_KEY_FILE_VERSION[] = "fileVersion";
constexpr size_t CANVASFILE_STAGE_MAP_SIZE = 1_GiB; // B
constexpr uint32_t CANVASFILE_STAGE_DBI_COUNT = 1;
constexpr char CANVASFILE_STAGE_DBI_TILES[] = "tiles";

constexpr uint64_t TILE_KEY_X_BITS = 24ull;
constexpr uint64_t TILE_KEY_X_BITS_OFFSET = 0ull;                // b/tile
constexpr uint64_t TILE_KEY_X_BITS_MASK = ((1ull << TILE_KEY_X_BITS) - 1ull) << TILE_KEY_X_BITS_OFFSET;
constexpr TileAxis TILE_KEY_X_BIAS = 1 << (TILE_KEY_X_BITS - 1); // tile
static_assert(sizeof(TileAxis) * 8 >= TILE_KEY_X_BITS);

constexpr uint64_t TILE_KEY_Y_BITS = 24ull;
constexpr uint64_t TILE_KEY_Y_BITS_OFFSET = TILE_KEY_X_BITS;     // b/tile
constexpr uint64_t TILE_KEY_Y_BITS_MASK = ((1ull << TILE_KEY_Y_BITS) - 1ull) << TILE_KEY_Y_BITS_OFFSET;
constexpr TileAxis TILE_KEY_Y_BIAS = 1 << (TILE_KEY_Y_BITS - 1); // tile
static_assert(sizeof(TileAxis) * 8 >= TILE_KEY_Y_BITS);

constexpr uint64_t TILE_KEY_LAYER_BITS = 14ull;
constexpr uint64_t TILE_KEY_LAYER_BITS_OFFSET = TILE_KEY_Y_BITS_OFFSET + TILE_KEY_Y_BITS; // b/mip
constexpr uint64_t TILE_KEY_LAYER_BITS_MASK = ((1ull << TILE_KEY_LAYER_BITS) - 1ull) << TILE_KEY_LAYER_BITS_OFFSET;

constexpr uint64_t TILE_KEY_MIP_BITS = 2ull;
constexpr uint64_t TILE_KEY_MIP_BITS_OFFSET = TILE_KEY_LAYER_BITS_OFFSET + TILE_KEY_LAYER_BITS; // b/layers
constexpr uint64_t TILE_KEY_MIP_BITS_MASK = ((1ull << TILE_KEY_MIP_BITS) - 1ull) << TILE_KEY_MIP_BITS_OFFSET;

static_assert(TILE_KEY_X_BITS + TILE_KEY_Y_BITS + TILE_KEY_LAYER_BITS + TILE_KEY_MIP_BITS == sizeof(uint64_t) * 8ull);

constexpr int64_t CANVASFILE_X_MAX = TILE_KEY_X_BIAS * TILE_WIDTH;              // px
constexpr int64_t CANVASFILE_X_MIN = -CANVASFILE_X_MAX;                         // px
constexpr int64_t CANVASFILE_Y_MAX = TILE_KEY_Y_BIAS * TILE_HEIGHT;             // px
constexpr int64_t CANVASFILE_Y_MIN = -CANVASFILE_Y_MAX;                         // px
constexpr int64_t CANVASFILE_WIDTH_MAX = (1 << TILE_KEY_X_BITS) * TILE_WIDTH;   // px
constexpr int64_t CANVASFILE_HEIGHT_MAX = (1 << TILE_KEY_Y_BITS) * TILE_HEIGHT; // px

/// Layers

constexpr size_t LAYER_MAX = 4096;
static_assert(LAYER_MAX < (1 << TILE_KEY_LAYER_BITS));
static_assert(LAYER_MAX <= std::numeric_limits<Layer>::max());
constexpr size_t LAYER_NAME_MAX_LEN = 64;
constexpr size_t LAYER_FOLDER_DEPTH_MAX = 3;

} // namespace Midori
