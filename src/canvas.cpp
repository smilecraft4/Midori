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

static const std::vector<std::uint8_t> blank_texture(TILE_SIZE *TILE_SIZE * 4,
                                                     0xef);

Canvas::Canvas(App *app) : app(app) {
  std::memset((std::uint8_t *)blank_texture.data(), 128,
              TILE_SIZE * TILE_SIZE * 4);
}

bool Canvas::New() {
  ZoneScoped;
  Quit();

  file.header.name[0] = 'm';
  file.header.name[1] = 'i';
  file.header.name[2] = 'd';
  file.header.name[3] = 'o';
  file.header.version[0] = 0;
  file.header.version[1] = 0;
  file.header.version[2] = 0;
  file.header.version[3] = 0;

  const Layer layer = CreateLayer("Base Layer", 0);
  selected_layer = layer;

  return true;
}

bool Canvas::SaveAs(const std::filesystem::path &filename) {
  ZoneScoped;
  file.filename = filename;
  file.saved = false;

  if (!Save()) {
    return false;
  }

  return true;
}

bool Canvas::Save() {
  ZoneScoped;
  if (!file.filename.has_filename()) {
    return false;
  }

  if (file.saved) {
    return true;
  }

  const auto filestring = file.filename.string();
  SDL_IOStream *file_io = SDL_IOFromFile(filestring.c_str(), "wb");
  if (file_io == nullptr) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open file:s '%s'",
                 filestring.c_str());
    return false;
  }
  {
    ZoneScopedN("Writing Layers");
    SDL_WriteIO(file_io, &file.header, sizeof(file.header));

    const std::uint32_t layer_num = file.layers.size();
    SDL_WriteIO(file_io, &layer_num, sizeof(layer_num));
    for (const auto &[layer, file_layer] : file.layers) {
      {
        ZoneScopedN("Writing Layer");
        const std::uint16_t layer_name_len = file_layer.layer.name.size();
        SDL_WriteIO(file_io, &layer_name_len, sizeof(layer_name_len));
        SDL_WriteIO(file_io, file_layer.layer.name.data(), layer_name_len);
        SDL_WriteIO(file_io, &file_layer.layer.depth,
                    sizeof(file_layer.layer.depth));
      }
      {
        ZoneScopedN("Writing Tiles");
        const std::uint32_t tile_num = file_layer.tile_saved.size();
        SDL_WriteIO(file_io, &tile_num, sizeof(tile_num));
        for (const auto &tile_pos : file_layer.tile_saved) {
          FileTile file_tile = {
              .position = tile_pos,
          };
          SDL_WriteIO(file_io, &file_tile, sizeof(file_tile));
        }
      }
    }
  }
  SDL_CloseIO(file_io);
  file.saved = true;

  return true;
}

