#include "commands.hpp"

#include "app.hpp"
#include "canvas.hpp"
#include "tiles.hpp"
#include <SDL3/SDL_assert.h>
#include <tracy/Tracy.hpp>

namespace Midori {

TileModificationCommand::TileModificationCommand(Canvas* canvas, Layer layer) : canvas(canvas), layer(layer) {
    SDL_assert(layer != LAYER_INVALID);
}

TileModificationCommand::~TileModificationCommand() {
    ZoneScoped;
    for (auto [tile, textures] : tileTextures) {
        SDL_ReleaseGPUTexture(canvas->app->renderer.device, textures.revert);
        SDL_ReleaseGPUTexture(canvas->app->renderer.device, textures.execute);
    }
}

std::string TileModificationCommand::Name() const {
    return "Tile Modification";
}

void TileModificationCommand::Execute() {
    ZoneScoped;
    SDL_assert(canvas->layers.contains(layer) && "Layer not found");

    SDL_assert(canvas->app->renderer.BeginCopyPass());
    for (auto [tile, textures] : tileTextures) {
        SDL_assert(textures.execute && "Texture is invalid");
        if (!canvas->tilesLoaded.contains(tile)) {
            if (!canvas->CreateTile(tile, false)) {
                SDL_assert(false && "Failed to create tile");
            }
        }

        SDL_assert(canvas->tilesLoaded.contains(tile) && "tile not found");
        SDL_assert(canvas->app->renderer.tileTextures.contains(tile) && "No tile texture found");
        SDL_GPUTexture* originalTexture = canvas->app->renderer.tileTextures.at(tile);
        SDL_ReleaseGPUTexture(canvas->app->renderer.device, originalTexture);

        canvas->app->renderer.tileTextures.at(tile) = canvas->app->renderer.DuplicateTileTexture(textures.execute);
        canvas->tilesModified.insert(tile);
    }
    SDL_assert(canvas->app->renderer.EndCopyPass(true));
}

void TileModificationCommand::Revert() {
    ZoneScoped;
    SDL_assert(canvas->layers.contains(layer) && "Layer not found");

    // We really should duplicate the texture before applying it

    SDL_assert(canvas->app->renderer.BeginCopyPass());
    for (auto [tile, textures] : tileTextures) {
        SDL_assert(textures.revert && "Texture is invalid");
        if (!canvas->tilesLoaded.contains(tile)) {
            if (!canvas->CreateTile(tile, false)) {
                SDL_assert(false && "Failed to create tile");
            }
        }

        SDL_assert(canvas->tilesLoaded.contains(tile) && "tile not found");
        SDL_assert(canvas->app->renderer.tileTextures.contains(tile) && "No tile texture found");
        SDL_GPUTexture* originalTexture = canvas->app->renderer.tileTextures.at(tile);
        SDL_ReleaseGPUTexture(canvas->app->renderer.device, originalTexture);

        canvas->app->renderer.tileTextures.at(tile) = canvas->app->renderer.DuplicateTileTexture(textures.revert);
        canvas->tilesModified.insert(tile);
    }
    SDL_assert(canvas->app->renderer.EndCopyPass(true));
}

void TileModificationCommand::TrackTile(TileCoord tile) {
    ZoneScoped;
    tileTextures.insert(tile, {.revert = nullptr, .execute = nullptr});
}

void TileModificationCommand::SaveTrackedTilesForPrevious(bool overwrite) {
    ZoneScoped;

    SDL_assert(canvas->app->renderer.BeginCopyPass());
    for (auto [tile, textures] : tileTextures) {
        if (!overwrite && textures.revert) {
            continue;
        }
        if (canvas->app->renderer.tileTextures.contains(tile)) {
            textures.revert = canvas->app->renderer.DuplicateTileTexture(tile);
        }
    }
    SDL_assert(canvas->app->renderer.EndCopyPass(true));
}

void TileModificationCommand::SaveTrackedTilesForNew(bool overwrite) {
    ZoneScoped;
    SDL_assert(canvas->app->renderer.BeginCopyPass());
    for (auto [tile, textures] : tileTextures) {
        if (!overwrite && textures.execute) {
            continue;
        }
        if (canvas->app->renderer.tileTextures.contains(tile)) {
            textures.execute = canvas->app->renderer.DuplicateTileTexture(tile);
        }
    }
    SDL_assert(canvas->app->renderer.EndCopyPass(true));
}

CommandHistory::CommandHistory(size_t capacity) : commands(capacity) {}

CommandHistory::~CommandHistory() = default;

void CommandHistory::Undo() {
    if (position == 0) {
        return;
    }
    commands[Index(position)]->Revert();
    position--;
}

void CommandHistory::Redo() {
    if (position == count) {
        return;
    }
    SDL_assert(position < count);
    position++;
    commands[Index(position)]->Execute();
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

void CommandHistory::Push(std::unique_ptr<ICommand> command) {
    if (position < count) {
        for (int pos = static_cast<int>(count) - 1; pos > position; pos--) {
            commands[Index(pos)].reset();
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

const ICommand* CommandHistory::Get(size_t index) const {
    SDL_assert(index < count);
    return commands[Index(index + 1)].get();
}

size_t CommandHistory::Empty() const {
    return count == 0;
}

size_t CommandHistory::Count() const {
    return count;
}

ViewportChangeCommand::ViewportChangeCommand(Canvas* canvas) : canvas(canvas) {}

ViewportChangeCommand::~ViewportChangeCommand() = default;

std::string ViewportChangeCommand::Name() const {
    return "View change";
}

void ViewportChangeCommand::Execute() {
    canvas->viewport = newViewport;
}

void ViewportChangeCommand::Revert() {
    canvas->viewport = previousViewport;
}

void ViewportChangeCommand::SetPreviousViewport(Viewport viewport) {
    previousViewport = viewport;
}

void ViewportChangeCommand::SetNewViewport(Viewport viewport) {
    newViewport = viewport;
}

} // namespace Midori
