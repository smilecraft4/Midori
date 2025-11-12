#include "midori/canvas.h"

#include <SDL3/SDL_assert.h>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <tracy/Tracy.hpp>
#include <unordered_set>
#include <vector>

#include "SDL3/SDL_log.h"
#include "midori/app.h"
#include "midori/renderer.h"
#include "midori/types.h"

namespace Midori {

Canvas::Canvas(App *app) : app(app) {}

bool Canvas::New() {
  constexpr int depth = 16;
  constexpr int width = 4;
  constexpr int height = 4;

  for (int z = 0; z < depth; z++) {
    std::string layer_name = std::format("Layer {}", z);
    const auto layer = CreateLayer(layer_name, depth);
    for (int y = -height / 2; y < height / 2; y++) {
      for (int x = -width / 2; x < width / 2; x++) {
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
  if (!tile_queued.empty()) {
    std::vector<Tile> scheduled_tile;
    scheduled_tile.reserve(app->renderer.free_tile_upload_offset.size());
    for (const auto &tile : tile_queued) {
      const std::uint8_t r = rand() % UINT8_MAX;
      const std::uint8_t g = rand() % UINT8_MAX;
      const std::uint8_t b = rand() % UINT8_MAX;
      std::vector<std::uint8_t> tile_blank_texture(TILE_SIZE * TILE_SIZE * 4,
                                                   0x00);
      for (size_t i = 0; i < tile_blank_texture.size(); i += 4) {
        tile_blank_texture[i + 0] = r;
        tile_blank_texture[i + 1] = g;
        tile_blank_texture[i + 2] = b;
        tile_blank_texture[i + 3] = UINT8_MAX;
      }
      if (!app->renderer.UploadTileTexture(tile, tile_blank_texture)) {
        break;
      }
      scheduled_tile.push_back(tile);
    }
    for (const auto &tile : scheduled_tile) {
      tile_queued.erase(tile);
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

// TODO: To Implement
Layer Canvas::GetLayer(std::uint8_t depth) const {
  SDL_assert(false && "To implement");
  return 0;
}

// TODO: To Implement
Layer Canvas::GetLayer(const std::string &name) const {
  SDL_assert(false && "To implement");
  return 0;
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

  // TODO: Handle layer depth

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

  // TODO: handle layer depth

  unassigned_layers.push_back(layer);
  layer_infos.erase(layer);
  layer_tiles.erase(layer);
}

// TODO: To implement
Tile Canvas::GetTile(const Layer layer, const glm::ivec2 position) const {
  SDL_assert(false && "To implement");
  return 0;
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

  // TODO: Multithreading the tile uploading procedure
  if (!app->renderer.CreateTileTexture(tile)) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create tile texture");
    return 0;
  }
  tile_queued.insert(tile);

  SDL_assert(!LayerHasTile(layer, tile) && "Layer already has tile");

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

} // namespace Midori
