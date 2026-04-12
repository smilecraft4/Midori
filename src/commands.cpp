#include "commands.h"

#include "app.h"
#include "canvas.h"
#include "tiles.h"
#include <SDL3/SDL_assert.h>
#include <tracy/Tracy.hpp>

namespace Midori {

TileModificationCommand::TileModificationCommand(Canvas* canvas, Layer layer) : canvas_(canvas), layer_(layer) {
    SDL_assert(layer_ != LAYER_INVALID);
}

TileModificationCommand::~TileModificationCommand() {
    ZoneScoped;
    for (const auto& [tile, texture] : previousTileTextures_) {
        SDL_assert(texture && "Texture is invalid");
        SDL_ReleaseGPUTexture(canvas_->app->renderer.device, texture);
    }

    for (const auto& [coord, texture] : newTileTextures_) {
        SDL_assert(texture && "Texture is invalid");
        SDL_ReleaseGPUTexture(canvas_->app->renderer.device, texture);
    }
}

std::string TileModificationCommand::Name() const {
    return "Tile Modification";
}

void TileModificationCommand::Execute() {
    ZoneScoped;
    SDL_assert(canvas_->HasLayer(layer_) && "Layer not found");

    ApplyTilesTexture(newTileTextures_);
}

void TileModificationCommand::Revert() {
    ZoneScoped;
    SDL_assert(canvas_->HasLayer(layer_) && "Layer not found");
    ApplyTilesTexture(previousTileTextures_);
}

void TileModificationCommand::SavePreviousTilesTexture(const eastl::hash_set<Tile>& tiles) {
    ZoneScoped;

    const auto duplicateTileTextures = DuplicateTileTextures(tiles);

    previousTileTextures_.reserve(previousTileTextures_.bucket_count() + duplicateTileTextures.size());
    for (const auto& [tileCoord, texture] : duplicateTileTextures) {
        if(previousTileTextures_.contains(tileCoord)) {
            continue;
        }
        previousTileTextures_[tileCoord] = texture;
    }
}

void TileModificationCommand::SaveNewTilesTexture(const eastl::hash_set<Tile>& tiles) {
    ZoneScoped;

    const auto duplicateTileTextures = DuplicateTileTextures(tiles);

    newTileTextures_.reserve(newTileTextures_.bucket_count() + duplicateTileTextures.size());
    for (const auto& [tileCoord, texture] : duplicateTileTextures) {
        if(newTileTextures_.contains(tileCoord)) {
            continue;
        }
        newTileTextures_[tileCoord] = texture;
    }
}

void TileModificationCommand::ApplyTilesTexture(
    const eastl::hash_map<TileCoord, SDL_GPUTexture*>& tileCoordsTexture) const {
    if (tileCoordsTexture.empty()) {
        return;
    }

    eastl::hash_map<TileCoord, SDL_GPUTexture*> copiedTileToChange(tileCoordsTexture.size());

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(canvas_->app->renderer.device);
    SDL_assert(cmd && "Failed to create copyPass command buffer");
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);

    for (const auto& [tileCoord, texture] : tileCoordsTexture) {
        const auto duplicatedTileTexture = canvas_->app->renderer.DuplicateTileTexture(copyPass, texture);
        SDL_assert(duplicatedTileTexture && "Failed to duplicate tile texture");
        copiedTileToChange[tileCoord] = duplicatedTileTexture;
    }

    SDL_EndGPUCopyPass(copyPass);
    auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_assert(fence && "Failed to submit copyPass command buffer");

    // TODO: Think about a way to make this less blocking. Especialy when
    // spamming Undo or Redo
    SDL_WaitForGPUFences(canvas_->app->renderer.device, true, &fence, 1);
    SDL_ReleaseGPUFence(canvas_->app->renderer.device, fence);

    for (const auto& [tileCoord, texture] : copiedTileToChange) {
        SDL_assert(texture && "Texture is invalid");
        Tile tile = canvas_->GetLoadedTileAt(layer_, tileCoord.pos);
        if (tile == TILE_INVALID) {
            tile = canvas_->CreateTile(layer_, tileCoord.pos);
            SDL_assert(tile != TILE_INVALID && "Failed to create tile");

            // We don't need to upload a blank texture to this tile, this could be avoided by providing an override to
            // CreateTile with the SDL_GPUTexture* as an input, stating this is the texture you will use
            canvas_->app->renderer.tile_texture_uninitialized.erase(tile);

            // This tile was originally unloaded, so we unload it as soon as possible, the Tile is still save to use for
            // the duration of this function
            canvas_->QueueUnloadTile(layer_, tile);
        }
        SDL_assert(tile != TILE_INVALID && "Failed to create tile");

        SDL_assert(canvas_->app->renderer.tile_textures.contains(tile) && "No tile texture found");
        SDL_GPUTexture* originalTexture = canvas_->app->renderer.tile_textures.at(tile);
        SDL_ReleaseGPUTexture(canvas_->app->renderer.device, originalTexture);

        canvas_->app->renderer.tile_textures[tile] = texture;
        canvas_->layerTilesModified.at(layer_).insert(tile);
    }
}

