#include "canvas.hpp"

#include "SDL3/SDL_gpu.h"
#include "SDL3/SDL_log.h"
#include "app.hpp"
#include "base.hpp"
#include "brushes.hpp"
#include "defines.hpp"
#include "layers.hpp"
#include "renderer.hpp"
#include "states.hpp"
#include "tiles.hpp"
#include <optional>

#define QOI_IMPLEMENTATION
#define QOI_NO_STDIO
#include "../deps/qoi.h"
#include "../deps/json.hpp"
#include <SDL3/SDL_assert.h>
#include <tracy/Tracy.hpp>
#include <imgui.h>
#include <string>
#include <utility>
#include <algorithm>
#include <format>

namespace Midori {

// Canvas

Canvas::Canvas(App* app) : app(app), canvasCommands(256), viewCommands(1024) {
    layersTree.reserve(layers.capacity());
}

SDL_EnumerationResult FindLayersCallback(void* userdata, const char* dirname, const char* fname) {
    ZoneScopedN("Parsing layer");
    if (dirname != nullptr) {
        auto* canvas = (Canvas*)userdata;
        const std::string folder = std::format("{}{}", dirname, fname);
        const std::string path = std::format("{}/layer.json", folder);

        size_t size;
        char* buf = (char*)SDL_LoadFile(path.c_str(), &size);
        SDL_assert(buf && size);

        const auto layerJson = nlohmann::json::parse(buf);

        LayerCreateInfo createInfo{};
        createInfo.opacity = layerJson.at("opacity");
        createInfo.hidden = layerJson.at("hidden");
        createInfo.name = layerJson.at("name");
        createInfo.height = layerJson.at("height");
        const auto layer = canvas->CreateLayer(createInfo, layerJson.at("id"));
        SDL_assert(layer != LAYER_INVALID);

        int tilesCount{};
        const auto* tilesFilenames = SDL_GlobDirectory(folder.c_str(), "*.qoi", 0, &tilesCount);
        Tile tile{
            .layer = layer,
        };
        for (int i = 0; i < tilesCount; i++) {
            int stage{};

            std::string tmp;
            const size_t len = strlen(tilesFilenames[i]);
            tmp.reserve(len);

            for (size_t c = 0; c < len; c++) {
                if (tilesFilenames[i][c] == '.') {
                    SDL_assert(stage == 1 && "Unexpected tile name format");
                    if (stage == 1) {
                        tile.y = static_cast<TileAxis>(std::atoi(tmp.c_str()));
                        stage++;
                    }
                    break;
                } else if (tilesFilenames[i][c] == '_') {
                    tile.x = static_cast<TileAxis>(std::atoi(tmp.c_str()));
                    tmp.clear();
                    stage++;
                } else {
                    tmp += tilesFilenames[i][c];
                }
            }

            SDL_assert(stage == 2 && "Unexpected tile name format");
            canvas->layers.at(layer).tiles.insert(tile);
        }
    }

    return SDL_ENUM_CONTINUE;
}

bool Canvas::CanQuit() const {
    return tilesWriting.empty();
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

    if (exists) {
        SDL_EnumerateDirectory(filename.c_str(), FindLayersCallback, this);
    }

    if (layers.empty()) {
        LayerCreateInfo createInfo{};
        createInfo.name = "Base Layer";
        const auto layer = CreateLayer(createInfo);
        SDL_assert(layer != LAYER_INVALID);
        SaveLayer(layer);
    }

    selectedLayer = layers.begin().pos;

    // temp
    OpenEraseBrush();
    OpenPaintBrush();

    if (paintBrushModified) {
        SavePaintBrush();
    }

    if (eraseBrushModified) {
        SaveEraseBrush();
    }

    return true;
}

void Canvas::Update() {
    ZoneScoped;

    UpdateTileVisibility(viewport);

    UpdateTileReading();
    UpdateTileWriting();
}

void Canvas::UpdateTileVisibility(Viewport& viewport) {
    ZoneScoped;

    const auto& visibleTiles = viewport.VisibleTiles(app->frameArena);
    for (const auto& loadedTile : tilesLoaded) {
        if (layers.at(loadedTile.layer).hidden || !viewport.IsTileVisible({loadedTile.x, loadedTile.y})) {
            UnloadTile(loadedTile);
        }
    }

    for (const auto& layer : layersTree) {
        auto& layerInfo = layers.at(layer);
        for (const auto& visibleTile : visibleTiles) {
            const Tile tile{
                .layer = layer,
                .y = static_cast<TileAxis>(visibleTile.y),
                .x = static_cast<TileAxis>(visibleTile.x),
            };

            if (!tilesLoaded.contains(tile) && layerInfo.tiles.contains(tile)) {
                LoadTile(tile);
            }
        }
    }
}

Layer Canvas::CreateLayer(LayerCreateInfo createInfo) {
    ZoneScoped;
    const Layer layer = layers.free_slot();
    if (layer == LAYER_INVALID) {
        return layer;
    }

    return CreateLayer(std::move(createInfo), layer);
}

Layer Canvas::CreateLayer(LayerCreateInfo createInfo, Layer layer) {
    ZoneScoped;
    SDL_assert(layer != LAYER_INVALID);

    createInfo.height = std::min(createInfo.height, static_cast<LayerHeight>(layersTree.size()));

    LayerInfo info{
        .id = layer,
        .height = createInfo.height,
        .opacity = createInfo.opacity,
        .blendMode = createInfo.blendMode,
        .hidden = createInfo.hidden,
        .internal = createInfo.internal,
        .modified = true,
    };

    if (!layers.emplace_at(layer, std::move(info))) {
        return LAYER_INVALID;
    }

    if (!app->renderer.CreateLayerTexture(layer)) {
        layers.erase(layer);
        return LAYER_INVALID;
    }

    layersTree.push_back(LAYER_INVALID);

    Layer prev = LAYER_INVALID;
    Layer next = layersTree[createInfo.height];
    layersTree[createInfo.height] = layer;
    for (size_t i = createInfo.height + 1; i < layersTree.size(); i++) {
        prev = next;
        next = layersTree[i];
        layersTree[i] = prev;
        layers.at(prev).height++;
        layers.at(prev).modified = true;
    }

    layers.at(layer).tiles.reserve(TILES_MAX);

    return layer;
}

bool Canvas::SaveLayer(Layer layer) {
    ZoneScoped;
    SDL_assert(layers.contains(layer));
    const auto& layerInfo = layers.at(layer);
    std::string path = std::format("{}/{}", filename, layerInfo.id);

    SDL_CreateDirectory(path.c_str());

    nlohmann::json layerJson = {
        {"id", layerInfo.id},         {"name", layerInfo.name},     {"opacity", layerInfo.opacity},
        {"hidden", layerInfo.hidden}, {"height", layerInfo.height},
    };
    std::string layerDump = layerJson.dump(4);

    std::string file = std::format("{}/layer.json", path);
    SDL_IOStream* fileIo = SDL_IOFromFile(file.c_str(), "w");
    SDL_assert(fileIo != nullptr);
    SDL_WriteIO(fileIo, layerDump.data(), layerDump.size());
    SDL_CloseIO(fileIo);
    layers.at(layer).modified = false;
    return true;
}

Layer Canvas::DuplicateLayer(Layer layer, bool temporary) {
    ZoneScoped;
    SDL_assert(layers.contains(layer));

    const auto& layerInfo = layers.at(layer);
    LayerCreateInfo createInfo{
        .name = layerInfo.name,
        .height = layerInfo.height,
        .opacity = layerInfo.opacity,
        .blendMode = layerInfo.blendMode,
        .hidden = layerInfo.hidden,
        .internal = temporary,
    };
    createInfo.height++;
    Layer newLayer = CreateLayer(createInfo);
    if (newLayer == LAYER_INVALID) {
        return newLayer;
    }

    // FIXME This is broken. It needs a temp file or an alternate file like untitled.mido.temp for all the temporary
    // stuff, since when moving the tile data will be lost. This only duplicate the layer not it tiles
    if (!temporary) {
        std::string layerPath = std::format("{}/{}", filename, layer);
        std::string newLayerPath = std::format("{}/{}", filename, newLayer);
        std::filesystem::copy(layerPath, newLayerPath);
        SaveLayer(newLayer);
    }

    return newLayer;
}

bool Canvas::MoveLayer(Layer layer, LayerHeight newHeight) {
    ZoneScoped;
    SDL_assert(layers.contains(layer) && "Layer not found");
    SDL_assert(!layersTree.empty());

    if (layers.size() == 1) {
        return true;
    }

    const LayerHeight oldHeight = layers.at(layer).height;
    newHeight = std::min(static_cast<LayerHeight>(layersTree.size() - 1), newHeight);

    if (newHeight > oldHeight) {
        const size_t start = oldHeight;
        const size_t end = newHeight;
        for (size_t i = start; i < end; i++) {
            Layer above = layersTree[i + 1];
            layersTree[i] = above;
            layers.at(above).height--;
            layers.at(above).modified = true;
        }

    } else {
        Layer above = LAYER_INVALID;
        const size_t start = newHeight;
        const size_t end = oldHeight;
        for (size_t i = start; i < end; i++) {
            Layer current = above;
            above = layersTree[i + 1];
            layersTree[i + 1] = current;
            layers.at(current).height++;
            layers.at(current).modified = true;
        }
    }

    layersTree[newHeight] = layer;
    layers.at(layer).height = newHeight;
    layers.at(layer).modified = true;

    return true;
}

bool Canvas::MergeLayers(const Layer srcLayer, const Layer dstLayer) {
    SDL_assert(layers.contains(srcLayer));
    SDL_assert(layers.contains(dstLayer));

    // TODO take into account the tiles not loaded but stored

    size_t failed{};
    for (const auto& tile : tilesLoaded) {
        if (tile.layer == srcLayer) {
            Tile target{
                .layer = dstLayer,
                .y = tile.y,
                .x = tile.x,
            };
            if (!MergeTiles(tile, target)) {
                failed++;
            }
        }
    }

    return (failed == 0);
}

bool Canvas::CreateTile(Tile tile, bool initialize) {
    ZoneScoped;
    SDL_assert(layers.contains(tile.layer) && "Layer missing");
    SDL_assert(!tilesLoaded.contains(tile) && "Tile already loaded");
    SDL_assert(!layers.at(tile.layer).tiles.contains(tile) && "Tile already exists");

    if (initialize) {
        const Renderer::Error err = app->renderer.CreateTileTexture(tile);
        SDL_assert(err == Renderer::Error::None);
    } else {
        const Renderer::Error err = app->renderer.CreateUninitializedTileTexture(tile);
        SDL_assert(err == Renderer::Error::None);
    }

    tilesLoaded.insert(tile);
    MarkTileDirty(tile);

    return true;
}

bool Canvas::DestroyTile(Tile tile) {
    ZoneScoped;
    if (!tilesLoaded.contains(tile)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Tried to destroy tile not loaded");
        return false;
    }

    app->renderer.DestroyTileTexture(tile);
    tilesLoaded.erase(tile);
    return true;
}

bool Canvas::UnloadTile(Tile tile) {
    ZoneScoped;
    SDL_assert(layers.contains(tile.layer) && "Layer not found");
    SDL_assert(tilesLoaded.contains(tile) && "Tile not found");

    if (tilesModified.contains(tile)) {
        return SaveTile(tile);
    } else {
        DestroyTile(tile);
    }

    return true;
}

bool Canvas::SaveTile(Tile tile) {
    ZoneScoped;

    if (!tilesModified.contains(tile)) {
        return true;
    }
    if (tilesWriting.contains(tile)) {
        return false;
    }

    const TileWriteData data{
        .tile = tile,
        .state = TileWriteState::Pending,
    };
    if (!tilesWriting.insert(tile, data)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Failed queue tile(%u, %d, %d) for saving", tile.layer, tile.x,
                    tile.y);
        return false;
    }

