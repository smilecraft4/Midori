#ifndef MIDORI_CANVAS_H
#define MIDORI_CANVAS_H

#include <cstdint>
#include <filesystem>
#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>

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
  bool Open(const std::filesystem::path &filename);
  bool SaveAs(const std::filesystem::path &filename);
  bool Save();

  // TODO: Move to it's own class
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

  [[nodiscard]] bool HasLayer(Layer layer) const;
  [[nodiscard]] bool LayerHasTile(Layer layer, Tile tile) const;
  [[nodiscard]] std::vector<Layer> Layers() const;
  [[nodiscard]] std::vector<Tile> LayerTiles(Layer layer) const;

  Layer CreateLayer(const std::string &name, std::uint8_t depth);
  void DeleteLayer(Layer layer);
  bool MergeLayers(Layer top, Layer bottom);
  bool SetLayerDepth(Layer layer, std::uint8_t depth);
  [[nodiscard]] std::uint8_t GetLayerDepth(Layer layer) const;

  bool MoveTileToLayer(Tile tile, Layer new_layer);
  bool MergeTile(Tile top, Tile btm);

  [[nodiscard]] Layer TileLayer(Tile tile) const;

  // TODO: Change for a better name, this name implies that a new tile is
  // created and maybe saved, but This should not be saved
  Tile CreateTile(Layer layer, glm::ivec2 position);
  void DeleteTile(Layer layer, glm::ivec2 position);
  void DeleteTile(Layer layer, Tile tile);

  // Member variables

  App *app;

  std::uint8_t current_max_layer_height = 0;
  std::unordered_map<Layer, LayerInfo> layer_infos;
  std::unordered_map<Layer, std::unordered_set<Tile>> layer_tiles;
  Layer selected_layer = 0;
  Layer last_assigned_layer = 0;
  std::vector<Layer> unassigned_layers;

  std::unordered_set<Layer> modified_layer;
  std::unordered_set<Tile> modified_tiles;

  std::unordered_map<Tile, TileInfo> tile_infos;
  Tile last_assigned_tile = 0;
  std::vector<Tile> unassigned_tiles;

  View view;
  glm::mat4 view_mat = glm::mat4(1.0f);
  std::vector<View> view_history;
  bool view_zooming = false;
  bool view_panning = false;
  bool view_rotating = false;

  // ?? Stuff
  void UpdateTileLoading();

  enum class TileLoadState : std::uint8_t {
    Queued,
    Read,
    Decompressed,
    Uploaded,
  };

  struct TileLoadStatus {
    Layer layer;
    Tile tile;
    TileLoadState state;
    std::vector<std::uint8_t> read_texture;
    std::vector<std::uint8_t> raw_texture;
  };
  std::unordered_map<Tile, TileLoadStatus> tile_load_queue;

  struct FileHeader {
    char name[4] = {'m', 'i', 'd', 'o'};
    std::uint8_t version[4] = {0, 0, 0, 0};
  };

  struct FileTile {
    glm::ivec2 position = {0, 0};
  };

  struct FileLayer {
    LayerInfo layer;
    std::unordered_set<glm::ivec2> tile_saved;
  };

  struct File {
    bool saved = false;
    std::filesystem::path filename;

    FileHeader header;
    std::unordered_map<Layer, FileLayer> layers;
  };
  File file;

  bool LayerHasTileFile(Layer layer, glm::ivec2 tile_pos);
};
} // namespace Midori

#endif
