#include "commands.hpp"

#include "app.hpp"
#include "canvas.hpp"
#include "tiles.hpp"
#include <SDL3/SDL_assert.h>
#include <tracy/Tracy.hpp>
#include <variant>

namespace Midori {

TileModificationCommand::TileModificationCommand(CanvasView* canvasView, Layer layer)
    : app(canvasView->app), canvasFileId(canvasView->canvasFileId), layer(layer) {
    SDL_assert(layer != LAYER_INVALID && "TileModificationCommand needs a real layer");
}

TileModificationCommand::~TileModificationCommand() {
    ZoneScoped;
    for (auto [tile, textures] : tileTextures) {
        SDL_ReleaseGPUTexture(app->renderer.device, textures.revert);
        SDL_ReleaseGPUTexture(app->renderer.device, textures.execute);
    }
}

std::string TileModificationCommand::Name() {
    return "Tile Modification";
}

void TileModificationCommand::Execute() {
    ZoneScoped;
    auto& canvasFile = app->canvasFiles.at(canvasFileId);
    SDL_assert(canvasFile.layers.contains(layer) && "Command targets a layer that no longer exists");

    const bool copyPassBegan = app->renderer.BeginCopyPass();
    SDL_assert(copyPassBegan && "Failed to begin the gpu copy pass");
    for (auto [tile, textures] : tileTextures) {
        if (!textures.execute) {
            canvasFile.DestroyTile(tile);
            canvasFile.DeleteTile(tile);
        } else {
            if (!canvasFile.tilesLoaded.contains(tile)) {
                const Error err = app->renderer.CreateUninitializedTileTexture(tile);
                SDL_assert(err == Error::None && "Failed to create the tile gpu texture");
                canvasFile.tilesLoaded.insert(tile);
            }

            SDL_assert(canvasFile.tilesLoaded.contains(tile) && "Tile is not loaded");
            SDL_assert(app->renderer.tileTextures.contains(tile) && "Loaded tile has no gpu texture");
            SDL_GPUTexture* originalTexture = app->renderer.tileTextures.at(tile);
            SDL_ReleaseGPUTexture(app->renderer.device, originalTexture);
            app->renderer.tileTextures.at(tile) = app->renderer.DuplicateTileTexture(textures.execute);
            canvasFile.MarkTileDirty(tile);
        }
    }
    const bool copyPassEnded = app->renderer.EndCopyPass(true);
    SDL_assert(copyPassEnded && "Failed to submit the gpu copy pass");
}

void TileModificationCommand::Revert() {
    ZoneScoped;
    auto& canvasFile = app->canvasFiles.at(canvasFileId);
    SDL_assert(canvasFile.layers.contains(layer) && "Command targets a layer that no longer exists");

    const bool copyPassBegan = app->renderer.BeginCopyPass();
    SDL_assert(copyPassBegan && "Failed to begin the gpu copy pass");
    for (auto [tile, textures] : tileTextures) {
        if (!textures.revert) {
            canvasFile.DestroyTile(tile);
            canvasFile.DeleteTile(tile);
        } else {
            if (!canvasFile.tilesLoaded.contains(tile)) {
                const Error err = app->renderer.CreateUninitializedTileTexture(tile);
                SDL_assert(err == Error::None && "Failed to create the tile gpu texture");
                canvasFile.tilesLoaded.insert(tile);
            }

            SDL_assert(canvasFile.tilesLoaded.contains(tile) && "Tile is not loaded");
            SDL_assert(app->renderer.tileTextures.contains(tile) && "Loaded tile has no gpu texture");
            SDL_GPUTexture* originalTexture = app->renderer.tileTextures.at(tile);
            SDL_ReleaseGPUTexture(app->renderer.device, originalTexture);
            app->renderer.tileTextures.at(tile) = app->renderer.DuplicateTileTexture(textures.revert);
            canvasFile.MarkTileDirty(tile);
        }
    }
    const bool copyPassEnded = app->renderer.EndCopyPass(true);
    SDL_assert(copyPassEnded && "Failed to submit the gpu copy pass");
}

Error TileModificationCommand::TrackTile(Tile tile) {
    ZoneScoped;

    // Already tracked: the same tile is touched by most points in a stroke.
    if (tileTextures.contains(tile)) {
        return Error::None;
    }

    // insert() throws when the map is full, and a stroke can cover more tiles than
    // the command has room for, so this has to be checked rather than asserted.
    if (tileTextures.full()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "Cannot track tile(%u, %d, %d): command is at its %zu tile limit",
                    tile.layer, tile.x, tile.y, tileTextures.capacity());
        return Error::OutOfCapacity;
    }

    tileTextures.insert(tile, {.revert = nullptr, .execute = nullptr});
    return Error::None;
}