    return true;
}

bool Canvas::LoadTile(Tile tile) {
    ZoneScoped;
    SDL_assert(layers.at(tile.layer).tiles.contains(tile) && "Tile not saved, cannot load it");

    // The tile is still queued for unloading (being written to disk) and its GPU
    // texture is therefore still resident. Cancel the pending write and re-adopt
    // that texture instead of creating a duplicate, which would fail because
    // tileTextures already holds an entry for this tile.
    if (tilesWriting.contains(tile) && app->renderer.tileTextures.contains(tile)) {
        tilesWriting.erase(tile);
        tilesLoaded.insert(tile);
        return true;
    }

    if (tilesReading.contains(tile)) {
        return false;
    }
    if (app->renderer.CreateUninitializedTileTexture(tile) != Renderer::Error::None) {
        return false;
    }
    const TileReadData data{
        .tile = tile,
        .state = TileReadState::Pending,
    };
    tilesReading.emplace(tile, data);
    return true;
}

bool Canvas::DeleteTile(Tile tile) {
    ZoneScoped;
    if (!layers.at(tile.layer).tiles.contains(tile)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Tried to delete tile not saved");
        return false;
    }

    SDL_assert(!tilesReading.contains(tile) && "Haven't figured this out yet :/");

    tilesModified.erase(tile);
    const std::string tilePath = std::format("{}/{}/{}_{}.qoi", filename, tile.layer, tile.x, tile.y);
    SDL_RemovePath(tilePath.c_str());
    layers.at(tile.layer).tiles.erase(tile);

    return true;
}

