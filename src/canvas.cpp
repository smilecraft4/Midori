#include "midori/canvas.h"

#include <SDL3/SDL_assert.h>
#include <cstdint>
#include <format>
#include <numbers>
#include <string>
#include <tracy/Tracy.hpp>
#include <unordered_set>
#include <vector>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

#include "midori/app.h"
#include "midori/renderer.h"
#include "midori/types.h"
#define QOI_IMPLEMENTATION
#include "qoi/qoi.h"

namespace Midori {

Canvas::Canvas(App *app) : app(app) {}

bool Canvas::New() {
  constexpr int depth = 4;

  const auto layer = CreateLayer("Base Layer", 0);

  return true;
}

void Canvas::Quit() {
  // TODO: Find a more elegant way to do this
  std::vector<Layer> layers = Layers();

  for (auto layer : layers) {
    DeleteLayer(layer);
  }
}

void Canvas::Update() {
  ZoneScoped;
  { // Loading/Unloading culled tiles
    ZoneScopedN("Tile culling");
    std::vector<glm::ivec2> visible_tile_positions =
        GetVisibleTilePositions(view, app->window_size);

    for (const auto &layer : Layers()) {
      if (!layer_infos.at(layer).visible) {
        // Should be already culled maybe who knwon
        continue;
      }
      std::unordered_set<glm::ivec2> tile_positions(
          visible_tile_positions.begin(), visible_tile_positions.end());

      // Iterate over every tile in layer
      const std::vector<Tile> layer_tiles = LayerTiles(layer);
      std::unordered_set<Tile> remove_tiles(layer_tiles.begin(),
                                            layer_tiles.end());
      for (const auto &tile : layer_tiles) {
        if (tile_positions.contains(tile_infos.at(tile).position)) {
          tile_positions.erase(tile_infos.at(tile).position);
          remove_tiles.erase(tile);
        }
      }

      for (const auto &tile : remove_tiles) {
        DeleteTile(layer, tile);
      }

      for (const auto &tile_pos : tile_positions) {
        CreateTile(layer, tile_pos);
      }
    }
  }

  UpdateTileLoading();
}

std::vector<Layer> Canvas::Layers() const {
  ZoneScoped;
  std::vector<Layer> layers;
  layers.reserve(layer_infos.size());
  for (const auto &[layer, info] : layer_infos) {
    layers.push_back(layer);
  }
  return layers;
}

bool Canvas::HasLayer(const Layer layer) const {
  ZoneScoped;
  return layer_infos.contains(layer);
}

bool Canvas::LayerHasTile(const Layer layer, const Tile tile) const {
  ZoneScoped;
  return layer_tiles.at(layer).contains(tile);
}

std::vector<Tile> Canvas::LayerTiles(const Layer layer) const {
  ZoneScoped;
  SDL_assert(HasLayer(layer) && "Layer not found");

  std::vector<Tile> tiles;
  tiles.reserve(layer_tiles.at(layer).size());
  for (const auto &tile : layer_tiles.at(layer)) {
    SDL_assert(tile != 0 && "Invalid tile found");
    tiles.push_back(tile);
  }

  return tiles;
}

Layer Canvas::CreateLayer(const std::string &name, const std::uint8_t depth) {
  ZoneScoped;
  Layer layer = 0;

  if (unassigned_layers.empty()) {
    if (last_assigned_layer >= LAYER_MAX) {
      SDL_assert(false && "Max layer reached");
      return 0;
    }
    // It's fine to increment first because 0 is invalid so the first assigned
    // layer is always 1
    last_assigned_layer++;
    layer = last_assigned_layer;
  } else {
    layer = unassigned_layers.back();
    unassigned_layers.pop_back();
  }

  SDL_assert(!HasLayer(layer) && "Layer already exists");

  if (!app->renderer.CreateLayerTexture(layer)) {
    unassigned_layers.push_back(layer);
    return 0;
  }

  layer_infos[layer] = LayerInfo{
      .name = name,
      .opacity = 1.0f,
      .layer = layer,
      .depth = depth,
      .blend_mode = BlendMode::Normal,
      .visible = true,
      .internal = false,
      .locked = false,
  };
  layer_tiles[layer] = std::unordered_set<Tile>();

  return layer;
}

void Canvas::DeleteLayer(const Layer layer) {
  ZoneScoped;
  SDL_assert(HasLayer(layer) && "Layer not found");

  std::vector<Tile> tiles = LayerTiles(layer);
  for (const auto tile : tiles) {
    DeleteTile(layer, tile);
  }

  app->renderer.DeleteLayerTexture(layer);

  unassigned_layers.push_back(layer);
  layer_infos.erase(layer);
  layer_tiles.erase(layer);
}

Tile Canvas::CreateTile(const Layer layer, const glm::ivec2 position) {
  ZoneScoped;
  SDL_assert(HasLayer(layer) && "Layer not found");
  Tile tile = 0;

  if (unassigned_tiles.empty()) {
    if (last_assigned_tile >= TILE_MAX) {
      SDL_assert(false && "Max tile reached");
      return 0;
    }
    // It's fine to increment first because 0 is invalid so the first assigned
    // tile is always 1
    last_assigned_tile++;
    tile = last_assigned_tile;
  } else {
    tile = unassigned_tiles.back();
    unassigned_tiles.pop_back();
  }

  SDL_assert(!LayerHasTile(layer, tile) && "Layer already has tile");

  if (!app->renderer.CreateTileTexture(tile)) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tile texture");
    return 0;
  }
  tile_load_queue[tile] = TileLoadStatus{
      .layer = layer,
      .tile = tile,
      .state = TileLoadState::Queued,
  };
  layer_tiles.at(layer).insert(tile);

