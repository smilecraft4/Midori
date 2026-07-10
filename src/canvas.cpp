#include "canvas.h"

#include "app.h"
#include "base.hpp"
#include "renderer.h"
#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "../deps/qoi.h"
#include "../deps/json.hpp"
#include <SDL3/SDL_assert.h>
#include <imgui.h>
#include <map>
#include <string>
#include <tracy/Tracy.hpp>
#include <utility>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/hash.hpp>
#include <algorithm>
#include <cstdint>
#include <format>

namespace Midori {

// Canvas

Canvas::Canvas(App* app) : app(app), canvasCommands(256), viewCommands(1024) {}

SDL_EnumerationResult FindLayersCallback(void* userdata, const char* dirname, const char* fname) {
    if (dirname != nullptr) {
        auto* canvas = (Canvas*)userdata;
        const std::string folder = std::format("{}{}", dirname, fname);
        const std::string path = std::format("{}/layer.json", folder);

        size_t size;
        char* buf = (char*)SDL_LoadFile(path.c_str(), &size);
        SDL_assert(buf && size);

        const auto layerJson = nlohmann::json::parse(buf);

        LayerInfo layerInfo{};
        layerInfo.id = layerJson.at("id");
        layerInfo.opacity = layerJson.at("opacity");
        layerInfo.locked = layerJson.at("locked");
        layerInfo.hidden = layerJson.at("hidden");
        layerInfo.name = layerJson.at("name");
        layerInfo.height = layerJson.at("height");
        const auto layer = canvas->CreateLayer(layerInfo);
        SDL_assert(layer != LAYER_INVALID);

        canvas->selectedLayer = layer; // TODO: Move this elsewhere
        canvas->layerTilesSaved[layer] = std::unordered_set<glm::ivec2>();

        int tilesCount;
        const auto* tilesFile = SDL_GlobDirectory(folder.c_str(), "*.qoi", 0, &tilesCount);
        for (int i = 0; i < tilesCount; i++) {
            size_t len = strlen(tilesFile[i]);
            int stage = 0;
            glm::ivec2 pos{};
            std::string tmp;
            for (size_t c = 0; c < len; c++) {
                if (tilesFile[i][c] == '.') {
                    SDL_assert(stage == 1);
                    if (stage == 1) {
                        pos.y = std::atoi(tmp.c_str());
                        stage++;
                    }
                    break;
                } else if (tilesFile[i][c] == '_') {
                    pos.x = std::atoi(tmp.c_str());
                    tmp.clear();
                    stage++;
                } else {
                    tmp += tilesFile[i][c];
                }
            }

            SDL_assert(stage == 2);
            canvas->layerTilesSaved[layer].emplace(pos);
        }
    }

    return SDL_ENUM_CONTINUE;
}

bool Canvas::CanQuit() {
    return tileToUnload.empty() && layerToDelete.empty() && tileToDelete.empty();
}

bool Canvas::Open() {
    ZoneScoped;
#ifdef NDEBUG
    const std::string path = SDL_GetPrefPath(nullptr, "midori");
#else
    const std::string path = SDL_GetPrefPath(nullptr, "midori-dev");
#endif
    filename = std::format("{}file", path);
    const bool exists = SDL_CreateDirectory(filename.c_str());
    SDL_Log("%s", filename.c_str());

    // nlohmann::json waypointsJson;
    // std::string waypointsDump = waypointsJson.dump(4);
    // std::string file = std::format("{}/waypoints.json", filename);
    // SDL_IOStream *file_io = SDL_IOFromFile(file.c_str(), "w");
    // SDL_assert(file_io != nullptr);
    // SDL_WriteIO(file_io, waypointsDump.data(), waypointsDump.size());
    // SDL_CloseIO(file_io);

    if (exists) {
        SDL_EnumerateDirectory(filename.c_str(), FindLayersCallback, this);
        CompactLayerHeight();
    }

    if (layerInfos.empty()) {
        LayerInfo layerInfo{};
        layerInfo.name = "Base Layer";
        const auto layer = CreateLayer(layerInfo);
        SDL_assert(layer != LAYER_INVALID);

        selectedLayer = layer;
        layerTilesSaved[layer] = std::unordered_set<glm::ivec2>();
        SaveLayer(layer);
    }

    OpenBrush();
    OpenEraser();

    return true;
}

void Canvas::CompactLayerHeight() {
    std::map<uint8_t, Layer> sortedHeightLayers;
    for (auto& [layer, layer_info] : layerInfos) {
        sortedHeightLayers[layer_info.height] = layer;
    }

    uint8_t previousHeight = 0;
    for (auto& [height, layer] : sortedHeightLayers) {
        SetLayerHeight(layer, previousHeight);
        previousHeight++;
    }
}

void Canvas::Update() {
    ZoneScoped;

    CullTiles(viewport);
    UpdateTileLoading();
}

void Canvas::CullTiles(Viewport& viewport) {
    ZoneScoped;

    const auto& tilesVisible = viewport.VisibleTiles(app->frameArena);
    for (const auto& [layer, info] : layerInfos) {
        if (info.hidden) {
            // A layer with 0 opacity can still be painted on,
            // so we keep it's tile loaded in case
            for (const auto& tile : layerTiles[layer]) {
                QueueUnloadTile(layer, tile);
            }
        } else {
            for (const auto& tile : layerTiles[layer]) {
                if (!viewport.IsTileVisible(tileInfos[tile].pos)) {
                    QueueUnloadTile(layer, tile);
                }
            }
            for (const auto& tilePos : tilesVisible) {
                if (layerTilesSaved[layer].contains(tilePos)) {
                    if (!layerTilePos[layer].contains(tilePos)) {
                        QueueLoadTile(layer, tilePos);
                    }
                }
            }
        }
    }
}

void Canvas::DeleteUpdate() {
    UpdateTileUnloading();

    ZoneScoped;
    { // Deleting tiles
        ZoneScopedN("Deleting tiles");

        std::vector<Tile, arena_allocator<Tile>> clearTiles(app->frameArena);
        clearTiles.reserve(tileToDelete.size());

        for (const auto& tile : tileToDelete) {
            SDL_assert(!tileReadQueue.contains(tile));

            if (tileWriteQueue.contains(tile)) {
                continue;
            }

            SDL_assert(tileInfos.contains(tile));
            const auto tileInfo = tileInfos.at(tile);
            SDL_assert(layerInfos.contains(tileInfo.layer));

            const std::string tilePath =
                std::format("{}/{}/{}_{}.qoi", filename, tileInfo.layer, tileInfo.pos.x, tileInfo.pos.y);
            SDL_RemovePath(tilePath.c_str());

            layerTiles.at(tileInfo.layer).erase(tile);
            layerTilePos.at(tileInfo.layer).erase(tileInfos.at(tile).pos);
            layerTilesSaved[tileInfo.layer].erase(tileInfos.at(tile).pos);
            app->renderer.ReleaseTileTexture(tile);
            tileInfos.erase(tile);
            tilesUnassigned.push_back(tile);
            clearTiles.push_back(tile);
        }
        for (const auto tile : clearTiles) {
            tileToDelete.erase(tile);
        }
    }

    { // Deleting layers
        ZoneScopedN("Delete layer");

        std::vector<Layer, arena_allocator<Layer>> layerCleared(app->frameArena);
        layerCleared.reserve(layerToDelete.size());

        for (const auto layer : layerToDelete) {
            if (!layerTiles.at(layer).empty()) {
                continue;
            }

            SDL_assert(layerTiles.at(layer).empty());
            SDL_assert(selectedLayer != layer);

            app->renderer.DeleteLayerTexture(layer);
            if (!layerInfos.at(layer).internal) {
                const std::string folderPath = std::format("{}/{}", filename, layer);
                const std::string infoPath = std::format("{}/layer.json", folderPath);
                SDL_RemovePath(infoPath.c_str());
                SDL_RemovePath(folderPath.c_str());
            }

            layersUnassigned.push_back(layer);
            layerInfos.erase(layer);
            layerTiles.erase(layer);
            layerTilesModified.erase(layer);
            layerTilesSaved.erase(layer);
            layerTilePos.erase(layer);

            layerCleared.push_back(layer);
        }

        for (const auto layer : layerCleared) {
            layerToDelete.erase(layer);
        }
    }
}

std::vector<Layer> Canvas::Layers() const {
    ZoneScoped;
    std::vector<Layer> layers;
    layers.reserve(layerInfos.size());
    for (const auto& [layer, info] : layerInfos) {
        layers.push_back(layer);
    }
    return layers;
}

bool Canvas::HasLayer(const Layer layer) const {
    ZoneScoped;
    return layerInfos.contains(layer);
}

bool Canvas::LayerHasTile(const Layer layer, const Tile tile) const {
    ZoneScoped;
    return layerTiles.at(layer).contains(tile);
}

std::vector<Tile> Canvas::LayerTiles(const Layer layer) const {
    ZoneScoped;
    SDL_assert(HasLayer(layer) && "Layer not found");

    std::vector<Tile> tiles;
    tiles.reserve(layerTiles.at(layer).size());
    for (const auto& tile : layerTiles.at(layer)) {
        SDL_assert(tile != 0 && "Invalid tile found");
        tiles.push_back(tile);
    }

    return tiles;
}

Layer Canvas::CreateLayer(LayerInfo layerInfo) {
    ZoneScoped;

    if (layerInfo.id != LAYER_INVALID) {
        layerLastAssigned = std::max(layerLastAssigned, layerInfo.id);
    } else {
        if (layersUnassigned.empty()) {
            if (layerLastAssigned >= LAYER_MAX) {
                SDL_assert(false && "Max layer reached");
                return 0;
            }
            layerLastAssigned++;
            layerInfo.id = layerLastAssigned;
        } else {
            layerInfo.id = layersUnassigned.back();
            layersUnassigned.pop_back();
        }
    }
    SDL_assert(layerInfo.id != LAYER_INVALID);
    SDL_assert(!HasLayer(layerInfo.id) && "Layer already exists");

    if (!app->renderer.CreateLayerTexture(layerInfo.id)) {
        layersUnassigned.push_back(layerInfo.id);
        return 0;
    }

    layersCurrentMaxHeight++;
    for (auto& [other_layer, other_layer_info] : layerInfos) {
        if (layerToDelete.contains(other_layer)) {
            continue;
        }

        if (other_layer == layerInfo.id) {
            continue;
        }
        if (other_layer_info.height >= layerInfo.height) {
            other_layer_info.height++;
            layersModified.insert(other_layer);
        }
    }

    layerInfos[layerInfo.id] = layerInfo;
    layerTiles[layerInfo.id] = std::unordered_set<Tile>();
    layerTilePos[layerInfo.id] = std::unordered_map<glm::ivec2, Tile>();
    layerTilesSaved[layerInfo.id] = std::unordered_set<glm::ivec2>();
    layerTilesModified[layerInfo.id] = std::unordered_set<Tile>();

    // TODO: do this properly
    layersHeightSorted.push_back(layerInfo.id);

    return layerInfo.id;
}

bool Canvas::SaveLayer(Layer layer) {
    SDL_assert(layerInfos.contains(layer));
    const auto& layerInfo = layerInfos.at(layer);
    std::string path = std::format("{}/{}", filename, layerInfo.id);

    SDL_CreateDirectory(path.c_str());

    nlohmann::json layerJson = {
        {"id", layerInfo.id},         {"name", layerInfo.name},     {"opacity", layerInfo.opacity},
        {"locked", layerInfo.locked}, {"hidden", layerInfo.hidden}, {"height", layerInfo.height},
    };
    std::string layerDump = layerJson.dump(4);

    std::string file = std::format("{}/layer.json", path);
    SDL_IOStream* fileIo = SDL_IOFromFile(file.c_str(), "w");
    SDL_assert(fileIo != nullptr);
    SDL_WriteIO(fileIo, layerDump.data(), layerDump.size());
    SDL_CloseIO(fileIo);
    layersModified.erase(layer);

    return true;
}

Layer Canvas::DuplicateLayer(Layer layer, bool temporary) {
    assert(layerInfos.contains(layer));

    Layer newLayer = LAYER_INVALID;
    { // We can't use the informations since they are create informations and not the real generated informations
        auto newLayerInfo = layerInfos[layer];
        newLayerInfo.id = LAYER_INVALID;
        newLayerInfo.height++;
        newLayerInfo.internal = temporary;
        newLayer = CreateLayer(newLayerInfo);
    }

    if (!temporary) {
        // Duplicate save folder for layers
        std::string layerPath = std::format("{}/{}", filename, layerInfos[layer].id);
        std::string newLayerPath = std::format("{}/{}", filename, layerInfos[newLayer].id);
        std::filesystem::copy(layerPath, newLayerPath);

        // Overwrite copied settings
        SaveLayer(newLayer);
    }

    return newLayer;
}

bool Canvas::SetLayerHeight(const Layer layer, LayerHeight height) {
    SDL_assert(layerInfos.contains(layer) && "Layer not found");

    // No new layer are created nor destroyed so the max should stay the same
    // We clamp the layer height to minimize looping
    height = std::min(layersCurrentMaxHeight, height);

    const LayerHeight oldHeight = layerInfos[layer].height;

    const bool moveUp = (oldHeight > height);
    const auto minHeight = std::min(oldHeight, height);
    const auto maxHeight = std::max(oldHeight, height);

    for (auto& [otherLayer, otherLayerInfo] : layerInfos) {
        SDL_assert(otherLayer != LAYER_INVALID);
        if (otherLayer == layer) {
            continue;
        }
        const auto otherHeight = otherLayerInfo.height;
        if (minHeight <= otherHeight && otherHeight <= maxHeight) {
            SDL_assert(moveUp && otherLayerInfo.height > 0);
            otherLayerInfo.height += (moveUp) ? 1 : -1;
            layersModified.insert(otherLayer);
        }
    }

    layerInfos[layer].height = height;
    layersModified.insert(layer);

    return true;
}

void Canvas::DeleteLayer(const Layer layer) {
    ZoneScoped;
    SDL_assert(HasLayer(layer) && "Layer not found");

    layersCurrentMaxHeight--;
    for (auto& [layer_below, layer_below_info] : layerInfos) {
        if (layerToDelete.contains(layer)) {
            continue;
        }

        if (layer_below == layer) {
            continue;
        }
        if (layer_below_info.height > layerInfos.at(layer).height) {
            layer_below_info.height--;
            layersModified.insert(layer_below);
        }
    }
    if (selectedLayer == layer) {
        selectedLayer = 0;
    }

    std::vector<Tile> tiles = LayerTiles(layer);
    if (layerInfos.at(layer).internal) {
        for (const auto tile : tiles) {
            QueueUnloadTile(layer, tile);
        }
    } else {
        for (const auto tile : tiles) {
            QueueTileDelete(layer, tile);
        }
    }

    if (layersModified.contains(layer)) {
        layersModified.erase(layer);
    }

    layerToDelete.insert(layer);
}

void Canvas::MergeLayer(const Layer overLayer, const Layer belowLayer) {
    SDL_assert(layerInfos.contains(overLayer));
    SDL_assert(!layerToDelete.contains(overLayer));
    SDL_assert(layerInfos.contains(belowLayer));
    SDL_assert(!layerToDelete.contains(belowLayer));

    // TODO: Do this for all the tiles stored in file
    std::vector<std::pair<Tile, Tile>> tileToMerge;
    for (const auto& overTile : layerTiles.at(overLayer)) {
        const auto tileMergePos = tileInfos.at(overTile).pos;
        const auto belowTile = layerTilePos.at(belowLayer).at(tileMergePos);
        tileToMerge.emplace_back(overTile, belowTile);
    }
    // TODO: Batch all merging action to only have a single command_buffer for this
    // per frame
    for (const auto& [over, below] : tileToMerge) {
        MergeTiles(over, below);
    }
}

Tile Canvas::CreateTile(const Layer layer, const glm::ivec2 position) {
    ZoneScoped;
    SDL_assert(layerInfos.contains(layer) && "Layer missing");
    SDL_assert(!layerTilePos.at(layer).contains(position) && "Tile already loaded");

    Tile tile = TILE_INVALID;
    if (tilesUnassigned.empty()) {
        SDL_assert(tileLastAssigned < TILES_MAX && "Tile limits reached");
        tileLastAssigned++;
        SDL_assert(!layerTiles.at(layer).contains(tileLastAssigned));
        tile = tileLastAssigned;
    } else {
        tile = tilesUnassigned.back();
        SDL_assert(!layerTiles.at(layer).contains(tile));
        SDL_assert(tile != TILE_INVALID && "Tile is invalid ?");
        tilesUnassigned.pop_back();
    }

    layerTiles.at(layer).insert(tile);
    layerTilePos.at(layer)[position] = tile;

    app->renderer.CreateTileTexture(tile);
    tileInfos[tile] = TileCoord{
        .layer = layer,
        .pos = position,
    };

    return tile;
}

Tile Canvas::QueueLoadTile(const Layer layer, const glm::ivec2 position) {
    ZoneScoped;
    SDL_assert(layerInfos.contains(layer) && "Layer missing");
    SDL_assert(!layerTilePos.at(layer).contains(position) && "Tile already loaded/loading");
    SDL_assert(layerTilesSaved.at(layer).contains(position) && "Tile not saved");

    const auto tile = CreateTile(layer, position);
    SDL_assert(tile != TILE_INVALID && "Tile invalid ?");

    tileReadQueue[tile] = TileReadStatus{
        .layer = layer,
        .tile = tile,
        .state = TileReadState::Queued,
    };

    return tile;
}

bool Canvas::QueueSaveTile(const Layer layer, const Tile tile) {
    ZoneScoped;
    SDL_assert(layerInfos.contains(layer) && "Layer not found");
    SDL_assert(layerTilesModified.contains(layer) && "Tile not found");
    SDL_assert(tileInfos.contains(tile) && "Tile not found");
    SDL_assert(!tileWriteQueue.contains(tile) && "Tile already being queued for saving");

    tileWriteQueue[tile] = TileWriteStatus{
        .layer = layer,
        .tile = tile,
        .state = TileWriteState::Queued,
        .position = tileInfos[tile].pos,
    };

    return true;
}

void Canvas::QueueUnloadTile(const Layer layer, const Tile tile) {
    ZoneScoped;
    SDL_assert(layerInfos.contains(layer) && "Layer not found");
    SDL_assert(tileInfos.contains(tile) && "Tile not found");

    if (!tileWriteQueue.contains(tile)) {
        QueueSaveTile(layer, tile);
    }

    tileToUnload.insert(tile);
}

void Canvas::QueueTileDelete(const Layer layer, const Tile tile) {
    ZoneScoped;
    SDL_assert(layerInfos.contains(layer) && "Layer not found");
    SDL_assert(tileInfos.contains(tile) && "Tile not found");
    SDL_assert(!tileToDelete.contains(tile) && "Tile already being deleted");

    tileToDelete.insert(tile);
}

Tile Canvas::GetLoadedTileAt(const Layer layer, const glm::ivec2 position) const {
    ZoneScoped;
    SDL_assert(layerTilePos.contains(layer));

    Tile tile = TILE_INVALID;
    if (layerTilePos.at(layer).contains(position)) {
        tile = layerTilePos.at(layer).at(position);
    }

    return tile;
}

void Canvas::MergeTiles(Tile overTile, Tile belowTile) {
    SDL_assert(tileInfos.contains(overTile));
    SDL_assert(!tileToDelete.contains(overTile));
    SDL_assert(tileInfos.contains(belowTile));
    SDL_assert(!tileToDelete.contains(belowTile));

    app->renderer.MergeTileTextures(overTile, belowTile);
    layerTilesModified[tileInfos.at(belowTile).layer].insert(belowTile);
}

void Canvas::ViewUpdateState(glm::vec2 cursorPos) {
    const auto pan = app->spacePressed;
    const auto zoom = app->ctrlPressed;
    const auto rotate = app->shiftPressed;

    if (viewPanning && (!pan || (pan && zoom) || (pan && rotate))) {
        viewPanning = false;
    }
    if (viewZooming && (!pan || !zoom || (pan && rotate))) {
        viewZooming = false;
    }
    if (viewRotating && (!pan || !rotate || (pan && zoom))) {
        viewRotating = false;
    }

    if (!viewPanning && (pan && !zoom && !rotate)) {
        // viewCursorStart = cursor_pos;
        viewPanning = true;
    }
    if (!viewZooming && (pan && zoom && !rotate)) {
        // viewCursorStart = cursor_pos;
        viewZooming = true;
    }
    if (!viewRotating && (pan && rotate && !zoom)) {
        // viewCursorStart = cursor_pos;
        viewRotating = true;
    }

    viewCursorPrevious = cursorPos;
}

void Canvas::ViewUpdateCursor(glm::vec2 cursorPos) {
    if (viewPanning) {
        const auto viewCursorDelta = cursorPos - viewCursorPrevious;
        // SDL_Log("%.2f, %.2f", viewCursorDelta.x, viewCursorDelta.y);
        viewport.Translate(viewCursorDelta);
    } else if (viewRotating) {
        const auto startAngle = std::atan2(viewCursorStart.x - (static_cast<float>(app->windowSize.x) / 2.0f),
                                           viewCursorStart.y - (static_cast<float>(app->windowSize.y) / 2.0f));
        const auto previousAngle = std::atan2(viewCursorPrevious.x - (static_cast<float>(app->windowSize.x) / 2.0f),
                                              viewCursorPrevious.y - (static_cast<float>(app->windowSize.y) / 2.0f));
        const auto currentAngle = std::atan2(cursorPos.x - (static_cast<float>(app->windowSize.x) / 2.0f),
                                             cursorPos.y - (static_cast<float>(app->windowSize.y) / 2.0f));
        const auto angleDelta = (previousAngle - startAngle) - (currentAngle - startAngle);
        viewport.Rotate(angleDelta);
    } else if (viewZooming) {
        const auto viewCursorDelta = cursorPos - viewCursorPrevious;
        viewport.Zoom(viewCursorStart, glm::vec2(viewCursorDelta.x / 1000.0f));
    }
}

// TODO: Make multithreaded
void Canvas::UpdateTileLoading() {
    ZoneScoped;

    std::vector<Tile, arena_allocator<Tile>> tilesUnqueued(app->frameArena);
    size_t i = 0;
    for (auto& [tile, tile_load] : tileReadQueue) {
        if (i >= Midori::Renderer::TILE_MAX_UPLOAD_TRANSFER) {
            break;
        }

        if (tile_load.state == TileReadState::Queued) {
            SDL_assert(tileInfos.contains(tile));
            const auto tileInfo = tileInfos.at(tile);
            const std::string tileFilename =
                std::format("{}/{}/{}_{}.qoi", filename, tile_load.layer, tileInfo.pos.x, tileInfo.pos.y);

            ZoneScopedN("Reading Tile");
            size_t bufSize;
            auto* buf = (uint8_t*)SDL_LoadFile(tileFilename.c_str(), &bufSize);
            SDL_assert(bufSize > 0);
            SDL_assert(buf);

            // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Found tile encoded texture");
            SDL_assert(bufSize > 0);
            SDL_assert(buf != nullptr);
            tile_load.encodedTexture.resize(bufSize);
            memcpy(tile_load.encodedTexture.data(), buf, bufSize);
            SDL_free(buf);

            tile_load.state = TileReadState::Read;
        }
        if (tile_load.state == TileReadState::Read) {
            ZoneScopedN("Decoding Tile");
            qoi_desc desc;
            auto* buf = (uint8_t*)qoi_decode(tile_load.encodedTexture.data(),
                                             static_cast<int>(tile_load.encodedTexture.size()), &desc, 4);
            SDL_assert(buf != nullptr);
            SDL_assert(desc.channels = 4);
            SDL_assert(desc.width = TILE_WIDTH);
            SDL_assert(desc.height = TILE_HEIGHT);
            tile_load.encodedTexture.clear();

            tile_load.rawTexture.resize(TILE_WIDTH * TILE_HEIGHT * 4);
            memcpy(tile_load.rawTexture.data(), buf, TILE_WIDTH * TILE_HEIGHT * 4);
            free(buf);

            tile_load.state = TileReadState::Decompressed;
        }
        if (tile_load.state == TileReadState::Decompressed) {
            ZoneScopedN("Uploading Tile");
            app->renderer.UploadTileTexture(tile, tile_load.rawTexture);
            tile_load.state = TileReadState::Uploaded;
            tilesUnqueued.push_back(tile);
        }
        i++;
    }
    for (const auto& tile : tilesUnqueued) {
        tileReadQueue.erase(tile);
    }
}

// TODO: Make multithreaded
void Canvas::UpdateTileUnloading() {
    ZoneScoped;

    std::vector<Tile, arena_allocator<Tile>> tileWritten(app->frameArena);
    size_t i = 0;
    for (auto& [tile, tile_write] : tileWriteQueue) {
        const auto tileInfo = tileInfos.at(tile);

        // If the tile is already saved marked it as finished
        if (!layerTilesModified.at(tileInfo.layer).contains(tile)) {
            tile_write.state = TileWriteState::Written;
            tileWritten.push_back(tile);
            continue;
        }

        // limit the amount of download per frame
        if (i >= Midori::Renderer::TILE_MAX_DOWNLOAD_TRANSFER) {
            break;
        }

        //  The queue to allow multithreading later on
        if (tile_write.state == TileWriteState::Queued) {
            if (app->renderer.DownloadTileTexture(tile)) {
                tile_write.state = TileWriteState::Downloading;
            }
        }
        if (tile_write.state == TileWriteState::Downloading) {
            if (app->renderer.IsTileTextureDownloaded(tile)) {
                if (app->renderer.CopyTileTextureDownloaded(tile, tile_write.rawTexture)) {
                    ZoneScopedN("Check if tile is empty");
                    bool empty = true;
                    for (const auto& v : tile_write.rawTexture) {
                        if (v > 1) {
                            empty = false;
                            break;
                        }
                    }
                    if (empty) {
                        QueueTileDelete(tileInfo.layer, tile);
                        tileWritten.push_back(tile);
                        tile_write.state = TileWriteState::Written;
                    } else {
                        tile_write.state = TileWriteState::Downloaded;
                    }
                }
            }
        }
        if (tile_write.state == TileWriteState::Downloaded) {
            ZoneScopedN("Encoding tile");
            const qoi_desc desc = {
                .width = TILE_WIDTH,
                .height = TILE_HEIGHT,
                .channels = 4,
                .colorspace = QOI_LINEAR,
            };
            int outLen = 0;
            auto* buf = (uint8_t*)qoi_encode(tile_write.rawTexture.data(), &desc, &outLen);
            SDL_assert(outLen > 0);
            SDL_assert(buf != nullptr);

            tile_write.encodedTexture.resize(outLen);
            memcpy(tile_write.encodedTexture.data(), buf, outLen);
            free(buf);

            tile_write.state = TileWriteState::Encoded;
        }
        if (tile_write.state == TileWriteState::Encoded) {
            ZoneScopedN("Write Tile file");
            const std::string tileFilename = std::format("{}/{}/{}_{}.qoi", filename, tile_write.layer,
                                                         tile_write.position.x, tile_write.position.y);

            SDL_IOStream* fileIo = SDL_IOFromFile(tileFilename.c_str(), "wb");
            SDL_assert(fileIo != nullptr);
            SDL_WriteIO(fileIo, tile_write.encodedTexture.data(), tile_write.encodedTexture.size());
            SDL_CloseIO(fileIo);
            // SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Wrote tile encoded texture");

            tile_write.state = TileWriteState::Written;
        }
        if (tile_write.state == TileWriteState::Written) {
            ZoneScopedN("Write Tile file");
            const auto& tileInfo = tileInfos[tile];
            SDL_assert(tileInfos.contains(tile));
            SDL_assert(layerInfos.contains(tileInfo.layer));
            layerTilesSaved[tileInfo.layer].insert(tileInfo.pos);
            layerTilesModified[tileInfo.layer].erase(tile);
            tileWritten.push_back(tile);
        }
    }

    {
        ZoneScopedN("Unqueing unload queue");
        for (const auto& tile : tileWritten) {
            tileWriteQueue.erase(tile);

            if (!tileToUnload.contains(tile)) {
                continue;
            }
            const auto& tileInfo = tileInfos.at(tile);
            if (!tileToDelete.contains(tile)) {
                app->renderer.ReleaseTileTexture(tile);
                layerTiles.at(tileInfo.layer).erase(tile);
                layerTilePos.at(tileInfo.layer).erase(tileInfos.at(tile).pos);
                tileInfos.erase(tile);
                tilesUnassigned.push_back(tile);
            }

            tileToUnload.erase(tile);
        }
    }
}

std::vector<glm::ivec2> GetTilePosAffectedByStrokePoint(Canvas::StrokePoint point) {
    constexpr glm::vec2 TILE_SIZE = glm::vec2(TILE_WIDTH, TILE_HEIGHT);
    std::vector<glm::ivec2> tilesPos;

    glm::ivec2 tilePosMin = glm::floor((point.position - (point.radius + 16.0f)) / TILE_SIZE);
    glm::ivec2 tilePosMax = glm::ceil((point.position + (point.radius + 16.0f)) / TILE_SIZE);

    const glm::ivec2 tileDistance = tilePosMax - tilePosMin;
    tilesPos.reserve((size_t)tileDistance.x * tileDistance.y);
    glm::ivec2 tilePos;
    for (tilePos.y = tilePosMin.y; tilePos.y < tilePosMax.y; tilePos.y++) {
        for (tilePos.x = tilePosMin.x; tilePos.x < tilePosMax.x; tilePos.x++) {
            tilesPos.push_back(tilePos);
        }
    }

    return tilesPos;
}

static float Remap(float v, float min, float max) {
    return (v * (max - min)) + min;
}

Canvas::StrokePoint Canvas::ApplyBrushPressure(StrokePoint point, const float pressure) const {
    if (brushOptions.opacityPressure) {
        // map pressure from [0, 1] to [min, max]
        point.color.a *= Remap(pressure, brushOptions.opacityPressureRange.x, brushOptions.opacityPressureRange.y);
    }
    if (brushOptions.radiusPressure) {
        point.radius *= Remap(pressure, brushOptions.radiusPressureRange.x, brushOptions.radiusPressureRange.y);
    }
    if (brushOptions.flowPressure) {
        point.flow *= Remap(pressure, brushOptions.flowPressureRange.x, brushOptions.flowPressureRange.y);
    }
    if (brushOptions.hardnessPressure) {
        point.hardness *= Remap(pressure, brushOptions.hardnessPressureRange.x, brushOptions.hardnessPressureRange.y);
    }

    return point;
}

// This assumes every painted tiles are not going to be culled
void Canvas::StartBrushStroke(StrokePoint point) {
    ZoneScoped;
    SDL_assert(selectedLayer != LAYER_INVALID);
    SDL_assert(!strokeStarted);
    SDL_assert(!currentTileModificationCommand);

    strokeStarted = true;
    currentTileModificationCommand = std::make_unique<TileModificationCommand>(this, selectedLayer);

    if (app->penInRange) {
        point = ApplyBrushPressure(point, app->penPressure);
    }

    SDL_assert(strokeLayer == 0);
    // Create a internal layer above the selected layer and set it as active
    LayerInfo layerInfo{};
    layerInfo.name = "Stroke Layer";
    layerInfo.height = layerInfos[selectedLayer].height;
    layerInfo.opacity = layerInfos[selectedLayer].opacity; // FIXME: this is broken, the blending is bad
    layerInfo.internal = true;
    strokeLayer = CreateLayer(layerInfo);
    SDL_assert(strokeLayer != LAYER_INVALID);

    const auto tilesPos = GetTilePosAffectedByStrokePoint(point);
    for (const auto& tilePos : tilesPos) {
        Tile tile = GetLoadedTileAt(strokeLayer, tilePos);
        if (tile == TILE_INVALID) {
            tile = CreateTile(strokeLayer, tilePos);
            SDL_assert(tile != TILE_INVALID && "Failed to create tile on stroke layer");
        }
        strokeTileAffected.insert(tile);

        if (!layerTilePos.at(selectedLayer).contains(tilePos)) {
            const auto srcLayerTile = CreateTile(selectedLayer, tilePos);
            SDL_assert(srcLayerTile != TILE_INVALID && "Failed to create on selected layer during stroke");
        }
        allTileStrokeAffected.insert(layerTilePos.at(selectedLayer).at(tilePos));
    }

    strokePoints.push_back(point);
    previousPoint = point;

    SDL_assert(strokeStarted);
    SDL_assert(currentTileModificationCommand);
}

// This assumes every painted tiles are not going to be culled
void Canvas::UpdateBrushStroke(StrokePoint point) {
    ZoneScoped;
    SDL_assert(strokeStarted);
    SDL_assert(strokeLayer != 0);
    SDL_assert(currentTileModificationCommand);

    if (app->penInRange) {
        point = ApplyBrushPressure(point, app->penPressure);
    }

    const auto distance = glm::distance(previousPoint.position, point.position);
    const int strokeNum = static_cast<int>(std::floor(distance / brushOptions.spacing));
    if (strokeNum == 0) {
        return;
    }

    const auto startPoint = previousPoint;
    const auto endPoint = point;

    static int round = 0;
    round = (round + 1) % 4;
    glm::vec4 debugColor;
    ImGui::ColorConvertHSVtoRGB((float)round / 4.0f, 0.8f, 1.0f, debugColor.r, debugColor.g, debugColor.b);

    const auto tStep = brushOptions.spacing / distance;
    float t = tStep;
    int i = 1;
    while (t < 1.0f) {
        ZoneScoped;

        StrokePoint newPoint;

        // Maybe use SIMD for this
        newPoint.position = glm::mix(startPoint.position, endPoint.position, t);
        newPoint.color = glm::mix(startPoint.color, endPoint.color, t);
        newPoint.flow = std::lerp(startPoint.flow, endPoint.flow, t);
        newPoint.radius = std::lerp(startPoint.radius, endPoint.radius, t);
        newPoint.hardness = std::lerp(startPoint.hardness, endPoint.hardness, t);

        strokePoints.push_back(newPoint);
        previousPoint = newPoint;

        t += tStep;
        i++;

        const auto tilesPos = GetTilePosAffectedByStrokePoint(newPoint);
        for (const auto& tilePos : tilesPos) {
            Tile strokeLayerTile = GetLoadedTileAt(strokeLayer, tilePos);
            if (strokeLayerTile == TILE_INVALID) {
                strokeLayerTile = CreateTile(strokeLayer, tilePos);
                SDL_assert(strokeLayerTile != TILE_INVALID && "Failed to create tile on stroke layer");
            }
            strokeTileAffected.insert(strokeLayerTile);

            if (!layerTilePos.at(selectedLayer).contains(tilePos)) {
                const auto selectedLayerTile = CreateTile(selectedLayer, tilePos);
                SDL_assert(selectedLayerTile != TILE_INVALID && "Failed to create tile on selected layer");
            }
            allTileStrokeAffected.insert(layerTilePos.at(selectedLayer).at(tilePos));
        }
    }
}

// This assumes every painted tiles are not going to be culled
void Canvas::EndBrushStroke(StrokePoint point) {
    ZoneScoped;
    SDL_assert(strokeStarted);
    SDL_assert(currentTileModificationCommand);

    currentTileModificationCommand->SavePreviousTilesTexture(allTileStrokeAffected);

    MergeLayer(strokeLayer, selectedLayer);
    DeleteLayer(strokeLayer);
    strokeLayer = 0;
    strokePoints.clear();

    currentTileModificationCommand->SaveNewTilesTexture(allTileStrokeAffected);

    canvasCommands.Push(std::move(currentTileModificationCommand));
    allTileStrokeAffected.clear();
    strokeStarted = false;

    SDL_assert(!currentTileModificationCommand);
    SDL_assert(!strokeStarted);
}

void Canvas::SaveBrush() {
#ifdef NDEBUG
    const std::string path = SDL_GetPrefPath(nullptr, "midori");
#else
    const std::string path = SDL_GetPrefPath(nullptr, "midori-dev");
#endif
    const std::string brushPath = std::format("{}brushes", path);
    SDL_CreateDirectory(brushPath.c_str());

    nlohmann::json brushJson = {
        {"color",
         {
             brushOptions.color.r,
             brushOptions.color.g,
             brushOptions.color.b,
             brushOptions.color.a,
         }},
        {"opacityPressure", brushOptions.opacityPressure},
        {"opacityPressureRange",
         {
             brushOptions.opacityPressureRange.x,
             brushOptions.opacityPressureRange.y,
         }},
        {"flow", brushOptions.flow},
        {"flowPressure", brushOptions.flowPressure},
        {"flowPressureRange",
         {
             brushOptions.flowPressureRange.x,
             brushOptions.flowPressureRange.y,
         }},
        {"radius", brushOptions.radius},
        {"radiusPressure", brushOptions.radiusPressure},
        {"radiusPressureRange",
         {
             brushOptions.radiusPressureRange.x,
             brushOptions.radiusPressureRange.y,
         }},
        {"hardness", brushOptions.hardness},
        {
            "hardnessPressure",
            brushOptions.hardnessPressure,
        },
        {"hardnessPressureRange",
         {
             brushOptions.hardnessPressureRange.x,
             brushOptions.hardnessPressureRange.y,
         }},
        {"spacing", brushOptions.spacing},
    };

    const std::string brushJsonDump = brushJson.dump(4);

    const std::string file = std::format("{}brushes/brush.json", path);
    SDL_IOStream* fileIo = SDL_IOFromFile(file.c_str(), "w");
    SDL_assert(fileIo != nullptr);
    SDL_WriteIO(fileIo, brushJsonDump.data(), brushJsonDump.size());
    SDL_CloseIO(fileIo);
    brushOptionsModified = false;
}

void Canvas::OpenBrush() {
#ifdef NDEBUG
    const std::string path = SDL_GetPrefPath(nullptr, "midori");
#else
    const std::string path = SDL_GetPrefPath(nullptr, "midori-dev");
#endif
    const std::string file = std::format("{}brushes/brush.json", path);

    size_t size;
    char* buf = (char*)SDL_LoadFile(file.c_str(), &size);
    if (size == 0) {
        brushOptionsModified = true;
        return;
    }

    auto layerJson = nlohmann::json::parse(buf);
    SDL_free(buf);

    brushOptions.color.r = layerJson.at("color")[0];
    brushOptions.color.g = layerJson.at("color")[1];
    brushOptions.color.b = layerJson.at("color")[2];
    brushOptions.color.a = layerJson.at("color")[3];

    brushOptions.opacityPressure = layerJson.at("opacityPressure");
    brushOptions.opacityPressureRange.x = layerJson.at("opacityPressureRange")[0];
    brushOptions.opacityPressureRange.y = layerJson.at("opacityPressureRange")[1];

    brushOptions.flow = layerJson.at("flow");
    brushOptions.flowPressure = layerJson.at("flowPressure");
    brushOptions.flowPressureRange.x = layerJson.at("flowPressureRange")[0];
    brushOptions.flowPressureRange.y = layerJson.at("flowPressureRange")[1];

    brushOptions.radius = layerJson.at("radius");
    brushOptions.radiusPressure = layerJson.at("radiusPressure");
    brushOptions.radiusPressureRange.x = layerJson.at("radiusPressureRange")[0];
    brushOptions.radiusPressureRange.y = layerJson.at("radiusPressureRange")[1];

    brushOptions.hardness = layerJson.at("hardness");
    brushOptions.hardnessPressure = layerJson.at("hardnessPressure");
    brushOptions.hardnessPressureRange.x = layerJson.at("hardnessPressureRange")[0];
    brushOptions.hardnessPressureRange.y = layerJson.at("hardnessPressureRange")[1];

    brushOptions.spacing = layerJson.at("spacing");
}

void Canvas::SaveEraser() {
#ifdef NDEBUG
    const std::string path = SDL_GetPrefPath(nullptr, "midori");
#else
    const std::string path = SDL_GetPrefPath(nullptr, "midori-dev");
#endif
    const std::string eraserPath = std::format("{}brushes", path);
    SDL_CreateDirectory(eraserPath.c_str());

    nlohmann::json eraserJson = {
        {"opacity", eraserOptions.opacity},
        {"opacityPressure", eraserOptions.opacityPressure},
        {"opacityPressureRange",
         {
             eraserOptions.opacityPressureRange.x,
             eraserOptions.opacityPressureRange.y,
         }},
        {"flow", eraserOptions.flow},
        {"flowPressure", eraserOptions.flowPressure},
        {"flowPressureRange",
         {
             eraserOptions.flowPressureRange.x,
             eraserOptions.flowPressureRange.y,
         }},
        {"radius", eraserOptions.radius},
        {"radiusPressure", eraserOptions.radiusPressure},
        {"radiusPressureRange",
         {
             eraserOptions.radiusPressureRange.x,
             eraserOptions.radiusPressureRange.y,
         }},
        {"hardness", eraserOptions.hardness},
        {
            "hardnessPressure",
            eraserOptions.hardnessPressure,
        },
        {"hardnessPressureRange",
         {
             eraserOptions.hardnessPressureRange.x,
             eraserOptions.hardnessPressureRange.y,
         }},
        {"spacing", eraserOptions.spacing},
    };

    const std::string eraserJsonDump = eraserJson.dump(4);

    const std::string file = std::format("{}brushes/eraser.json", path);
    SDL_IOStream* fileIo = SDL_IOFromFile(file.c_str(), "w");
    SDL_assert(fileIo != nullptr);
    SDL_WriteIO(fileIo, eraserJsonDump.data(), eraserJsonDump.size());
    SDL_CloseIO(fileIo);
    eraserOptionsModified = false;
}

void Canvas::OpenEraser() {
#ifdef NDEBUG
    const std::string path = SDL_GetPrefPath(nullptr, "midori");
#else
    const std::string path = SDL_GetPrefPath(nullptr, "midori-dev");
#endif
    const std::string file = std::format("{}brushes/eraser.json", path);

    size_t size;
    char* buf = (char*)SDL_LoadFile(file.c_str(), &size);
    if (size == 0) {
        eraserOptionsModified = true;
        return;
    }

    auto layerJson = nlohmann::json::parse(buf);
    SDL_free(buf);

    eraserOptions.opacity = layerJson.at("opacity");

    eraserOptions.opacityPressure = layerJson.at("opacityPressure");
    eraserOptions.opacityPressureRange.x = layerJson.at("opacityPressureRange")[0];
    eraserOptions.opacityPressureRange.y = layerJson.at("opacityPressureRange")[1];

    eraserOptions.flow = layerJson.at("flow");
    eraserOptions.flowPressure = layerJson.at("flowPressure");
    eraserOptions.flowPressureRange.x = layerJson.at("flowPressureRange")[0];
    eraserOptions.flowPressureRange.y = layerJson.at("flowPressureRange")[1];

    eraserOptions.radius = layerJson.at("radius");
    eraserOptions.radiusPressure = layerJson.at("radiusPressure");
    eraserOptions.radiusPressureRange.x = layerJson.at("radiusPressureRange")[0];
    eraserOptions.radiusPressureRange.y = layerJson.at("radiusPressureRange")[1];

    eraserOptions.hardness = layerJson.at("hardness");
    eraserOptions.hardnessPressure = layerJson.at("hardnessPressure");
    eraserOptions.hardnessPressureRange.x = layerJson.at("hardnessPressureRange")[0];
    eraserOptions.hardnessPressureRange.y = layerJson.at("hardnessPressureRange")[1];

    eraserOptions.spacing = layerJson.at("spacing");
}

void Canvas::ChangeRadiusSize(glm::vec2 cursorDelta, bool slowMode) {
    if (slowMode) {
        cursorDelta *= 0.25f;
    }

    if (brushMode) {
        brushOptions.radius += cursorDelta.x;
    } else if (eraserMode) {
        eraserOptions.radius += cursorDelta.x;
    }
}

std::vector<Color> Canvas::DownloadCanvasTexture(glm::ivec2& size) const {
    ZoneScoped;

    SDL_assert(app->windowSize.x > 0);
    SDL_assert(app->windowSize.y > 0);
    SDL_assert(app->renderer.canvasTexture);

    size.x = app->windowSize.x;
    size.y = app->windowSize.y;

    std::vector<Color> canvasTexture(static_cast<std::size_t>(app->windowSize.x)
                                     * static_cast<std::size_t>(app->windowSize.y));

    const SDL_GPUTransferBufferCreateInfo transferBufferCreateInfo = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        .size = static_cast<Uint32>(canvasTexture.size() * sizeof(Color)),
    };
    SDL_GPUTransferBuffer* transferBuffer =
        SDL_CreateGPUTransferBuffer(app->renderer.device, &transferBufferCreateInfo);
    SDL_assert(transferBuffer);