bool Canvas::DeleteLayer(Layer layer) {
    ZoneScoped;
    SDL_assert(layers.contains(layer) && "Layer not found");

    // FIXME: figure tileLoading
    // FIXME: figure tileUnloading

    for (const auto& tile : tilesLoaded) {
        if (tile.layer == layer) {
            DestroyTile(tile);
        }
    }

    for (const auto& tile : layers.at(layer).tiles) {
        SDL_assert(!tilesReading.contains(tile) && "Haven't firgured this out yet :/");

        tilesModified.erase(tile);
        const std::string tilePath = std::format("{}/{}/{}_{}.qoi", filename, tile.layer, tile.x, tile.y);
        SDL_RemovePath(tilePath.c_str());
    }
    layers.at(layer).tiles.clear();

    const size_t start = layers.at(layer).height;
    const size_t end = layersTree.size() - 1;
    for (size_t i = start; i < end; i++) {
        Layer above = layersTree[i + 1];
        layersTree[i] = above;
        layers.at(above).height--;
        layers.at(above).modified = true;
    }
    layersTree.pop_back();

    app->renderer.DestroyLayerTexture(layer);
    const std::string layerInfoPath = std::format("{}/{}/layer.json", filename, layer);
    SDL_RemovePath(layerInfoPath.c_str());
    const std::string layerPath = std::format("{}/{}", filename, layer);
    SDL_RemovePath(layerPath.c_str());
    layers.erase(layer);

    return true;
}

