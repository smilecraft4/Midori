#ifndef MIDORI_CANVAS_H
#define MIDORI_CANVAS_H

#include <cstdint>
#include <filesystem>
#include <vector>

#include "midori/types.h"

class App;

class Canvas {
 public:
  Canvas(const Canvas&) = delete;
  Canvas(Canvas&&) = delete;
  Canvas& operator=(const Canvas&) = delete;
  Canvas& operator=(Canvas&&) = delete;

  explicit Canvas(App* app);
  ~Canvas() = default;

  [[nodiscard]] static Canvas New();
  [[nodiscard]] static Canvas Open(const std::filesystem::path& filename);
  bool SaveAs(const std::filesystem::path& filename);
  bool Save();

  struct TileCreateInfo {
    int x;
    int y;
    Layer layer;
  };

  Tile CreateTile(TileCreateInfo tile_create_info);
  bool DeleteTile(Tile tile);
  Layer GetTileLayer(Tile tile);

  struct LayerCreateInfo {
    using Depth = std::uint8_t;
    static constexpr auto MAX_DEPTH = UINT8_MAX;

    std::string name;
    Depth depth;
  };

  Layer CreateLayer(LayerCreateInfo layer_create_info);
  bool DeleteLayer(Layer layer);
  bool AddTileToLayer(Layer layer, Tile tile);
  bool RemoveTileFromLayer(Layer layer, Tile tile);
  std::vector<Tile> GetLayerTiles(Layer layer);
  bool LayerContainsTile(Layer layer, Tile tile);

  App* app_;

  bool saved_;
  std::filesystem::path filename_;
};

#endif