    SDL_GPUCommandBuffer* cmdBuf = SDL_AcquireGPUCommandBuffer(app->renderer.device);
    SDL_assert(cmdBuf);

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmdBuf);
    SDL_GPUTextureRegion region = {
        .texture = app->renderer.canvasTexture,
        .mip_level = 0,
        .layer = 0,
        .x = 0,
        .y = 0,
        .z = 0,
        .w = static_cast<Uint32>(app->windowSize.x),
        .h = static_cast<Uint32>(app->windowSize.y),
        .d = 1,
    };
    const SDL_GPUTextureTransferInfo transferInfo = {
        .transfer_buffer = transferBuffer,
        .pixels_per_row = static_cast<Uint32>(app->windowSize.x),
        .rows_per_layer = static_cast<Uint32>(app->windowSize.y),
    };
    SDL_DownloadFromGPUTexture(copyPass, &region, &transferInfo);

    SDL_EndGPUCopyPass(copyPass);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdBuf);
    SDL_assert(fence);
    SDL_WaitForGPUFences(app->renderer.device, true, &fence, 1);
    SDL_ReleaseGPUFence(app->renderer.device, fence);

    const auto* colors = static_cast<Color*>(SDL_MapGPUTransferBuffer(app->renderer.device, transferBuffer, false));

    std::memcpy(canvasTexture.data(), colors, canvasTexture.size() * sizeof(Color));

    SDL_UnmapGPUTransferBuffer(app->renderer.device, transferBuffer);
    SDL_ReleaseGPUTransferBuffer(app->renderer.device, transferBuffer);

    return canvasTexture;
}

