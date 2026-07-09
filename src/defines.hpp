#pragma once

#include "base.hpp"

constexpr size_t WINDOW_WIDTH_MIN = 0;
constexpr size_t WINDOW_HEIGHT_MIN = 0;
constexpr size_t WINDOW_WIDTH_MAX = 7680;
constexpr size_t WINDOW_HEIGHT_MAX = 4320;
static_assert(WINDOW_WIDTH_MIN < WINDOW_WIDTH_MAX);
static_assert(WINDOW_HEIGHT_MIN < WINDOW_HEIGHT_MAX);

constexpr float VIEWPORT_ZOOM_SNAP_INCREMENT = 0.5f;
constexpr float VIEWPORT_ZOOM_EXP_MAX = 4.0f;
constexpr float VIEWPORT_ZOOM_EXP_MIN = -3.5f;
constexpr float VIEWPORT_ZOOM_MAX = Pow(2.0f, VIEWPORT_ZOOM_EXP_MAX);
constexpr float VIEWPORT_ZOOM_MIN = Pow(2.0f, VIEWPORT_ZOOM_EXP_MIN);
constexpr float VIEWPORT_ROTATION_SNAP_INCREMENT = 22.5f * std::numbers::pi_v<float> / 180.0f;
static_assert(VIEWPORT_ZOOM_EXP_MIN < VIEWPORT_ZOOM_EXP_MAX);
static_assert(Mod(VIEWPORT_ZOOM_EXP_MIN, VIEWPORT_ZOOM_SNAP_INCREMENT) == 0.0f);
static_assert(Mod(VIEWPORT_ZOOM_EXP_MAX, VIEWPORT_ZOOM_SNAP_INCREMENT) == 0.0f);

constexpr size_t LAYER_MAX = UINT16_MAX;
constexpr size_t LAYER_FOLDER_DEPTH_MAX = 3;
constexpr size_t LAYER_FOLDER_CHILD_MAX = 32;
static_assert(Pow(LAYER_FOLDER_CHILD_MAX, LAYER_FOLDER_DEPTH_MAX) < LAYER_MAX);

constexpr size_t TILES_MAX = UINT16_MAX;
constexpr size_t TILE_WIDTH = 256;
constexpr size_t TILE_HEIGHT = 256;
constexpr size_t TILE_PIXEL_SIZE = 4;
constexpr size_t TILE_SIZE = TILE_WIDTH * TILE_HEIGHT * TILE_PIXEL_SIZE;