// TODO make Tile/Layer action work on persitant data (saved to disk) and not exclusivly on resident data (ram/vram)

bool Canvas::MergeTiles(Tile srcTile, Tile dstTile) {
    ZoneScoped;
    SDL_assert(layers.contains(srcTile.layer));
    SDL_assert(layers.contains(dstTile.layer));
    SDL_assert(tilesLoaded.contains(srcTile));

    if (tilesLoaded.contains(dstTile)) {
        app->renderer.MergeTileTextures(srcTile, dstTile);

        MarkTileDirty(dstTile);
    } else {
        if (!MoveTile(srcTile, dstTile)) {
            return false;
        }
    }

    return true;
}

bool Canvas::MoveTile(Tile srcTile, Tile dstTile) {
    ZoneScoped;
    SDL_assert(layers.contains(srcTile.layer));
    SDL_assert(layers.contains(dstTile.layer));
    SDL_assert(tilesLoaded.contains(srcTile));

    tilesLoaded.insert(dstTile);
    MarkTileDirty(dstTile);

    // FIXME Duplicate the texture to avoid weird errors
    const auto& tileTexture = app->renderer.tileTextures.at(srcTile);
    app->renderer.tileTextures.insert(dstTile, tileTexture);

    app->renderer.tileTextures.erase(srcTile);
    tilesLoaded.erase(srcTile);
    if (layers.at(srcTile.layer).tiles.contains(srcTile)) {
        if (!DeleteTile(srcTile)) {
            return false;
        }
    }

    return true;
}

void Canvas::MarkTileDirty(Tile tile) {
    ZoneScoped;
    if (!layers.contains(tile.layer)) {
        return;
    }
    if (layers.at(tile.layer).internal) {
        return;
    }
    tilesModified.insert(tile);
}

// TODO: Make multithreaded
bool Canvas::UpdateTileReading() {
    ZoneScoped;

    std::vector<Tile, arena_allocator<Tile>> tilesUnqueued(app->frameArena);
    for (auto [tile, data] : tilesReading) {
        SDL_assert(!tilesLoaded.contains(tile));

        if (data.state == TileReadState::Pending) {
            ZoneScopedN("Reading Tile");
            const std::string tileFilename = std::format("{}/{}/{}_{}.qoi", filename, tile.layer, tile.x, tile.y);

            auto slot = app->renderer.ReserveTileTextureUpload(tile);
            if (!slot.has_value()) {
                continue;
            }
            data.rendererSlot = slot.value();
            auto& info = app->renderer.GetTileTextureUploadInfo(data.rendererSlot);

            size_t encodedBufSize;
            auto* encodedBuf = static_cast<std::byte*>(SDL_LoadFile(tileFilename.c_str(), &encodedBufSize));
            if (!encodedBuf) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read tile texture");
                app->renderer.FreeTileTextureUpload(data.rendererSlot);
                tilesUnqueued.push_back(data.tile);
            }

            qoi_desc desc{};
            auto* decodedBuf =
                static_cast<std::byte*>(qoi_decode(encodedBuf, static_cast<int>(encodedBufSize), &desc, 4));
            SDL_free(encodedBuf);

            if (!decodedBuf) {
                app->renderer.FreeTileTextureUpload(data.rendererSlot);
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to decode tile texture");
                tilesUnqueued.push_back(data.tile);
            }

            memcpy(info.data, decodedBuf, TILE_SIZE);
            free(decodedBuf);

            app->renderer.CommitTileTextureUpload(data.rendererSlot);
            data.state = TileReadState::Uploaded;
            tilesUnqueued.push_back(tile);
            tilesLoaded.insert(tile);
        }
    }
    for (const auto& tile : tilesUnqueued) {
        tilesReading.erase(tile);
    }

    return true;
}

