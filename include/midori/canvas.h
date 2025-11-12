#ifndef MIDORI_CANVAS_H
#define MIDORI_CANVAS_H

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "glm/ext/vector_int2.hpp"
#include "midori/types.h"

namespace Midori {

class App;

class Canvas {
public:
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

  // Layer Stuff

  [[nodiscard]] bool HasLayer(Layer layer) const;
  [[nodiscard]] bool LayerHasTile(Layer layer, Tile tile) const;
  [[nodiscard]] std::vector<Layer> Layers() const;
  [[nodiscard]] std::vector<Tile> LayerTiles(Layer layer) const;

  [[nodiscard]] Layer GetLayer(std::uint8_t depth) const;
  [[nodiscard]] Layer GetLayer(const std::string &name) const;

  Layer CreateLayer(const std::string &name, std::uint8_t depth);
  void DeleteLayer(Layer layer);

  // Tile Stuff

  [[nodiscard]] Layer TileLayer(Tile tile) const;
  [[nodiscard]] Tile GetTile(Layer layer, glm::ivec2 position) const;

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
};
} // namespace Midori

#endif