bool Canvas::Open(const std::filesystem::path &filename) {
  ZoneScoped;
  if (file.filename.has_filename() && !file.saved) {
    // Return a different error here prompting the user to save the current file
    return false;
  }

  if (!std::filesystem::exists(filename)) {
    return false;
  }
  Canvas::Quit();

  const auto filestring = filename.string();
  SDL_IOStream *file_io = SDL_IOFromFile(filestring.c_str(), "rb");
  if (file_io == nullptr) {
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open file:s '%s'",
                 filestring.c_str());
    return false;
  }

  SDL_ReadIO(file_io, &file.header, sizeof(file.header));

  {
    ZoneScopedN("Reading Layers");
    std::uint32_t layer_num;
    SDL_ReadIO(file_io, &layer_num, sizeof(layer_num));
    Layer layer;
    std::unordered_map<Layer, std::uint8_t> real_layer_depth;
    real_layer_depth.reserve(layer_num);
    for (size_t i = 0; i < layer_num; i++) {
      {
        ZoneScopedN("Reading Layer");
        LayerInfo layer_info;
        std::uint16_t layer_name_len;
        SDL_ReadIO(file_io, &layer_name_len, sizeof(layer_name_len));
        layer_info.name.resize(layer_name_len);
        SDL_ReadIO(file_io, layer_info.name.data(), layer_name_len);
        SDL_ReadIO(file_io, &layer_info.depth, sizeof(layer_info.depth));
        layer = CreateLayer(layer_info.name, 0);
        real_layer_depth[layer] = layer_info.depth;
      }

      {
        ZoneScopedN("Reading Tile");
        std::uint32_t tile_num = 0;
        SDL_ReadIO(file_io, &tile_num, sizeof(tile_num));
        std::vector<FileTile> file_tiles(tile_num);
        SDL_ReadIO(file_io, file_tiles.data(), sizeof(FileTile) * tile_num);
        for (auto file_tile : file_tiles) {
          file.layers.at(layer).tile_saved.insert(file_tile.position);
        }
      }
    }
    for (auto [layer, depth] : real_layer_depth) {
      SetLayerDepth(layer, depth);
    }
  }

  SDL_CloseIO(file_io);

  file.filename = filename;
  file.saved = true;

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
        if (LayerHasTileFile(layer, tile_pos))
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

  current_max_layer_height++;
  SDL_assert(depth <= current_max_layer_height);
  for (auto &[other_layer, other_layer_info] : layer_infos) {
    if (other_layer == layer) {
      continue;
    }
    if (other_layer_info.depth >= depth) {
      other_layer_info.depth++;
      file.layers[layer].layer.depth++;
    }

    SDL_assert(other_layer_info.depth < current_max_layer_height);
  }

  const auto layer_info = LayerInfo{
      .name = name,
      .opacity = 1.0f,
      .layer = layer,
      .depth = depth,
      .blend_mode = BlendMode::Normal,
      .visible = true,
      .internal = false,
      .locked = false,
  };

  layer_infos[layer] = layer_info;
  layer_tiles[layer] = std::unordered_set<Tile>();
  layer_tile_pos[layer] = std::unordered_map<glm::ivec2, Tile>();

  file.layers[layer] = FileLayer{
      .layer = layer_info,
      .tile_saved = std::unordered_set<glm::ivec2>(),
  };

  return layer;
}

bool Canvas::SetLayerDepth(const Layer layer, std::uint8_t depth) {
  SDL_assert(file.layers.contains(layer) && "Layer not found");

  // No new layer are created nor destroyed so the max should stay the same
  depth = std::min(current_max_layer_height, depth);

  const auto old_depth = layer_infos[layer].depth;

  const std::int8_t direction = (old_depth > depth) ? +1 : -1;
  const auto min_depth = std::min(old_depth, depth);
  const auto max_depth = std::max(old_depth, depth);

  for (auto &[other_layer, other_layer_info] : layer_infos) {
    if (other_layer == layer) {
      continue;
    }
    const auto other_depth = other_layer_info.depth;
    if (min_depth <= other_depth && other_depth <= max_depth) {
      other_layer_info.depth += direction;
      file.layers[other_layer].layer.depth += direction;
    }
    SDL_assert(other_depth < current_max_layer_height);
  }

  layer_infos[layer].depth = depth;
  file.layers[layer].layer.depth = depth;
  file.saved = false;

  return true;
}

std::uint8_t Canvas::GetLayerDepth(const Layer layer) const {
  SDL_assert(file.layers.contains(layer) && "Layer not found");
  return file.layers.at(layer).layer.depth;
}

// TODO: Make this a queued action
void Canvas::DeleteLayer(const Layer layer) {
  ZoneScoped;
  SDL_assert(HasLayer(layer) && "Layer not found");

  std::vector<Tile> tiles = LayerTiles(layer);
  for (const auto tile : tiles) {
    DeleteTile(layer, tile);
  }

  app->renderer.DeleteLayerTexture(layer);

  current_max_layer_height--;
  for (auto &[layer_below, layer_below_info] : layer_infos) {
    if (layer_below == layer) {
      continue;
    }
    if (layer_below_info.depth > layer_infos.at(layer).depth) {
      layer_below_info.depth--;
      file.layers[layer].layer.depth--;
    }
    SDL_assert(layer_below_info.depth < current_max_layer_height);
  }

  unassigned_layers.push_back(layer);
  layer_infos.erase(layer);
  layer_tiles.erase(layer);
  layer_tile_pos.erase(layer);
  file.layers.erase(layer);

  if (selected_layer == layer) {
    selected_layer = 0;
  }
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
  layer_tile_pos.at(layer)[position] = tile;

  tile_infos[tile] = TileInfo{
      .layer = layer,
      .position = position,
  };

  return tile;
}

