#include "commands.h"

#include "app.h"
#include "canvas.h"
#include "tiles.h"
#include <SDL3/SDL_assert.h>
#include <tracy/Tracy.hpp>

namespace Midori {

TileModificationCommand::TileModificationCommand(Canvas* canvas, Layer layer) : canvas(canvas), layer(layer) {
    SDL_assert(layer != LAYER_INVALID);
}

TileModificationCommand::~TileModificationCommand() {
    ZoneScoped;
    for (const auto& [tile, texture] : previousTileTextures) {
        SDL_assert(texture && "Texture is invalid");
        SDL_ReleaseGPUTexture(canvas->app->renderer.device, texture);
    }

    for (const auto& [coord, texture] : newTileTextures) {
        SDL_assert(texture && "Texture is invalid");
        SDL_ReleaseGPUTexture(canvas->app->renderer.device, texture);
    }
}

std::string TileModificationCommand::Name() const {
    return "Tile Modification";
}

void TileModificationCommand::Execute() {
    ZoneScoped;
    SDL_assert(canvas->HasLayer(layer) && "Layer not found");

    ApplyTilesTexture(newTileTextures);
}

void TileModificationCommand::Revert() {
    ZoneScoped;
    SDL_assert(canvas->HasLayer(layer) && "Layer not found");
    ApplyTilesTexture(previousTileTextures);
}

void TileModificationCommand::SavePreviousTilesTexture(const std::unordered_set<Tile>& tiles) {
    ZoneScoped;

    const auto duplicateTileTextures = DuplicateTileTextures(tiles);

    previousTileTextures.reserve(previousTileTextures.bucket_count() + duplicateTileTextures.size());
    for (const auto& [tileCoord, texture] : duplicateTileTextures) {
        if (previousTileTextures.contains(tileCoord)) {
            continue;
        }
        previousTileTextures[tileCoord] = texture;
    }
}

void TileModificationCommand::SaveNewTilesTexture(const std::unordered_set<Tile>& tiles) {
    ZoneScoped;

    const auto duplicateTileTextures = DuplicateTileTextures(tiles);

    newTileTextures.reserve(newTileTextures.bucket_count() + duplicateTileTextures.size());
    for (const auto& [tileCoord, texture] : duplicateTileTextures) {
        if (newTileTextures.contains(tileCoord)) {
            continue;
        }
        newTileTextures[tileCoord] = texture;
    }
}

void TileModificationCommand::ApplyTilesTexture(
    const std::unordered_map<TileCoord, SDL_GPUTexture*>& tileCoordsTexture) const {
    if (tileCoordsTexture.empty()) {
        return;
    }

    std::unordered_map<TileCoord, SDL_GPUTexture*> copiedTileToChange(tileCoordsTexture.size());

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(canvas->app->renderer.device);
    SDL_assert(cmd && "Failed to create copyPass command buffer");
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);

    for (const auto& [tileCoord, texture] : tileCoordsTexture) {
        auto* const duplicatedTileTexture = canvas->app->renderer.DuplicateTileTexture(copyPass, texture);
        SDL_assert(duplicatedTileTexture && "Failed to duplicate tile texture");
        copiedTileToChange[tileCoord] = duplicatedTileTexture;
    }

    SDL_EndGPUCopyPass(copyPass);
    auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_assert(fence && "Failed to submit copyPass command buffer");

    // TODO: Think about a way to make this less blocking. Especialy when
    // spamming Undo or Redo
    SDL_WaitForGPUFences(canvas->app->renderer.device, true, &fence, 1);
    SDL_ReleaseGPUFence(canvas->app->renderer.device, fence);

    for (const auto& [tileCoord, texture] : copiedTileToChange) {
        SDL_assert(texture && "Texture is invalid");
        Tile tile = canvas->GetLoadedTileAt(layer, tileCoord.pos);
        if (tile == TILE_INVALID) {
            tile = canvas->CreateTile(layer, tileCoord.pos);
            SDL_assert(tile != TILE_INVALID && "Failed to create tile");

            // We don't need to upload a blank texture to this tile, this could be avoided by providing an override to
            // CreateTile with the SDL_GPUTexture* as an input, stating this is the texture you will use
            canvas->app->renderer.tileTextureUninitialized.erase(tile);

            // This tile was originally unloaded, so we unload it as soon as possible, the Tile is still save to use for
            // the duration of this function
            canvas->QueueUnloadTile(layer, tile);
        }
        SDL_assert(tile != TILE_INVALID && "Failed to create tile");

        SDL_assert(canvas->app->renderer.tileTextures.contains(tile) && "No tile texture found");
        SDL_GPUTexture* originalTexture = canvas->app->renderer.tileTextures.at(tile);
        SDL_ReleaseGPUTexture(canvas->app->renderer.device, originalTexture);

        canvas->app->renderer.tileTextures[tile] = texture;
        canvas->layerTilesModified.at(layer).insert(tile);
    }
}

std::unordered_map<TileCoord, SDL_GPUTexture*>
TileModificationCommand::DuplicateTileTextures(const std::unordered_set<Tile>& tiles) const {
    // The textures must be duplicated. The texture saved in the command don't share the same lifetime as the one
    // loaded/unloaded in runtime. The  textures here should be kept as archive versions only and never used as is to
    // avoid leaks/crash

    std::unordered_map<TileCoord, SDL_GPUTexture*> duplicatedTilesTexture(tiles.size());

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(canvas->app->renderer.device);
    SDL_assert(cmd && "Failed to create copyPass command buffer");
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);

    for (const auto& tile : tiles) {
        SDL_assert(tile != TILE_INVALID && "Tile is invalid");
        SDL_assert(canvas->app->renderer.tileTextures.contains(tile) && "Tile texture not found");

        auto* const tileTexture = canvas->app->renderer.tileTextures.at(tile);
        auto* const duplicatedTileTexture = canvas->app->renderer.DuplicateTileTexture(copyPass, tileTexture);
        SDL_assert(duplicatedTileTexture && "Failed to duplicate tile texture");
        duplicatedTilesTexture[canvas->tileInfos.at(tile)] = duplicatedTileTexture;
    }

    SDL_EndGPUCopyPass(copyPass);
    auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_assert(fence && "Failed to submit copyPass command buffer");

    // TODO: Think about a way to make this less blocking. Especialy when
    // spamming Undo or Redo
    SDL_WaitForGPUFences(canvas->app->renderer.device, true, &fence, 1);
    SDL_ReleaseGPUFence(canvas->app->renderer.device, fence);

    return duplicatedTilesTexture;
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
    commands.clear();
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
