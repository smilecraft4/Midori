#include "midori/canvas.h"

#include <SDL3/SDL_assert.h>
#include <cstdint>
#include <format>
#include <imgui.h>
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
                                                     0x00);

Canvas::Canvas(App *app) : app(app) {
  std::memset((std::uint8_t *)blank_texture.data(), 0,
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

void Canvas::MergeLayer(const Layer over_layer, const Layer below_layer) {
  SDL_assert(layer_infos.contains(over_layer));
  SDL_assert(layer_infos.contains(below_layer));

  // Do this for only visible tiles for now
  std::vector<std::pair<Tile, Tile>> tile_to_merge;
  std::vector<Tile> tile_to_move;
  for (const auto &over_tile : layer_tiles.at(over_layer)) {
    const auto tile_merge_pos = tile_infos.at(over_tile).position;
    const auto below_tile = layer_tile_pos.at(below_layer).at(tile_merge_pos);
    tile_to_merge.emplace_back(over_tile, below_tile);
  }
  for (const auto &[over, below] : tile_to_merge) {
    MergeTiles(over, below);
    // DeleteTile(over_layer, over);
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
  layer_tile_pos.at(layer).emplace(position, tile);

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

  layer_tiles.at(layer).erase(tile);
  layer_tile_pos.at(layer).erase(tile_infos.at(tile).position);
  tile_infos.erase(tile);
}

void Canvas::MergeTiles(Tile over_tile, Tile below_tile) {
  // TODO: make this a queued action and only delete tile after rendering
  app->renderer.MergeTileTextures(over_tile, below_tile);
  // DeleteTile(tile_infos.at(over_tile).layer, over_tile);
}

void Canvas::MoveTile(Tile tile, Layer new_layer) {
  SDL_assert(false && "temporary");
  SDL_assert(tile_infos.contains(tile));
  const auto old_tile_info = tile_infos.at(tile);

  SDL_assert(layer_infos.contains(old_tile_info.layer));
  layer_tiles.at(old_tile_info.layer).erase(tile);
  layer_tile_pos.at(old_tile_info.layer).erase(old_tile_info.position);

  SDL_assert(layer_infos.contains(new_layer));
  SDL_assert(!layer_tiles.at(new_layer).contains(tile));
  tile_infos.at(tile).layer = new_layer;
  layer_tiles.at(new_layer).insert(tile);
  layer_tile_pos.at(new_layer).emplace(old_tile_info.position, tile);
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
  app->renderer.view_changed = true;
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
  app->renderer.view_changed = true;
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
  app->renderer.view_changed = true;
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

std::vector<glm::ivec2>
GetTilePosAffectedByStrokePoint(Canvas::StrokePoint point) {
  constexpr glm::vec2 tile_size = glm::vec2(TILE_SIZE);
  std::vector<glm::ivec2> tiles_pos;

  glm::ivec2 tile_pos_min =
      glm::floor((point.position - point.radius) / tile_size);
  glm::ivec2 tile_pos_max =
      glm::ceil((point.position + point.radius) / tile_size);

  const glm::ivec2 tile_distance = tile_pos_max - tile_pos_min;
  tiles_pos.reserve(tile_distance.x * tile_distance.y);
  glm::ivec2 tile_pos;
  for (tile_pos.y = tile_pos_min.y; tile_pos.y < tile_pos_max.y; tile_pos.y++) {
    for (tile_pos.x = tile_pos_min.x; tile_pos.x < tile_pos_max.x;
         tile_pos.x++) {
      tiles_pos.push_back(tile_pos);
    }
  }

  return tiles_pos;
}

Canvas::StrokePoint Canvas::ApplyPressure(StrokePoint point, float pressure) {
  StrokePoint pressure_point = point;

  // TODO: Add min max value

  if (brush_options.opacity_pressure) {
    pressure_point.color *= pressure; // We use premultiplied alpha
  }
  if (brush_options.radius_pressure) {
    pressure_point.radius *= pressure;
  }
  if (brush_options.flow_pressure) {
    pressure_point.flow *= pressure;
  }
  if (brush_options.hardness_pressure) {
    pressure_point.hardness *= pressure;
  }

  return pressure_point;
}

// This assumes every painted tiles are not going to be culled
void Canvas::StartStroke(StrokePoint point) {
  ZoneScoped;
  SDL_assert(!stroke_started);
  stroke_started = true;

  if (app->pen_in_range) {
    point = ApplyPressure(point, app->pen_pressure);
  }

  SDL_assert(stroke_layer == 0);
  // Create a temporary layer above the selected layer and set it as active
  stroke_layer =
      CreateLayer("Stroke Layer", layer_infos.at(selected_layer).depth);
  SDL_assert(stroke_layer != 0);

  const auto tiles_pos = GetTilePosAffectedByStrokePoint(point);
  for (const auto &tile_pos : tiles_pos) {
    Tile tile = GetTileAt(stroke_layer, tile_pos);
    if (tile == 0) {
      tile = CreateTile(stroke_layer, tile_pos);
    }
    if (!layer_tile_pos.at(selected_layer).contains(tile_pos)) {
      tile = CreateTile(selected_layer, tile_pos);
    }
    stroke_tile_affected.insert(tile);
    // app->renderer.tile_to_draw.insert(tile);
  }

  previous_point = point;
  stroke_points.push_back(point);
}

// This assumes every painted tiles are not going to be culled
void Canvas::UpdateStroke(StrokePoint point) {
  ZoneScoped;
  SDL_assert(stroke_started);
  SDL_assert(stroke_layer != 0);

  if (app->pen_in_range) {
    point = ApplyPressure(point, app->pen_pressure);
  }

  const auto distance = glm::distance(previous_point.position, point.position);
  const int stroke_num = std::floor(distance / brush_options.spacing);
  if (stroke_num == 0) {
    return;
  }

  const auto start_point = previous_point;
  const auto end_point = point;

  static int round = 0;
  round = (round + 1) % 4;
  glm::vec4 debug_color;
  ImGui::ColorConvertHSVtoRGB((float)round / 4.0f, 0.8f, 1.0f, debug_color.r,
                              debug_color.g, debug_color.b);

  const auto t_step = brush_options.spacing / distance;
  SDL_Log("spacing: %f, d: %f, step: %f", t_step * distance, distance, t_step);
  float t = t_step;
  int i = 1;
  while (t < 1.0f) {
    ZoneScoped;

    StrokePoint point;

    // Maybe use SIMD for this
    point.position = glm::mix(start_point.position, end_point.position, t);
    point.color = glm::mix(start_point.color, end_point.color, t);
    // if (brush_options.debug_color) {
    //   point.color.r = debug_color.r;
    //   point.color.g = debug_color.g;
    //   point.color.b = debug_color.b;
    // }
    point.flow = std::lerp(start_point.flow, end_point.flow, t);
    point.radius = std::lerp(start_point.radius, end_point.radius, t);
    // if (brush_options.debug_radius) {
    //   point.radius = std::lerp(0.0f, end_point.radius, t);
    // }
    point.hardness = std::lerp(start_point.hardness, end_point.hardness, t);

    stroke_points.push_back(point);
    previous_point = point;

    SDL_Log("%d/%d/%llu: t: %f", i, stroke_num, stroke_points.size(), t);

    t += t_step;
    i++;

    {
      ZoneScoped;
      const auto affected_tile_pos =
          GetTilePosAffectedByStrokePoint(previous_point);
      for (const auto &tile_pos : affected_tile_pos) {
        Tile tile = GetTileAt(stroke_layer, tile_pos);
        if (tile == 0) {
          tile = CreateTile(stroke_layer, tile_pos);
        }
        if (!layer_tile_pos.at(selected_layer).contains(tile_pos)) {
          tile = CreateTile(selected_layer, tile_pos);
        }
        stroke_tile_affected.insert(tile);
      }
    }
  }
}

// This assumes every painted tiles are not going to be culled
void Canvas::EndStroke(StrokePoint point) {
  ZoneScoped;
  SDL_assert(stroke_started);

  if (app->pen_in_range) {
    point = ApplyPressure(point, app->pen_pressure);
  }

  // Merge stroke layer with selected layer
  MergeLayer(stroke_layer, selected_layer);
  DeleteLayer(stroke_layer);
  stroke_layer = 0;

  stroke_started = false;
}
} // namespace Midori