  tile_infos[tile] = TileInfo{
      .layer = layer,
      .position = position,
  };

  return tile;
}

void Canvas::DeleteTile(const Layer layer, const Tile tile) {
  ZoneScoped;
  SDL_assert(HasLayer(layer) && "Layer not found");
  SDL_assert(LayerHasTile(layer, tile) && "Layer does not have tile");

  app->renderer.DeleteTileTexture(tile);
  unassigned_tiles.push_back(tile);
  tile_infos.erase(tile);

  layer_tiles.at(layer).erase(tile);
}

std::vector<glm::ivec2>
Canvas::GetVisibleTilePositions(const View &view, const glm::vec2 size) const {
  ZoneScoped;
  constexpr auto tile_size = glm::vec2(TILE_SIZE);
  const glm::ivec2 t_min = glm::floor((-view.pan - (size / 2.0f)) / tile_size);
  const glm::ivec2 t_max = glm::ceil((-view.pan + (size / 2.0f)) / tile_size);
  const glm::ivec2 t_num = t_max - t_min;

  std::vector<glm::ivec2> positions;
  positions.reserve(std::size_t(t_num.x) * std::size_t(t_num.y));

  for (auto y = t_min.y; y < t_max.y; y++) {
    for (auto x = t_min.x; x < t_max.x; x++) {
      positions.emplace_back(x, y);
    }
  }

  return positions;
}

void Canvas::ViewUpdateState(bool pan, bool zoom, bool rotate) {
  if (view_panning && (!pan || (pan && zoom) || (pan && rotate))) {
    ViewPanStop();
  }
  if (view_zooming && (!pan || !zoom || (pan && rotate))) {
    ViewZoomStop();
  }
  if (view_rotating && (!pan || !rotate || (pan && zoom))) {
    ViewRotateStop();
  }

  if (!view_panning && (pan && !zoom && !rotate)) {
    ViewPanStart();
  }
  if (!view_zooming && (pan && zoom && !rotate)) {
    ViewZoomStart(app->cursor_current_pos);
  }
  if (!view_rotating && (pan && rotate && !zoom)) {
    ViewRotateStart(app->cursor_current_pos);
  }
}

void Canvas::ViewUpdateCursor(glm::vec2 cursor_pos, glm::vec2 cursor_delta) {
  if (view_panning) {
    ViewPan(view.pan + cursor_delta);
  } else if (view_rotating) {
    ViewRotate(view.rotation + (cursor_delta.x / 360.0f * std::numbers::pi));
  } else if (view_zooming) {
    ViewZoom(view.zoom_amount + (cursor_delta.x / 1000.0f));
  }
}