bool Canvas::SampleTexture(const std::vector<Color>& texture, glm::ivec2 textureSize, glm::vec2 pos, Color& color) {
    SDL_assert(textureSize.x > 0);
    SDL_assert(textureSize.y > 0);
    SDL_assert(texture.size() == static_cast<std::size_t>(textureSize.x) * textureSize.y
               && "SampleTexture is of wrong dimensions");

    const auto rx = static_cast<int>(std::round(pos.x));
    const auto ry = static_cast<int>(std::round(pos.y));

    if (rx < 0 || rx >= textureSize.x) {
        return false;
    }
    if (ry < 0 || ry >= textureSize.y) {
        return false;
    }

    const std::size_t index = static_cast<std::size_t>(rx) + (static_cast<std::size_t>(ry) * textureSize.x);
    SDL_assert(index < texture.size() && "Sample out of bound");

    color = texture[index];
    return true;
}

Canvas::StrokePoint Canvas::ApplyEraserPressure(StrokePoint point, const float pressure) const {
    if (eraserOptions.opacityPressure) {
        point.color.a *= Remap(pressure, eraserOptions.opacityPressureRange.x, eraserOptions.opacityPressureRange.y);
    }
    if (eraserOptions.radiusPressure) {
        point.radius *= Remap(pressure, eraserOptions.radiusPressureRange.x, eraserOptions.radiusPressureRange.y);
    }
    if (eraserOptions.flowPressure) {
        point.flow *= Remap(pressure, eraserOptions.flowPressureRange.x, eraserOptions.flowPressureRange.y);
    }
    if (eraserOptions.hardnessPressure) {
        point.hardness *= Remap(pressure, eraserOptions.hardnessPressureRange.x, eraserOptions.hardnessPressureRange.y);
    }

    return point;
}

