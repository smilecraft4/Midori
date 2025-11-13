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

namespace Midori {

constexpr size_t MAX_RANDOM_BLANK_TEXTURE = 16;
std::vector<std::vector<std::uint8_t>> random_blank_textures;

Canvas::Canvas(App *app) : app(app) {

  random_blank_textures.resize(MAX_RANDOM_BLANK_TEXTURE);
  for (auto &blank_textures : random_blank_textures) {
    {
      ZoneScopedN("Creating random tile texture");
      const std::uint8_t r = rand() % UINT8_MAX;
      const std::uint8_t g = rand() % UINT8_MAX;
      const std::uint8_t b = rand() % UINT8_MAX;
      blank_textures.resize(TILE_SIZE * TILE_SIZE * 4);
      for (size_t i = 0; i < blank_textures.size(); i += 4) {
        blank_textures[i + 0] = r;
        blank_textures[i + 1] = g;
        blank_textures[i + 2] = b;
        blank_textures[i + 3] = UINT8_MAX;
      }
    }
  }
}

bool Canvas::New() {
  constexpr int depth = 4;
  constexpr int width = 4;
  constexpr int height = 2;

  for (int z = 0; z < depth; z++) {
    std::string layer_name = std::format("Layer {}", z);
    const auto layer = CreateLayer(layer_name, depth);
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        const auto tile = CreateTile(layer, glm::ivec2(x, y));
      }
    }
  }

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
      if (layer_infos.at(layer).hidden) {
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

  { // Uploading Queued tile texture
    ZoneScopedN("Uploading Queued tile texture");
    if (!tile_queued.empty()) {
      const size_t tile_uploading = std::min(
          tile_queued.size(), app->renderer.free_tile_upload_offset.size());

      for (size_t i = 0; i < tile_uploading; i++) {
        const Tile tile = *tile_queued.cbegin();
        if (!app->renderer.UploadTileTexture(
                tile,
                random_blank_textures[rand() % MAX_RANDOM_BLANK_TEXTURE])) {
          SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                       "Failed to upload tile texture");
        }
        tile_queued.erase(tile);
      }
    }
  }
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
      .depth = depth,
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
  tile_queued.insert(tile);
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
} // namespace Midori