void Canvas::ViewPanStart() {
  SDL_assert(!view_panning);
  SDL_assert(!view_rotating);
  SDL_assert(!view_zooming);

  view_panning = true;
}
void Canvas::ViewPan(glm::vec2 amount) {
  SDL_assert(view_panning);
  view.pan = amount;
}
void Canvas::ViewPanStop() {
  SDL_assert(view_panning);

  view_panning = false;
}

void Canvas::ViewRotateStart(glm::vec2 start) {
  SDL_assert(!view_panning);
  SDL_assert(!view_rotating);
  SDL_assert(!view_zooming);

  view.rotate_start = start;
  view_rotating = true;
}
void Canvas::ViewRotate(float amount) {
  SDL_assert(view_rotating);
  view.rotation = amount;
}
void Canvas::ViewRotateStop() {
  SDL_assert(view_rotating);

  view_rotating = false;
}

void Canvas::ViewZoomStart(glm::vec2 origin) {
  SDL_assert(!view_panning);
  SDL_assert(!view_rotating);
  SDL_assert(!view_zooming);

  view.zoom_origin = origin;
  view_zooming = true;
}
void Canvas::ViewZoom(float amount) {
  SDL_assert(view_zooming);

  view.zoom_amount = amount;
}
void Canvas::ViewZoomStop() {
  SDL_assert(view_zooming);

  view_zooming = false;
}

void Canvas::UpdateTileLoading() {
  ZoneScoped;

  std::vector<Tile> tiles_unqueued;
  size_t i = 0;
  for (auto &[tile, tile_load] : tile_load_queue) {
    if (i >= Midori::Renderer::TILE_MAX_UPLOAD_TRANSFER) {
      break;
    }

    if (!HasLayer(tile_load.layer)) {
      tiles_unqueued.push_back(tile);
      continue;
    } else if (!LayerHasTile(tile_load.layer, tile_load.tile)) {
      tiles_unqueued.push_back(tile);
      continue;
    }

    // TODO: Make this atrocity multithreaded/async
    if (tile_load.state == TileLoadState::Queued) {

      std::string file =
          std::format("./data/tiles/{}.qoi", tile_load.layer % 3);

      ZoneScopedN("Reading Tile");
      size_t buf_size;
      auto *buf = (std::uint8_t *)SDL_LoadFile(file.c_str(), &buf_size);

      SDL_assert(buf_size > 0);
      SDL_assert(buf != nullptr);

      tile_load.read_texture.resize(buf_size);
      memcpy(tile_load.read_texture.data(), buf, buf_size);
      SDL_free(buf);

      tile_load.state = TileLoadState::Read;
    }
    if (tile_load.state == TileLoadState::Read) {
      ZoneScopedN("Decoding Tile");
      qoi_desc desc;
      auto *buf =
          (std::uint8_t *)qoi_decode(tile_load.read_texture.data(),
                                     tile_load.read_texture.size(), &desc, 4);
      SDL_assert(buf != nullptr);
      SDL_assert(desc.channels = 4);
      SDL_assert(desc.width = TILE_SIZE);
      SDL_assert(desc.height = TILE_SIZE);
      tile_load.read_texture.clear();

      tile_load.raw_texture.resize(TILE_SIZE * TILE_SIZE * 4);
      memcpy(tile_load.raw_texture.data(), buf, TILE_SIZE * TILE_SIZE * 4);
      free(buf);

      tile_load.state = TileLoadState::Decompressed;
    }
    if (tile_load.state == TileLoadState::Decompressed) {
      ZoneScopedN("Uploading Tile");
      if (!app->renderer.UploadTileTexture(tile, tile_load.raw_texture)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Failed to upload tile texture");
      } else {
        tile_load.state = TileLoadState::Uploaded;
        tiles_unqueued.push_back(tile);
      }
    }
    i++;
  }
  for (const auto &tile : tiles_unqueued) {
    tile_load_queue.erase(tile);
  }
}

} // namespace Midori
