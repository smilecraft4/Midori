#include "midori/canvas.h"

#include <SDL3/SDL_assert.h>
#include <imgui.h>

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
#include "nlohmann/json.hpp"
#include "qoi/qoi.h"
#define UUID_SYSTEM_GENERATOR
#include "uuid.h"

namespace Midori {

Canvas::Canvas(App *app) : app(app) {}

SDL_EnumerationResult findLayersCallback(void *userdata, const char *dirname, const char *fname) {
    if (dirname) {
        auto *canvas = (Canvas *)userdata;
        const std::string folder = std::format("{}{}", dirname, fname);
        const std::string path = std::format("{}/layer.json", folder);

        size_t size;
        char *buf = (char *)SDL_LoadFile(path.c_str(), &size);
        SDL_assert(buf && size);

        auto layerJson = nlohmann::json::parse(buf);
        const std::string name = layerJson.at("name");
        const uint8_t depth = layerJson.at("depth");

        const auto layer = canvas->CreateLayer(name, depth);
        canvas->selected_layer = layer;
        std::string uuid = layerJson.at("uuid");
        canvas->layer_infos[layer].id = uuids::uuid::from_string(uuid).value();
        canvas->layer_infos[layer].opacity = layerJson.at("opacity");
        canvas->layer_infos[layer].locked = layerJson.at("locked");
        canvas->layer_infos[layer].visible = layerJson.at("visible");
        canvas->layerTilesSaved[layer] = std::unordered_set<glm::ivec2>();

        int tilesCount;
        const auto *tilesFile = SDL_GlobDirectory(folder.c_str(), "*.qoi", 0, &tilesCount);
        for (int i = 0; i < tilesCount; i++) {
            size_t len = strlen(tilesFile[i]);
            int stage = 0;
            glm::ivec2 pos{};
            std::string buf;
            for (size_t c = 0; c < len; c++) {
                if (tilesFile[i][c] == '.') {
                    SDL_assert(stage == 1);
                    if (stage == 1) {
                        pos.y = std::atoi(buf.c_str());
                        stage++;
                    }
                    break;
                } else if (tilesFile[i][c] == '_') {
                    pos.x = std::atoi(buf.c_str());
                    buf.clear();
                    stage++;
                } else {
                    buf += tilesFile[i][c];
                }
            }

            SDL_assert(stage == 2);
            canvas->layerTilesSaved[layer].emplace(pos);
        }
    }

    return SDL_ENUM_CONTINUE;
}

bool Canvas::CanQuit() { return tile_to_unload.empty() && layer_to_delete.empty() && tile_to_delete.empty(); }

bool Canvas::Open() {
    ZoneScoped;
    const std::string path = SDL_GetPrefPath(NULL, "midori");
    filename = std::format("{}file", path);
    const bool exists = SDL_CreateDirectory(filename.c_str());
    SDL_Log("%s", filename.c_str());

    if (exists) {
        SDL_EnumerateDirectory(filename.c_str(), findLayersCallback, this);
    }

    if (layer_infos.empty()) {
        const auto layer = CreateLayer("Base Layer", 0);
        selected_layer = layer;
        layerTilesSaved[layer] = std::unordered_set<glm::ivec2>();
        SaveLayer(layer);
    }

    return true;
}

bool Canvas::Save() {
    // Maybe force save
    return true;
}

void Canvas::Update() {
    ZoneScoped;
    {  // Loading/Unloading culled tiles
        ZoneScopedN("Tile culling");
        std::vector<glm::ivec2> view_visible_tiles = GetViewVisibleTiles(view, app->window_size);

        for (const auto &layer : Layers()) {
            if (!layer_infos.at(layer).visible) {
                // Should be already culled maybe who knwon
                continue;
            }
            std::unordered_set<glm::ivec2> tile_positions(view_visible_tiles.begin(), view_visible_tiles.end());

            // Iterate over every tile in layer
            const std::vector<Tile> layer_tiles = LayerTiles(layer);
            std::unordered_set<Tile> remove_tiles(layer_tiles.begin(), layer_tiles.end());
            for (const auto &tile : layer_tiles) {
                if (tile_positions.contains(tile_infos.at(tile).position)) {
                    tile_positions.erase(tile_infos.at(tile).position);
                    remove_tiles.erase(tile);
                }
            }

            for (const auto &tile : remove_tiles) {
                UnloadTile(layer, tile);
            }

            for (const auto &tile_pos : tile_positions) {
                if (layerTilesSaved.at(layer).contains(tile_pos)) {
                    const auto result = LoadTile(layer, tile_pos);
                }
            }
        }
    }

    UpdateTileLoading();
}

void Canvas::DeleteUpdate() {
    UpdateTileUnloading();

    ZoneScoped;
    {  // Deleting tiles
        ZoneScopedN("Deleting tiles");

        std::vector<Tile> clear_tiles;
        for (const auto &tile : tile_to_delete) {
            SDL_assert(!tile_read_queue.contains(tile));

            if (tile_write_queue.contains(tile)) {
                continue;
            }

            SDL_assert(tile_infos.contains(tile));
            const auto tile_info = tile_infos.at(tile);
            SDL_assert(layer_infos.contains(tile_info.layer));

            const auto uuidString = uuids::to_string(layer_infos.at(tile_info.layer).id);
            const std::string tilePath =
                std::format("{}/{}/{}_{}.qoi", filename, uuidString, tile_info.position.x, tile_info.position.y);
            SDL_RemovePath(tilePath.c_str());

            layer_tiles.at(tile_info.layer).erase(tile);
            layer_tile_pos.at(tile_info.layer).erase(tile_infos.at(tile).position);
            layerTilesSaved[tile_info.layer].erase(tile_infos.at(tile).position);
            app->renderer.ReleaseTileTexture(tile);
            tile_infos.erase(tile);
            clear_tiles.push_back(tile);
        }
        for (const auto tile : clear_tiles) {
            tile_to_delete.erase(tile);
        }
    }

    {  // Deleting layers
        ZoneScopedN("Delete layer");

        std::vector<Layer> layer_cleared;
        for (const auto layer : layer_to_delete) {
            if (!layer_tiles.at(layer).empty()) {
                continue;
            }

            SDL_assert(layer_tiles.at(layer).empty());
            SDL_assert(selected_layer != layer);

            app->renderer.DeleteLayerTexture(layer);
            if (!layer_infos.at(layer).temporary) {
                const auto uuidString = uuids::to_string(layer_infos.at(layer).id);
                const std::string folderPath = std::format("{}/{}", filename, uuidString);
                const std::string infoPath = std::format("{}/layer.json", folderPath);
                SDL_RemovePath(infoPath.c_str());
                SDL_RemovePath(folderPath.c_str());
            }

            unassigned_layers.push_back(layer);
            layer_infos.erase(layer);
            layer_tiles.erase(layer);
            layerTilesModified.erase(layer);
            layerTilesSaved.erase(layer);
            layer_tile_pos.erase(layer);
            file.layers.erase(layer);

            layer_cleared.push_back(layer);
        }

        for (const auto layer : layer_cleared) {
            layer_to_delete.erase(layer);
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
        if (layer_to_delete.contains(other_layer)) {
            continue;
        }

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
        .id = uuids::uuid_system_generator{}(),
        .name = name,
        .opacity = 1.0f,
        .layer = layer,
        .depth = depth,
        .blend_mode = BlendMode::Normal,
        .visible = true,
        .temporary = false,
        .locked = false,
    };

    layer_infos[layer] = layer_info;
    layer_tiles[layer] = std::unordered_set<Tile>();
    layer_tile_pos[layer] = std::unordered_map<glm::ivec2, Tile>();
    layerTilesSaved[layer] = std::unordered_set<glm::ivec2>();
    layerTilesModified[layer] = std::unordered_set<Tile>();

    return layer;
}

bool Canvas::SaveLayer(Layer layer) {
    SDL_assert(layer_infos.contains(layer));
    const auto &layerInfo = layer_infos.at(layer);
    const auto uuidString = uuids::to_string(layerInfo.id);
    std::string path = std::format("{}/{}", filename, uuidString);

    SDL_CreateDirectory(path.c_str());

    nlohmann::json layerJson = {
        {"uuid", uuidString},         {"name", layerInfo.name},       {"opacity", layerInfo.opacity},
        {"locked", layerInfo.locked}, {"visible", layerInfo.visible}, {"depth", layerInfo.depth},
    };
    std::string layerDump = layerJson.dump();

    std::string file = std::format("{}/layer.json", path);
    SDL_IOStream *file_io = SDL_IOFromFile(file.c_str(), "w");
    SDL_assert(file_io != nullptr);
    SDL_WriteIO(file_io, layerDump.data(), layerDump.size());
    SDL_CloseIO(file_io);

    return true;
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

    current_max_layer_height--;
    for (auto &[layer_below, layer_below_info] : layer_infos) {
        if (layer_to_delete.contains(layer)) {
            continue;
        }

        if (layer_below == layer) {
            continue;
        }
        if (layer_below_info.depth > layer_infos.at(layer).depth) {
            layer_below_info.depth--;
            file.layers[layer].layer.depth--;
        }
        SDL_assert(layer_below_info.depth < current_max_layer_height);
    }
    if (selected_layer == layer) {
        selected_layer = 0;
    }

    SDL_Log("Canvas::DeleteLayer: %d", layer);
    std::vector<Tile> tiles = LayerTiles(layer);
    if (layer_infos.at(layer).temporary) {
        for (const auto tile : tiles) {
            UnloadTile(layer, tile);
        }
    } else {
        for (const auto tile : tiles) {
            DeleteTile(layer, tile);
        }
    }

    layer_to_delete.insert(layer);
}

void Canvas::MergeLayer(const Layer over_layer, const Layer below_layer) {
    SDL_assert(layer_infos.contains(over_layer));
    SDL_assert(!layer_to_delete.contains(over_layer));
    SDL_assert(layer_infos.contains(below_layer));
    SDL_assert(!layer_to_delete.contains(below_layer));

    // TODO: Do this for all the tiles stored in file
    std::vector<std::pair<Tile, Tile>> tile_to_merge;
    std::vector<Tile> tile_to_move;
    for (const auto &over_tile : layer_tiles.at(over_layer)) {
        const auto tile_merge_pos = tile_infos.at(over_tile).position;
        const auto below_tile = layer_tile_pos.at(below_layer).at(tile_merge_pos);
        tile_to_merge.emplace_back(over_tile, below_tile);
    }
    for (const auto &[over, below] : tile_to_merge) {
        MergeTiles(over, below);
    }
}

std::expected<Tile, TileError> Canvas::CreateTile(const Layer layer, const glm::ivec2 position) {
    ZoneScoped;
    SDL_assert(layer_infos.contains(layer) && "Layer missing");
    SDL_assert(!layer_tile_pos.at(layer).contains(position) && "Tile already loaded");

    Tile tile = TILE_INVALID;
    if (unassigned_tiles.empty()) {
        if (last_assigned_tile >= TILE_MAX) {
            return std::unexpected(TileError::OutOfHandle);
        }
        last_assigned_tile++;
        tile = last_assigned_tile;
    } else {
        tile = unassigned_tiles.back();
        if (tile == TILE_INVALID) {
            return std::unexpected(TileError::Invalid);
        }
        unassigned_tiles.pop_back();
    }

    layer_tiles.at(layer).insert(tile);
    layer_tile_pos.at(layer)[position] = tile;

    app->renderer.CreateTileTexture(tile);
    const auto &layer_info = layer_infos.at(layer);
    tile_infos[tile] = TileInfo{
        .layer = layer,
        .position = position,
    };

    return tile;
}

std::expected<Tile, TileError> Canvas::LoadTile(const Layer layer, const glm::ivec2 position) {
    ZoneScoped;
    SDL_assert(layer_infos.contains(layer) && "Layer missing");
    SDL_assert(!layer_tile_pos.at(layer).contains(position) && "Tile already loaded/loading");
    SDL_assert(layerTilesSaved.at(layer).contains(position) && "Tile not saved");

    Tile tile = TILE_INVALID;
    if (const auto result = CreateTile(layer, position); result.has_value()) {
        tile = result.value();
    } else {
        return result;
    }

    const auto layer_info = layer_infos.at(layer);
    tile_read_queue[tile] = TileReadStatus{
        .layer_id = layer_info.id,
        .tile = tile,
        .state = TileReadState::Queued,
    };

    return tile;
}

std::optional<TileError> Canvas::UnloadTile(const Layer layer, const Tile tile) {
    ZoneScoped;
    SDL_assert(layer_infos.contains(layer) && "Layer not found");
    SDL_assert(tile_infos.contains(tile) && "Tile not found");
    if (tile_write_queue.contains(tile)) {
        return TileError::Unknwown;
    }

    tile_to_unload.insert(tile);
    const LayerInfo layer_info = layer_infos.at(layer);
    tile_write_queue[tile] = TileWriteStatus{
        .layer_id = layer_info.id,
        .tile = tile,
        .state = TileWriteState::Queued,
        .position = tile_infos.at(tile).position,
    };

    return std::nullopt;
}

std::optional<TileError> Canvas::DeleteTile(const Layer layer, const Tile tile) {
    ZoneScoped;
    SDL_assert(layer_infos.contains(layer) && "Layer not found");
    SDL_assert(tile_infos.contains(tile) && "Tile not found");
    SDL_assert(!tile_to_delete.contains(tile) && "Tile already being deleted");

    tile_to_delete.insert(tile);

    return std::nullopt;
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

// TODO: Batch all merging action to only have a single command_buffer for this
// per frame
void Canvas::MergeTiles(Tile over_tile, Tile below_tile) {
    SDL_assert(tile_infos.contains(over_tile));
    SDL_assert(!tile_to_delete.contains(over_tile));
    SDL_assert(tile_infos.contains(below_tile));
    SDL_assert(!tile_to_delete.contains(below_tile));

    app->renderer.MergeTileTextures(over_tile, below_tile);
    layerTilesModified[tile_infos.at(below_tile).layer].insert(below_tile);
}

std::vector<glm::ivec2> Canvas::GetViewVisibleTiles(const View &view, const glm::vec2 size) const {
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
    for (auto &[tile, tile_load] : tile_read_queue) {
        if (i >= Midori::Renderer::TILE_MAX_UPLOAD_TRANSFER) {
            break;
        }

        if (tile_load.state == TileReadState::Queued) {
            SDL_assert(tile_infos.contains(tile));
            const auto tile_info = tile_infos.at(tile);
            const auto uuidString = uuids::to_string(tile_load.layer_id);
            const std::string tile_filename =
                std::format("{}/{}/{}_{}.qoi", filename, uuidString, tile_info.position.x, tile_info.position.y);

            ZoneScopedN("Reading Tile");
            size_t buf_size;
            auto *buf = (uint8_t *)SDL_LoadFile(tile_filename.c_str(), &buf_size);
            SDL_assert(buf_size > 0);
            SDL_assert(buf);

            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Found tile encoded texture");
            SDL_assert(buf_size > 0);
            SDL_assert(buf != nullptr);
            tile_load.encoded_texture.resize(buf_size);
            memcpy(tile_load.encoded_texture.data(), buf, buf_size);
            SDL_free(buf);

            tile_load.state = TileReadState::Read;
        }
        if (tile_load.state == TileReadState::Read) {
            ZoneScopedN("Decoding Tile");
            qoi_desc desc;
            auto *buf = (uint8_t *)qoi_decode(tile_load.encoded_texture.data(),
                                              (size_t)tile_load.encoded_texture.size(), &desc, 4);
            SDL_assert(buf != nullptr);
            SDL_assert(desc.channels = 4);
            SDL_assert(desc.width = TILE_SIZE);
            SDL_assert(desc.height = TILE_SIZE);
            tile_load.encoded_texture.clear();

            tile_load.raw_texture.resize(TILE_SIZE * TILE_SIZE * 4);
            memcpy(tile_load.raw_texture.data(), buf, TILE_SIZE * TILE_SIZE * 4);
            free(buf);

            tile_load.state = TileReadState::Decompressed;
        }
        if (tile_load.state == TileReadState::Decompressed) {
            ZoneScopedN("Uploading Tile");
            app->renderer.UploadTileTexture(tile, tile_load.raw_texture);
            tile_load.state = TileReadState::Uploaded;
            tiles_unqueued.push_back(tile);
        }
        i++;
    }
    for (const auto &tile : tiles_unqueued) {
        tile_read_queue.erase(tile);
    }
}

void Canvas::UpdateTileUnloading() {
    ZoneScoped;

    std::vector<Tile> tiles_unqueued;
    size_t i = 0;
    for (auto &[tile, tile_write] : tile_write_queue) {
        const auto tile_info = tile_infos.at(tile);

        if (!layerTilesModified.at(tile_info.layer).contains(tile)) {
            tiles_unqueued.push_back(tile);
            continue;
        }

        if (i >= Midori::Renderer::TILE_MAX_DOWNLOAD_TRANSFER) {
            break;
        }
        if (tile_write.state == TileWriteState::Queued) {
            if (app->renderer.DownloadTileTexture(tile)) {
                tile_write.state = TileWriteState::Downloading;
            }
        }
        if (tile_write.state == TileWriteState::Downloading) {
            if (app->renderer.IsTileTextureDownloaded(tile)) {
                if (app->renderer.CopyTileTextureDownloaded(tile, tile_write.raw_texture)) {
                    tile_write.state = TileWriteState::Downloaded;
                }
            }
        }
        if (tile_write.state == TileWriteState::Downloaded) {
            ZoneScopedN("Encoding tile");
            const qoi_desc desc = {
                .width = TILE_SIZE,
                .height = TILE_SIZE,
                .channels = 4,
                .colorspace = QOI_LINEAR,
            };
            int out_len = 0;
            auto *buf = (uint8_t *)qoi_encode(tile_write.raw_texture.data(), &desc, &out_len);
            SDL_assert(out_len > 0);
            SDL_assert(buf != nullptr);

            tile_write.encoded_texture.resize(out_len);
            memcpy(tile_write.encoded_texture.data(), buf, out_len);
            free(buf);

            tile_write.state = TileWriteState::Encoded;
        }
        if (tile_write.state == TileWriteState::Encoded) {
            ZoneScopedN("Write Tile file");
            const auto uuidString = uuids::to_string(tile_write.layer_id);
            const std::string tile_filename =
                std::format("{}/{}/{}_{}.qoi", filename, uuidString, tile_write.position.x, tile_write.position.y);

            SDL_IOStream *file_io = SDL_IOFromFile(tile_filename.c_str(), "wb");
            SDL_assert(file_io != nullptr);
            SDL_WriteIO(file_io, tile_write.encoded_texture.data(), tile_write.encoded_texture.size());
            SDL_CloseIO(file_io);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Wrote tile encoded texture");

            tile_write.state = TileWriteState::Written;
        }
        if (tile_write.state == TileWriteState::Written) {
            ZoneScopedN("Write Tile file");
            SDL_assert(tile_infos.contains(tile));
            const auto tile_info = tile_infos.at(tile);

            SDL_assert(layer_infos.contains(tile_info.layer));
            layerTilesSaved[tile_info.layer].insert(tile_info.position);

            tiles_unqueued.push_back(tile);
        }
    }

    {
        ZoneScopedN("Unqueing unload queue");
        for (const auto &tile : tiles_unqueued) {
            const auto &tile_info = tile_infos.at(tile);
            app->renderer.ReleaseTileTexture(tile);
            layer_tiles.at(tile_info.layer).erase(tile);
            layer_tile_pos.at(tile_info.layer).erase(tile_infos.at(tile).position);
            tile_infos.erase(tile);
            tile_to_unload.erase(tile);

            tile_write_queue.erase(tile);
            unassigned_tiles.push_back(tile);
        }
    }
}

bool Canvas::LayerHasTileFile(const Layer layer, const glm::ivec2 tile_pos) {
    SDL_assert(layerTilesSaved.contains(layer));
    return layerTilesSaved[layer].contains(tile_pos);
}

static glm::ivec2 GetTilePos(const glm::vec2 canvas_pos) {
    constexpr glm::vec2 tile_size = glm::vec2(TILE_SIZE);
    return glm::ivec2(glm::floor(canvas_pos / tile_size));
}

std::vector<glm::ivec2> GetTilePosAffectedByStrokePoint(Canvas::StrokePoint point) {
    constexpr glm::vec2 tile_size = glm::vec2(TILE_SIZE);
    std::vector<glm::ivec2> tiles_pos;

    glm::ivec2 tile_pos_min = glm::floor((point.position - point.radius) / tile_size);
    glm::ivec2 tile_pos_max = glm::ceil((point.position + point.radius) / tile_size);

    const glm::ivec2 tile_distance = tile_pos_max - tile_pos_min;
    tiles_pos.reserve(tile_distance.x * tile_distance.y);
    glm::ivec2 tile_pos;
    for (tile_pos.y = tile_pos_min.y; tile_pos.y < tile_pos_max.y; tile_pos.y++) {
        for (tile_pos.x = tile_pos_min.x; tile_pos.x < tile_pos_max.x; tile_pos.x++) {
            tiles_pos.push_back(tile_pos);
        }
    }

    return tiles_pos;
}

Canvas::StrokePoint Canvas::ApplyPressure(StrokePoint point, float pressure) {
    StrokePoint pressure_point = point;

    // TODO: Add min max value

    if (brush_options.opacity_pressure) {
        pressure_point.color *= pressure;  // We use premultiplied alpha
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
    stroke_layer = CreateLayer("Stroke Layer", layer_infos.at(selected_layer).depth);
    SDL_assert(stroke_layer != 0);
    layer_infos.at(stroke_layer).temporary = true;

    const auto tiles_pos = GetTilePosAffectedByStrokePoint(point);
    for (const auto &tile_pos : tiles_pos) {
        Tile tile = GetTileAt(stroke_layer, tile_pos);
        if (tile == 0) {
            if (const auto result = CreateTile(stroke_layer, tile_pos); result.has_value()) {
                tile = result.value();
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create stroke tile");
            }
        }
        if (!layer_tile_pos.at(selected_layer).contains(tile_pos)) {
            if (const auto result = CreateTile(selected_layer, tile_pos); result.has_value()) {
                tile = result.value();
            } else {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create under stroke tile");
            }
        }
        stroke_tile_affected.insert(tile);
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
    ImGui::ColorConvertHSVtoRGB((float)round / 4.0f, 0.8f, 1.0f, debug_color.r, debug_color.g, debug_color.b);

    const auto t_step = brush_options.spacing / distance;
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

        t += t_step;
        i++;

        {
            ZoneScoped;
            const auto tiles_pos = GetTilePosAffectedByStrokePoint(point);
            for (const auto &tile_pos : tiles_pos) {
                Tile tile = GetTileAt(stroke_layer, tile_pos);
                if (tile == 0) {
                    if (const auto result = CreateTile(stroke_layer, tile_pos); result.has_value()) {
                        tile = result.value();
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create stroke tile");
                    }
                }
                if (!layer_tile_pos.at(selected_layer).contains(tile_pos)) {
                    if (const auto result = CreateTile(selected_layer, tile_pos); result.has_value()) {
                        tile = result.value();
                    } else {
                        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create under stroke tile");
                    }
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

    // save the modified tile before merge to the history

    // Merge stroke layer with selected layer
    MergeLayer(stroke_layer, selected_layer);
    DeleteLayer(stroke_layer);
    stroke_layer = 0;

    // Save all the modified tiles once merged
    // for (const auto &tile : allTileStrokeAffected) {
    //    layerTilesModified[selected_layer].insert(tile);
    //}

    stroke_started = false;
}
}  // namespace Midori