void TileModificationCommand::SaveTrackedTilesForPrevious(bool overwrite) {
    ZoneScoped;

    const bool copyPassBegan = app->renderer.BeginCopyPass();
    SDL_assert(copyPassBegan && "Failed to begin the gpu copy pass");
    for (auto [tile, textures] : tileTextures) {
        if (!overwrite && textures.revert) {
            continue;
        }
        if (app->renderer.tileTextures.contains(tile)) {
            if (textures.revert) {
                SDL_ReleaseGPUTexture(app->renderer.device, textures.revert);
            }
            textures.revert = app->renderer.DuplicateTileTexture(tile);
        }
    }
    const bool copyPassEnded = app->renderer.EndCopyPass(true);
    SDL_assert(copyPassEnded && "Failed to submit the gpu copy pass");
}

void TileModificationCommand::SaveTrackedTilesForNew(bool overwrite) {
    ZoneScoped;
    const bool copyPassBegan = app->renderer.BeginCopyPass();
    SDL_assert(copyPassBegan && "Failed to begin the gpu copy pass");
    for (auto [tile, textures] : tileTextures) {
        if (!overwrite && textures.execute) {
            continue;
        }
        if (app->renderer.tileTextures.contains(tile)) {
            if (textures.execute) {
                SDL_ReleaseGPUTexture(app->renderer.device, textures.execute);
            }
            textures.execute = app->renderer.DuplicateTileTexture(tile);
        }
    }
    const bool copyPassEnded = app->renderer.EndCopyPass(true);
    SDL_assert(copyPassEnded && "Failed to submit the gpu copy pass");
}

CommandHistory::CommandHistory(size_t capacity) : ring(capacity) {}

void CommandHistory::Undo() {
    if (position == 0) {
        return;
    }

    std::visit(
        [](auto&& command) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(command)>, std::monostate>) {
                command.Revert();
            }
        },
        ring.at(position - 1));
    position--;
}

void CommandHistory::Redo() {
    if (position == ring.size()) {
        return;
    }
    SDL_assert(position < ring.size());
    position++;

    std::visit(
        [](auto&& command) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(command)>, std::monostate>) {
                command.Execute();
            }
        },
        ring.at(position - 1));
}

void CommandHistory::Clear() {
    position = 0;
    ring.clear();
}

void CommandHistory::Push(Command&& command) {
    SDL_assert(ring.size() <= position && "Untracked position in ring_arry");
    if (ring.size() > position) {
        while (ring.size() > position) {
            ring.pop_front();
        }
    } else if (ring.full()) {
        ring.pop_front();
        position--;
    }
    ring.push_back(std::move(command));
    position++;
}

const Command* CommandHistory::Get(size_t pos) const {
    SDL_assert(pos < ring.size());
    return &ring.at(pos - 1);
}

size_t CommandHistory::Empty() const {
    return ring.empty();
}

size_t CommandHistory::Count() const {
    return ring.size();
}

void CommandHistory::DrawUI() {
    if (Empty()) {
        ImGui::TextColored(ImColor(0, 0, 128), "Nothing");

    } else {
        if (ImGui::Button("Clear History")) {
            Clear();
        }
        for (int pos = static_cast<int>(Count()) - 1; pos >= 0; pos--) {
            std::visit(
                [this, pos](auto&& command) {
                    if constexpr (!std::is_same_v<std::decay_t<decltype(command)>, std::monostate>) {
                        const auto name = command.Name();
                        if ((pos + 1) < position) {
                            ImGui::TextColored(ImColor(0, 0, 0), "[%d]: %s", pos, name.c_str());
                        } else if ((pos + 1) == position) {
                            ImGui::TextColored(ImColor(0, 128, 0), "[%d]: %s", pos, name.c_str());
                        } else if ((pos + 1) > position) {
                            ImGui::TextColored(ImColor(0, 0, 128), "[%d]: %s", pos, name.c_str());
                        }
                    }
                },
                ring.at(pos + 1));
        }
    }
}

ViewportChangeCommand::ViewportChangeCommand(App* app, size_t canvasViewId) : app(app), canvasViewId(canvasViewId) {}

std::string ViewportChangeCommand::Name() {
    return "View change";
}

void ViewportChangeCommand::Execute() const {
    auto& canvasView = app->canvasViews.at(canvasViewId);
    canvasView.viewport.SetRotation(newViewport.rot);
    canvasView.viewport.SetTranslation(newViewport.trans);
    canvasView.viewport.SetZoom(newViewport.scale);
}

void ViewportChangeCommand::Revert() const {
    auto& canvasView = app->canvasViews.at(canvasViewId);
    canvasView.viewport.SetRotation(previousViewport.rot);
    canvasView.viewport.SetTranslation(previousViewport.trans);
    canvasView.viewport.SetZoom(previousViewport.scale);
}

void ViewportChangeCommand::SetPreviousViewport(Viewport viewport) {
    previousViewport.rot = viewport.rotation;
    previousViewport.trans = viewport.translation;
    previousViewport.scale = viewport.zoom;
}

void ViewportChangeCommand::SetNewViewport(Viewport viewport) {
    newViewport.rot = viewport.rotation;
    newViewport.trans = viewport.translation;
    newViewport.scale = viewport.zoom;
}

} // namespace Midori