eastl::hash_map<TileCoord, SDL_GPUTexture*>
TileModificationCommand::DuplicateTileTextures(const eastl::hash_set<Tile>& tiles) const {
    // The textures must be duplicated. The texture saved in the command don't share the same lifetime as the one
    // loaded/unloaded in runtime. The  textures here should be kept as archive versions only and never used as is to
    // avoid leaks/crash

    eastl::hash_map<TileCoord, SDL_GPUTexture*> duplicatedTilesTexture(tiles.size());

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(canvas_->app->renderer.device);
    SDL_assert(cmd && "Failed to create copyPass command buffer");
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);

    for (const auto& tile : tiles) {
        SDL_assert(tile != TILE_INVALID && "Tile is invalid");
        SDL_assert(canvas_->app->renderer.tile_textures.contains(tile) && "Tile texture not found");

        const auto tileTexture = canvas_->app->renderer.tile_textures.at(tile);
        const auto duplicatedTileTexture = canvas_->app->renderer.DuplicateTileTexture(copyPass, tileTexture);
        SDL_assert(duplicatedTileTexture && "Failed to duplicate tile texture");
        duplicatedTilesTexture[canvas_->tileInfos.at(tile)] = duplicatedTileTexture;
    }

    SDL_EndGPUCopyPass(copyPass);
    auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_assert(fence && "Failed to submit copyPass command buffer");

    // TODO: Think about a way to make this less blocking. Especialy when
    // spamming Undo or Redo
    SDL_WaitForGPUFences(canvas_->app->renderer.device, true, &fence, 1);
    SDL_ReleaseGPUFence(canvas_->app->renderer.device, fence);

    return duplicatedTilesTexture;
}

CommandHistory::CommandHistory(size_t capacity) : commands(capacity) {
}

CommandHistory::~CommandHistory() = default;

void CommandHistory::Undo() {
    if(currentPosition == 0) {
        return;
    }

    currentPosition--;
    commands[currentPosition]->Revert();
}

void CommandHistory::Redo() {
    if(currentPosition >= size) {
        return;
    }

    commands[currentPosition]->Execute();
    currentPosition++;
}

void CommandHistory::Clear() {
    currentPosition = 0;
    commands.clear();
}

void CommandHistory::Push(std::unique_ptr<ICommand> command) {
    // remove the element until the currentPosition is at the latest command
    if (currentPosition <= size) {
        for (size_t i = currentPosition; i < size; i++) {
            auto index = (begin + i) % commands.size();
            commands[index] = nullptr;
        };
        size = currentPosition + 1;
    } else {
        size++;
    }

    SDL_assert(size < commands.size());

    const auto index = (begin + currentPosition) % commands.size();
    commands[currentPosition] = std::move(command);
    currentPosition++;
    SDL_assert(index < commands.size());
    
    if(begin == begin + currentPosition) {
        begin = (begin + 1) % commands.size();
    }
}

const ICommand* CommandHistory::Get(size_t position) const {
    return commands[(begin + position) % commands.size()].get();
}

size_t CommandHistory::Empty() const {
    return size == 0;
}

size_t CommandHistory::Count() const {
    return size;
}

ViewportChangeCommand::ViewportChangeCommand(Canvas* canvas) : canvas_(canvas) {
}

ViewportChangeCommand::~ViewportChangeCommand() = default;

std::string ViewportChangeCommand::Name() const {
    return "View change";
}

void ViewportChangeCommand::Execute() {
    canvas_->viewport = newViewport_;
}

void ViewportChangeCommand::Revert() {
    canvas_->viewport = previousViewport_;
}

void ViewportChangeCommand::SetPreviousViewport(Viewport viewport) {
    previousViewport_ = viewport;
}

void ViewportChangeCommand::SetNewViewport(Viewport viewport) {
    newViewport_ = viewport;
}

} // namespace Midori