// This assumes every painted tiles are not going to be culled
void Canvas::StartEraserStroke(StrokePoint point) {
    ZoneScoped;
    SDL_assert(selectedLayer && "No layer selected");
    SDL_assert(!strokeStarted && "Stroke already started");
    SDL_assert(!currentTileModificationCommand && "TileModification already started");

    strokeStarted = true;
    currentTileModificationCommand = std::make_unique<TileModificationCommand>(this, selectedLayer);

    if (app->penInRange) {
        point = ApplyEraserPressure(point, app->penPressure);
    }

    const auto tilesPos = GetTilePosAffectedByStrokePoint(point);

    // TODO: find a better algorithm when erasing on layers. Right now the opacity of the erase brush does not work.
    // TODO: find a way to duplicate the layer this early and instead use a temporary internal layer as the
    // modification source
    std::unordered_set<Tile> tileTexturesToSave;
    for (const auto& tilePos : tilesPos) {
        Tile tile = GetLoadedTileAt(selectedLayer, tilePos);
        if (tile != TILE_INVALID) {
            strokeTileAffected.insert(tile);
            layerTilesModified[selectedLayer].insert(tile);
            allTileStrokeAffected.insert(layerTilePos.at(selectedLayer).at(tilePos));
            tileTexturesToSave.insert(tile);
        }
    }

    currentTileModificationCommand->SavePreviousTilesTexture(tileTexturesToSave);

    // stroke_points.push_back(point);
    strokePoints.clear();
    previousPoint = point;

    SDL_assert(strokeStarted);
    SDL_assert(currentTileModificationCommand);
}