// TODO: Make multithreaded
bool Canvas::UpdateTileWriting() {
    ZoneScoped;

    std::vector<Tile, arena_allocator<Tile>> tileUnloaded(app->frameArena);
    for (auto [tile, data] : tilesWriting) {
        SDL_assert(tilesModified.contains(tile));

        if (data.state == TileWriteState::Pending) {
            const auto slot = app->renderer.DownloadTileTexture(tile);
            if (!slot.has_value()) {
                continue;
            }
            data.rendererSlot = slot.value();
            data.state = TileWriteState::Downloading;
        }
        if (data.state == TileWriteState::Downloading) {
            if (!app->renderer.QueryTileTextureDownload(data.rendererSlot)) {
                continue;
            }
            auto& info = app->renderer.GetTileTextureDownloadInfo(data.rendererSlot);
            bool empty{true};
            for (size_t i = 0; i < TILE_SIZE; i++) {
                if (static_cast<uint8_t>(info.data[i]) != 0) {
                    empty = false;
                    break;
                }
            }
            if (empty) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Detected empty tile");
                app->renderer.FreeTileTextureDownload(data.rendererSlot);
                DeleteTile(tile);
                data.state = TileWriteState::Done;
            } else {
                data.state = TileWriteState::Encoding;
            }
        }
        if (data.state == TileWriteState::Encoding) {
            ZoneScopedN("Writing tile");
            auto& info = app->renderer.GetTileTextureDownloadInfo(data.rendererSlot);
            const qoi_desc desc = {
                .width = TILE_WIDTH,
                .height = TILE_HEIGHT,
                .channels = 4,
                .colorspace = QOI_LINEAR,
            };
            int encodedBufSize = 0;
            auto* encodedBuf = static_cast<std::byte*>(qoi_encode(info.data, &desc, &encodedBufSize));
            SDL_assert(encodedBufSize > 0);
            SDL_assert(encodedBuf != nullptr);
            SDL_assert(static_cast<size_t>(encodedBufSize) <= TILE_ENCODED_MAX_SIZE
                       && "Encoded tile larger than buffer");

            app->renderer.FreeTileTextureDownload(data.rendererSlot);

            // data.encoded = tileTextureEncodedPool.allocate(1);
            // data.encoded->size = encodedBufSize;
            // memcpy(data.encoded->data, encodedBuf, encodedBufSize);

            const std::string tileFilename = std::format("{}/{}/{}_{}.qoi", filename, tile.layer, tile.x, tile.y);

            SDL_IOStream* fileIo = SDL_IOFromFile(tileFilename.c_str(), "wb");
            SDL_assert(fileIo != nullptr);
            SDL_WriteIO(fileIo, encodedBuf, encodedBufSize);
            SDL_CloseIO(fileIo);

            free(encodedBuf);

            // tileTextureEncodedPool.deallocate(data.encoded, 1);
            // data.encoded->size = 0;

            layers.at(tile.layer).tiles.insert(tile);
            tilesModified.erase(tile);
            data.state = TileWriteState::Done;
        }
        if (data.state == TileWriteState::Done) {
            DestroyTile(tile);
            tileUnloaded.push_back(tile);
        }
    }

    {
        ZoneScopedN("Unqueing unload queue");
        for (const auto& tile : tileUnloaded) {
            tilesWriting.erase(tile);
        }
    }

    return true;
}

