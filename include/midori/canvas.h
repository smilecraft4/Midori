#ifndef MIDORI_CANVAS_H
#define MIDORI_CANVAS_H

#include <cstdint>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "midori/types.h"

namespace Midori {

class App;

class Canvas {
public:
  struct View {
    glm::vec2 pan = glm::vec2(0.0f, 0.0f);
    float zoom_amount = 1.0f;
    glm::vec2 zoom_origin = glm::vec2(0.0f, 0.0f);
    float rotation = 0.0f;
    glm::vec2 rotate_start = glm::vec2(1.0f, 0.0f);
  };

  Canvas(const Canvas &) = delete;
  Canvas(Canvas &&) = delete;
  Canvas &operator=(const Canvas &) = delete;
  Canvas &operator=(Canvas &&) = delete;

  Canvas(App *app);
  ~Canvas() = default;

  void Update();
  void Quit();

  // File Stuff
  bool New();
  bool Open();
  bool SaveAs();
  bool Save();

  // Viewport Stuff

  void ViewUpdateState(bool pan, bool zoom, bool rotate);
  void ViewUpdateCursor(glm::vec2 cursor_pos, glm::vec2 cursor_delta);
  void ViewPanStart();
  void ViewPan(glm::vec2 amount);
  void ViewPanStop();
  void ViewRotateStart(glm::vec2 start);
  void ViewRotate(float amount);
  void ViewRotateStop();
  void ViewZoomStart(glm::vec2 origin);
  void ViewZoom(float amount);
  void ViewZoomStop();
  [[nodiscard]] std::vector<glm::ivec2>
  GetVisibleTilePositions(const View &view, glm::vec2 size) const;

  // Layer Stuff

  [[nodiscard]] bool HasLayer(Layer layer) const;
  [[nodiscard]] bool LayerHasTile(Layer layer, Tile tile) const;
  [[nodiscard]] std::vector<Layer> Layers() const;
  [[nodiscard]] std::vector<Tile> LayerTiles(Layer layer) const;

  Layer CreateLayer(const std::string &name, std::uint8_t depth);
  void DeleteLayer(Layer layer);

  // Tile Stuff

  [[nodiscard]] Layer TileLayer(Tile tile) const;

  Tile CreateTile(Layer layer, glm::ivec2 position);
  void DeleteTile(Layer layer, Tile tile);

  // Member variables

  App *app;

  std::unordered_set<Tile> tile_queued;

  std::unordered_map<Layer, LayerInfo> layer_infos;
  std::unordered_map<Layer, std::unordered_set<Tile>> layer_tiles;
  Layer last_assigned_layer = 0;
  std::vector<Layer> unassigned_layers;

  std::unordered_map<Tile, TileInfo> tile_infos;
  Tile last_assigned_tile = 0;
  std::vector<Tile> unassigned_tiles;

  View view;
  glm::mat4 view_mat = glm::mat4(1.0f);
  std::vector<View> view_history;
  bool view_zooming = false;
  bool view_panning = false;
  bool view_rotating = false;
};
} // namespace Midori

#endif