void Canvas::DeleteTile(Layer layer, glm::ivec2 position) {
  ZoneScoped;
  for (const auto &tile : layer_tiles.at(layer)) {
    if (tile_infos.at(tile).position == position) {
      DeleteTile(layer, tile);
      return;
    }
  }
}

Tile Canvas::GetTileAt(const Layer layer, const glm::ivec2 position) const {
  ZoneScoped;
  SDL_assert(layer_tile_pos.contains(layer));

  Tile tile = 0;
  if (layer_tile_pos.at(layer).contains(position)) {
    tile = layer_tile_pos.at(layer).at(position);
  }

  return tile;
}

// TODO: Make this a queued action
void Canvas::DeleteTile(const Layer layer, const Tile tile) {
  ZoneScoped;
  SDL_assert(HasLayer(layer) && "Layer not found");
  SDL_assert(LayerHasTile(layer, tile) && "Layer does not have tile");

  app->renderer.DeleteTileTexture(tile);
  unassigned_tiles.push_back(tile);

  layer_tile_pos.at(layer).erase(tile_infos.at(tile).position);
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

      const auto layer_depth = file.layers.at(tile_load.layer).layer.depth;
      std::string file = std::format("./data/tiles/{}.qoi", layer_depth % 3);

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
      if (!app->renderer.UploadTileTexture(tile, blank_texture)) {
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

bool Canvas::LayerHasTileFile(const Layer layer, const glm::ivec2 tile_pos) {
  return file.layers.at(layer).tile_saved.contains(tile_pos);
}

static glm::ivec2 GetTilePos(const glm::vec2 canvas_pos) {
  constexpr glm::vec2 tile_size = glm::vec2(TILE_SIZE);
  return glm::ivec2(glm::floor(canvas_pos / tile_size));
}

// This assumes every painted tiles are not going to be culled
void Canvas::StartStroke(StrokePoint point) {
  const auto tile_pos = GetTilePos(point.position);
  Tile tile = GetTileAt(selected_layer, tile_pos);
  if (tile == 0) {
    tile = CreateTile(selected_layer, tile_pos);
  }
  stroke_tile_affected.insert(tile);

  previous_point = point;
  stroke_points.push_back(point);
  stroke_options.points_num++;
}

// This assumes every painted tiles are not going to be culled
void Canvas::UpdateStroke(StrokePoint point, const float spacing) {
  const auto distance = glm::distance(point.position, previous_point.position);
  float process_distance = spacing;
  const float t_step = spacing / distance;
  stroke_points.reserve(stroke_points.size() +
                        std::size_t(std::ceil(1.0f / t_step)));

  const auto start = stroke_options.points_num;
  std::unordered_set<glm::ivec2> processed_tile_pos;
  float t = t_step;
  while (t < 1.0f) {
    StrokePoint lerped_point;
    lerped_point.position.x =
        std::lerp(previous_point.position.x, point.position.x, t);
    lerped_point.position.y =
        std::lerp(previous_point.position.y, point.position.y, t);
    stroke_points.push_back(lerped_point);
    stroke_options.points_num++;
    t += t_step;

    const auto tile_pos = GetTilePos(point.position);
    if (!processed_tile_pos.contains(tile_pos)) {
      processed_tile_pos.insert(tile_pos);
      Tile tile = GetTileAt(selected_layer, tile_pos);
      if (tile == 0) {
        tile = CreateTile(selected_layer, tile_pos);
      }
      stroke_tile_affected.insert(tile);
    }
  }
  previous_point = point;
}

// This assumes every painted tiles are not going to be culled
void Canvas::EndStroke(StrokePoint point) {
  const auto tile_pos = GetTilePos(point.position);
  Tile tile = GetTileAt(selected_layer, tile_pos);
  if (tile == 0) {
    tile = CreateTile(selected_layer, tile_pos);
  }
  stroke_tile_affected.insert(tile);

  previous_point = point;
  stroke_points.push_back(point);
  stroke_options.points_num++;
}
} // namespace Midori