std::vector<glm::ivec2, arena_allocator<glm::ivec2>> Canvas::GetTilePosAffectedByStrokePoint(Canvas::StrokePoint point,
                                                                                             arena<>& arena) {
    constexpr glm::vec2 TILE_SIZE = glm::vec2(TILE_WIDTH, TILE_HEIGHT);
    std::vector<glm::ivec2, arena_allocator<glm::ivec2>> tilesPos(arena);

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

Canvas::StrokePoint LerpStrokePoint(Canvas::StrokePoint start, Canvas::StrokePoint end, float t) {
    start.position = glm::mix(start.position, end.position, t);
    start.color = glm::mix(start.color, end.color, t);
    start.flow = std::lerp(start.flow, end.flow, t);
    start.radius = std::lerp(start.radius, end.radius, t);
    start.hardness = std::lerp(start.hardness, end.hardness, t);
    return start;
}

void Canvas::StartPaintStroke(StrokePoint point) {
    ZoneScoped;
    SDL_assert(currentTool == Tool::Paint);
    SDL_assert(currentToolState == ToolState::None);
    SDL_assert(layers.contains(selectedLayer));
    SDL_assert(!layers.contains(strokeLayer));
    SDL_assert(!currentTileModificationCommand);

    strokeArena.reset();
    std::construct_at(&strokePoints, strokeArena);
    strokePoints.reserve(8192);
    strokeTileAffected.reserve(256);

    currentTileModificationCommand = TileModificationCommand(this, selectedLayer);

    const auto& selectedLayerInfo = layers.at(selectedLayer);
    LayerCreateInfo strokeLayerCreateInfo{};
    strokeLayerCreateInfo.name = "Stroke Layer";
    strokeLayerCreateInfo.height = selectedLayerInfo.height;
    strokeLayerCreateInfo.opacity = selectedLayerInfo.opacity; // FIXME: this is broken, the blending is bad
    strokeLayerCreateInfo.internal = true;
    strokeLayer = CreateLayer(strokeLayerCreateInfo);
    SDL_assert(strokeLayer != LAYER_INVALID);

    const auto tilesPos = GetTilePosAffectedByStrokePoint(point, app->frameArena);
    for (const auto& tilePos : tilesPos) {
        const Tile strokeTile{
            .layer = strokeLayer,
            .y = static_cast<TileAxis>(tilePos.y),
            .x = static_cast<TileAxis>(tilePos.x),
        };
        if (!tilesLoaded.contains(strokeTile)) {
            if (!CreateTile(strokeTile)) {
                SDL_assert(false && "Failed to create tile on stroke layer");
            }
        }
        strokeTileAffected.insert(strokeTile);

        const Tile underTile{
            .layer = selectedLayer,
            .y = static_cast<TileAxis>(tilePos.y),
            .x = static_cast<TileAxis>(tilePos.x),
        };
        currentTileModificationCommand->TrackTile(underTile);
    }

    strokePoints.push_back(point);
    previousPoint = point;
    currentToolState = ToolState::Started;
}

void Canvas::UpdatePaintStroke(StrokePoint point) {
    ZoneScoped;
    SDL_assert(currentTool == Tool::Paint);
    SDL_assert(currentToolState == ToolState::Started);
    SDL_assert(layers.contains(selectedLayer));
    SDL_assert(layers.contains(strokeLayer));
    SDL_assert(currentTileModificationCommand);

    const StrokePoint startPoint = previousPoint;
    const auto distance = glm::distance(startPoint.position, point.position);
    const int strokeNum = static_cast<int>(std::floor(distance / paintBrush.maxPressureSpacing));
    if (strokeNum == 0) {
        return;
    }

    float distanceCovered{paintBrush.maxPressureSpacing};
    while (distanceCovered < distance) {
        ZoneScoped;
        const float t = distanceCovered / distance;
        const StrokePoint newPoint = LerpStrokePoint(startPoint, point, t);
        const StrokePoint smoothedPoint = LerpStrokePoint(newPoint, previousPoint, paintBrush.smoothing);
        strokePoints.push_back(smoothedPoint);
        previousPoint = smoothedPoint;
        distanceCovered += paintBrush.maxPressureSpacing;

        const auto tilesPos = GetTilePosAffectedByStrokePoint(newPoint, app->frameArena);
        for (const auto& tilePos : tilesPos) {
            const Tile strokeTile{
                .layer = strokeLayer,
                .y = static_cast<TileAxis>(tilePos.y),
                .x = static_cast<TileAxis>(tilePos.x),
            };
            if (!tilesLoaded.contains(strokeTile)) {
                if (!CreateTile(strokeTile)) {
                    SDL_assert(false && "Failed to create tile on stroke layer");
                }
            }
            strokeTileAffected.insert(strokeTile);

            const Tile underTile{
                .layer = selectedLayer,
                .y = static_cast<TileAxis>(tilePos.y),
                .x = static_cast<TileAxis>(tilePos.x),
            };
            currentTileModificationCommand->TrackTile(underTile);
        }
    }
}

void Canvas::EndPaintStroke(StrokePoint point) {
    ZoneScoped;
    SDL_assert(currentTool == Tool::Paint);
    SDL_assert(currentToolState == ToolState::Started);
    SDL_assert(layers.contains(selectedLayer));
    SDL_assert(layers.contains(strokeLayer));
    SDL_assert(currentTileModificationCommand);

    currentTileModificationCommand->SaveTrackedTilesForPrevious();

    MergeLayers(strokeLayer, selectedLayer);
    DeleteLayer(strokeLayer);

    currentTileModificationCommand->SaveTrackedTilesForNew();
    canvasCommands.Push(std::move(currentTileModificationCommand.value()));
    currentTileModificationCommand = std::nullopt;

    strokeLayer = LAYER_INVALID;
    strokePoints.clear();
    currentToolState = ToolState::None;
}

void Canvas::OpenPaintBrush() {
#ifdef NDEBUG
    const std::string path = SDL_GetPrefPath(nullptr, "midori");
#else
    const std::string path = SDL_GetPrefPath(nullptr, "midori-dev");
#endif
    const std::string file = std::format("{}brushes/paintBrush.json", path);

    size_t size;
    char* buf = (char*)SDL_LoadFile(file.c_str(), &size);
    if (size == 0) {
        paintBrushModified = true;
        return;
    }

    auto brushJson = nlohmann::json::parse(buf);
    SDL_free(buf);

    paintBrush.color.r = brushJson.at("color")[0];
    paintBrush.color.g = brushJson.at("color")[1];
    paintBrush.color.b = brushJson.at("color")[2];
    paintBrush.color.a = brushJson.at("color")[3];
    paintBrush.minPressureOpacity = brushJson.at("minOpacity");
    paintBrush.maxPressureOpacity = brushJson.at("maxOpacity");
    paintBrush.minPressureFlow = brushJson.at("minFlow");
    paintBrush.maxPressureFlow = brushJson.at("maxFlow");
    paintBrush.minPressureRadius = brushJson.at("minRadius");
    paintBrush.maxPressureRadius = brushJson.at("maxRadius");
    paintBrush.minPressureHardness = brushJson.at("minHardness");
    paintBrush.maxPressureHardness = brushJson.at("maxHardness");
    paintBrush.minPressureSpacing = brushJson.at("minSpacing");
    paintBrush.maxPressureSpacing = brushJson.at("maxSpacing");
    paintBrush.smoothing = brushJson.at("smoothing");
    paintBrushModified = false;
}

void Canvas::SavePaintBrush() {
#ifdef NDEBUG
    const std::string path = SDL_GetPrefPath(nullptr, "midori");
#else
    const std::string path = SDL_GetPrefPath(nullptr, "midori-dev");
#endif
    const std::string brushPath = std::format("{}brushes", path);
    SDL_CreateDirectory(brushPath.c_str());

    nlohmann::json brushJson = {
        {"color", {paintBrush.color.r, paintBrush.color.b, paintBrush.color.g, paintBrush.color.a}},
        {"minOpacity", paintBrush.minPressureOpacity},
        {"maxOpacity", paintBrush.maxPressureOpacity},
        {"minFlow", paintBrush.minPressureFlow},
        {"maxFlow", paintBrush.maxPressureFlow},
        {"minRadius", paintBrush.minPressureRadius},
        {"maxRadius", paintBrush.maxPressureRadius},
        {"minHardness", paintBrush.minPressureHardness},
        {"maxHardness", paintBrush.maxPressureHardness},
        {"minSpacing", paintBrush.minPressureSpacing},
        {"maxSpacing", paintBrush.maxPressureSpacing},
        {"smoothing", paintBrush.smoothing},
    };
    const std::string brushJsonDump = brushJson.dump(4);

    const std::string file = std::format("{}brushes/paintBrush.json", path);
    SDL_IOStream* fileIo = SDL_IOFromFile(file.c_str(), "w");
    SDL_assert(fileIo != nullptr);
    SDL_WriteIO(fileIo, brushJsonDump.data(), brushJsonDump.size());
    SDL_CloseIO(fileIo);
    paintBrushModified = false;
}

void Canvas::OpenEraseBrush() {
#ifdef NDEBUG
    const std::string path = SDL_GetPrefPath(nullptr, "midori");
#else
    const std::string path = SDL_GetPrefPath(nullptr, "midori-dev");
#endif
    const std::string file = std::format("{}brushes/eraseBrush.json", path);

    size_t size;
    char* buf = (char*)SDL_LoadFile(file.c_str(), &size);
    if (size == 0) {
        eraseBrushModified = true;
        return;
    }

    auto brushJson = nlohmann::json::parse(buf);
    SDL_free(buf);

    eraseBrush.color.r = brushJson.at("color")[0];
    eraseBrush.color.g = brushJson.at("color")[1];
    eraseBrush.color.b = brushJson.at("color")[2];
    eraseBrush.color.a = brushJson.at("color")[3];
    eraseBrush.minPressureOpacity = brushJson.at("minOpacity");
    eraseBrush.maxPressureOpacity = brushJson.at("maxOpacity");
    eraseBrush.minPressureFlow = brushJson.at("minFlow");
    eraseBrush.maxPressureFlow = brushJson.at("maxFlow");
    eraseBrush.minPressureRadius = brushJson.at("minRadius");
    eraseBrush.maxPressureRadius = brushJson.at("maxRadius");
    eraseBrush.minPressureHardness = brushJson.at("minHardness");
    eraseBrush.maxPressureHardness = brushJson.at("maxHardness");
    eraseBrush.minPressureSpacing = brushJson.at("minSpacing");
    eraseBrush.maxPressureSpacing = brushJson.at("maxSpacing");
    eraseBrush.smoothing = brushJson.at("smoothing");
    eraseBrushModified = false;
}

void Canvas::SaveEraseBrush() {
#ifdef NDEBUG
    const std::string path = SDL_GetPrefPath(nullptr, "midori");
#else
    const std::string path = SDL_GetPrefPath(nullptr, "midori-dev");
#endif
    const std::string brushPath = std::format("{}brushes", path);
    SDL_CreateDirectory(brushPath.c_str());

    nlohmann::json brushJson = {
        {"color", {eraseBrush.color.r, eraseBrush.color.b, eraseBrush.color.g, eraseBrush.color.a}},
        {"minOpacity", eraseBrush.minPressureOpacity},
        {"maxOpacity", eraseBrush.maxPressureOpacity},
        {"minFlow", eraseBrush.minPressureFlow},
        {"maxFlow", eraseBrush.maxPressureFlow},
        {"minRadius", eraseBrush.minPressureRadius},
        {"maxRadius", eraseBrush.maxPressureRadius},
        {"minHardness", eraseBrush.minPressureHardness},
        {"maxHardness", eraseBrush.maxPressureHardness},
        {"minSpacing", eraseBrush.minPressureSpacing},
        {"maxSpacing", eraseBrush.maxPressureSpacing},
        {"smoothing", eraseBrush.smoothing},
    };
    const std::string brushJsonDump = brushJson.dump(4);

    const std::string file = std::format("{}brushes/eraseBrush.json", path);
    SDL_IOStream* fileIo = SDL_IOFromFile(file.c_str(), "w");
    SDL_assert(fileIo != nullptr);
    SDL_WriteIO(fileIo, brushJsonDump.data(), brushJsonDump.size());
    SDL_CloseIO(fileIo);
    eraseBrushModified = false;
}

// This assumes every painted tiles are not going to be culled
void Canvas::StartEraseStroke(StrokePoint point) {
    ZoneScoped;
    SDL_assert(currentTool == Tool::Erase);
    SDL_assert(currentToolState == ToolState::None);
    SDL_assert(selectedLayer != LAYER_INVALID && "No layer selected");
    SDL_assert(!currentTileModificationCommand && "TileModification already started");

    strokeArena.reset();
    std::construct_at(&strokePoints, strokeArena);
    strokeTileAffected.reserve(256);
    strokePoints.reserve(STROKE_MAX_POINTS);

    currentTileModificationCommand = TileModificationCommand(this, selectedLayer);

    const auto tilesPos = GetTilePosAffectedByStrokePoint(point, app->frameArena);

    // TODO: find a better algorithm when erasing on layers. Right now the opacity of the erase brush does not work.
    // TODO: find a way to duplicate the layer this early and instead use a temporary internal layer as the
    // modification source
    for (const auto& tilePos : tilesPos) {
        const Tile tile{
            .layer = selectedLayer,
            .y = static_cast<TileAxis>(tilePos.y),
            .x = static_cast<TileAxis>(tilePos.x),
        };
        if (tilesLoaded.contains(tile)) {
            strokeTileAffected.insert(tile);
            MarkTileDirty(tile);
            currentTileModificationCommand->TrackTile(tile);
        }
    }
    currentTileModificationCommand->SaveTrackedTilesForPrevious();

    previousPoint = point;
    currentToolState = ToolState::Started;
}

// This assumes every painted tiles are not going to be culled
void Canvas::UpdateEraseStroke(StrokePoint point) {
    ZoneScoped;
    SDL_assert(currentTool == Tool::Erase);
    SDL_assert(currentToolState == ToolState::Started);
    SDL_assert(selectedLayer != LAYER_INVALID && "No layer selected");
    SDL_assert(currentTileModificationCommand && "TileModification already started");

    const StrokePoint startPoint = previousPoint;
    const auto distance = glm::distance(startPoint.position, point.position);
    float distanceCovered{eraseBrush.maxPressureSpacing};
    while (distanceCovered < distance) {
        ZoneScoped;

        const float t = distanceCovered / distance;
        const StrokePoint newPoint = LerpStrokePoint(startPoint, point, t);
        strokePoints.push_back(newPoint);
        previousPoint = newPoint;
        distanceCovered += eraseBrush.maxPressureSpacing;

        const auto tilesPos = GetTilePosAffectedByStrokePoint(newPoint, app->frameArena);
        for (const auto& tilePos : tilesPos) {
            const Tile tile{
                .layer = selectedLayer,
                .y = static_cast<TileAxis>(tilePos.y),
                .x = static_cast<TileAxis>(tilePos.x),
            };
            if (tilesLoaded.contains(tile)) {
                strokeTileAffected.insert(tile);
                MarkTileDirty(tile);
                currentTileModificationCommand->TrackTile(tile);
            }
        }
    }
    // overwrite=false: the erase compute pass has already modified the tiles tracked in previous
    // frames, so only capture the original content of newly tracked tiles. Overwriting here would
    // clobber the pre-erase image saved in StartEraseStroke, making undo a no-op.
    currentTileModificationCommand->SaveTrackedTilesForPrevious(false);
}

// This assumes every painted tiles are not going to be culled
void Canvas::EndEraseStroke(StrokePoint point) {
    ZoneScoped;
    SDL_assert(currentTool == Tool::Erase);
    SDL_assert(currentToolState == ToolState::Started);
    SDL_assert(selectedLayer != LAYER_INVALID && "Stroke already started");
    SDL_assert(currentTileModificationCommand && "TileModification already started");

    currentTileModificationCommand->SaveTrackedTilesForNew();
    canvasCommands.Push(std::move(currentTileModificationCommand.value()));
    currentTileModificationCommand = std::nullopt;

    strokePoints.clear();
    strokeLayer = LAYER_INVALID;

    currentToolState = ToolState::None;
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

} // namespace Midori