// This assumes every painted tiles are not going to be culled
void Canvas::UpdateEraserStroke(StrokePoint point) {
    ZoneScoped;
    SDL_assert(strokeStarted);

    if (app->penInRange) {
        point = ApplyEraserPressure(point, app->penPressure);
    }

    const auto distance = glm::distance(previousPoint.position, point.position);
    const int strokeNum = static_cast<int>(std::floor(distance / eraserOptions.spacing));
    if (strokeNum == 0) {
        return;
    }

    const auto startPoint = previousPoint;
    const auto endPoint = point;

    static int round = 0;
    round = (round + 1) % 4;
    glm::vec4 debugColor;
    ImGui::ColorConvertHSVtoRGB((float)round / 4.0f, 0.8f, 1.0f, debugColor.r, debugColor.g, debugColor.b);

    std::unordered_set<Tile> tileTexturesToSave;

    const auto tStep = eraserOptions.spacing / distance;
    float t = tStep;
    while (t < 1.0f) {
        ZoneScoped;

        StrokePoint newPoint;

        // Maybe use SIMD for this
        newPoint.position = glm::mix(startPoint.position, endPoint.position, t);
        newPoint.color = glm::mix(startPoint.color, endPoint.color, t);
        newPoint.flow = std::lerp(startPoint.flow, endPoint.flow, t);
        newPoint.radius = std::lerp(startPoint.radius, endPoint.radius, t);
        newPoint.hardness = std::lerp(startPoint.hardness, endPoint.hardness, t);

        strokePoints.push_back(newPoint);
        previousPoint = newPoint;

        t += tStep;

        const auto tilesPos = GetTilePosAffectedByStrokePoint(newPoint);

        for (const auto& tilePos : tilesPos) {
            Tile tile = GetLoadedTileAt(selectedLayer, tilePos);
            if (tile != TILE_INVALID) {
                strokeTileAffected.insert(tile);
                layerTilesModified[selectedLayer].insert(tile);
                if (!allTileStrokeAffected.contains(tile)) {
                    allTileStrokeAffected.insert(layerTilePos.at(selectedLayer).at(tilePos));
                    tileTexturesToSave.insert(tile);
                }
            }
        }
    }

    currentTileModificationCommand->SavePreviousTilesTexture(tileTexturesToSave);
}

// This assumes every painted tiles are not going to be culled
void Canvas::EndEraserStroke(StrokePoint point) {
    ZoneScoped;
    SDL_assert(strokeStarted && "Stroke must be started to end it");
    SDL_assert(currentTileModificationCommand && "tile modification not started");

    currentTileModificationCommand->SaveNewTilesTexture(allTileStrokeAffected);
    canvasCommands.Push(std::move(currentTileModificationCommand));

    strokePoints.clear();
    allTileStrokeAffected.clear();

    strokeStarted = false;

    SDL_assert(!strokeStarted);
    SDL_assert(!currentTileModificationCommand);
}
} // namespace Midori
