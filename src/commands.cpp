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
    SDL_assert(layer != LAYER_INVALID);
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
    SDL_assert(canvasFile.layers.contains(layer) && "Layer not found");

    const bool copyPassBegan = app->renderer.BeginCopyPass();
    SDL_assert(copyPassBegan);
    for (auto [tile, textures] : tileTextures) {
        if (!textures.execute) {
            canvasFile.DestroyTile(tile);
            canvasFile.DeleteTile(tile);
        } else {
            if (!canvasFile.tilesLoaded.contains(tile)) {
                const Error err = app->renderer.CreateUninitializedTileTexture(tile);
                SDL_assert(err == Error::None);
                canvasFile.tilesLoaded.insert(tile);
            }

            SDL_assert(canvasFile.tilesLoaded.contains(tile) && "tile not found");
            SDL_assert(app->renderer.tileTextures.contains(tile) && "No tile texture found");
            SDL_GPUTexture* originalTexture = app->renderer.tileTextures.at(tile);
            SDL_ReleaseGPUTexture(app->renderer.device, originalTexture);
            app->renderer.tileTextures.at(tile) = app->renderer.DuplicateTileTexture(textures.execute);
            canvasFile.MarkTileDirty(tile);
        }
    }
    const bool copyPassEnded = app->renderer.EndCopyPass(true);
    SDL_assert(copyPassEnded);
}

void TileModificationCommand::Revert() {
    ZoneScoped;
    auto& canvasFile = app->canvasFiles.at(canvasFileId);
    SDL_assert(canvasFile.layers.contains(layer) && "Layer not found");

    const bool copyPassBegan = app->renderer.BeginCopyPass();
    SDL_assert(copyPassBegan);
    for (auto [tile, textures] : tileTextures) {
        if (!textures.revert) {
            canvasFile.DestroyTile(tile);
            canvasFile.DeleteTile(tile);
        } else {
            if (!canvasFile.tilesLoaded.contains(tile)) {
                const Error err = app->renderer.CreateUninitializedTileTexture(tile);
                SDL_assert(err == Error::None);
                canvasFile.tilesLoaded.insert(tile);
            }

            SDL_assert(canvasFile.tilesLoaded.contains(tile) && "tile not found");
            SDL_assert(app->renderer.tileTextures.contains(tile) && "No tile texture found");
            SDL_GPUTexture* originalTexture = app->renderer.tileTextures.at(tile);
            SDL_ReleaseGPUTexture(app->renderer.device, originalTexture);
            app->renderer.tileTextures.at(tile) = app->renderer.DuplicateTileTexture(textures.revert);
            canvasFile.MarkTileDirty(tile);
        }
    }
    const bool copyPassEnded = app->renderer.EndCopyPass(true);
    SDL_assert(copyPassEnded);
}

void TileModificationCommand::TrackTile(Tile tile) {
    ZoneScoped;
    tileTextures.insert(tile, {.revert = nullptr, .execute = nullptr});
}

void TileModificationCommand::SaveTrackedTilesForPrevious(bool overwrite) {
    ZoneScoped;

    const bool copyPassBegan = app->renderer.BeginCopyPass();
    SDL_assert(copyPassBegan);
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
    SDL_assert(copyPassEnded);
}

void TileModificationCommand::SaveTrackedTilesForNew(bool overwrite) {
    ZoneScoped;
    const bool copyPassBegan = app->renderer.BeginCopyPass();
    SDL_assert(copyPassBegan);
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
    SDL_assert(copyPassEnded);
}

CommandHistory::CommandHistory(size_t capacity) : commands(capacity) {}

CommandHistory::~CommandHistory() = default;

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
        commands[Index(position)]);
    position--;
}

void CommandHistory::Redo() {
    if (position == count) {
        return;
    }
    SDL_assert(position < count);
    position++;

    std::visit(
        [](auto&& command) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(command)>, std::monostate>) {
                command.Execute();
            }
        },
        commands[Index(position)]);
}

void CommandHistory::Clear() {
    position = 0;
    count = 0;
    start = 0;
    commands.clear(); // we just want to call the destructors
    commands.resize(commands.capacity());
}

size_t CommandHistory::Index(size_t pos) const {
    SDL_assert(pos > 0);
    SDL_assert(pos <= count);
    return (start + (pos - 1)) % commands.capacity();
}

void CommandHistory::Push(Command&& command) {
    if (position < count) {
        for (int pos = static_cast<int>(count) - 1; pos > position; pos--) {
            commands[Index(position)] = std::monostate();
        }
    }

    if (position < commands.capacity()) {
        position++;
        count = position;
    } else {
        start = (start + 1) % commands.size();
    }

    commands[Index(position)] = std::move(command);
}

const Command* CommandHistory::Get(size_t index) const {
    SDL_assert(index < count);
    return &commands[Index(index + 1)];
}

size_t CommandHistory::Empty() const {
    return count == 0;
}

size_t CommandHistory::Count() const {
    return count;
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
                commands[Index(pos + 1)]);
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
