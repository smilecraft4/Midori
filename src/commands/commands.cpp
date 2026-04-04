#include "commands.h"

#include <SDL3/SDL_assert.h>
#include <tracy/Tracy.hpp>
#include "../app.h"
#include "../canvas.h"

namespace Midori {

TileModificationCommand::TileModificationCommand(Canvas& canvas, Layer layer, Type type)
    : canvas_(canvas), Command(type) {}

TileModificationCommand::~TileModificationCommand() {
    ZoneScoped;
    for (const auto& [coord, texture] : previousTileTextures_) {
        SDL_assert(layer_ == coord.layer && "The tile is from another layer");
        SDL_assert(texture && "Texture is invalid");

        const auto tile = canvas_.GetTileAt(coord.layer, coord.pos);
        if (tile != TILE_INVALID) {
            SDL_assert(canvas_.app->renderer.tile_textures.at(tile) == texture &&
                       "Texture is used by the runtime layer");
        }

        SDL_ReleaseGPUTexture(canvas_.app->renderer.device, texture);
    }

    for (const auto& [coord, texture] : newTileTextures_) {
        SDL_assert(layer_ == coord.layer && "The tile is from another layer");
        SDL_assert(texture && "Texture is invalid");

        const auto tile = canvas_.GetTileAt(coord.layer, coord.pos);
        if (tile != TILE_INVALID) {
            SDL_assert(canvas_.app->renderer.tile_textures.at(tile) == texture &&
                       "Texture is used by the runtime layer");
        }

        SDL_ReleaseGPUTexture(canvas_.app->renderer.device, texture);
    }
}

std::string TileModificationCommand::Name() const {
    switch (type_) {
        case Type::Paint:
            return "Paint";
        case Type::Erase:
            return "Erase";
        default:
            return "Tile edit";
    }
}

void TileModificationCommand::Execute() {
    ZoneScoped;
    SDL_assert(previousTileTextures_.size() == newTileTextures_.size() && "Missing textures states");
    SDL_assert(canvas_.HasLayer(layer_) && "Layer not found");

    ApplyTilesTexture(newTileTextures_);
}

void TileModificationCommand::Revert() {
    ZoneScoped;
    SDL_assert(previousTileTextures_.size() == newTileTextures_.size() && "Missing textures states");

    ApplyTilesTexture(newTileTextures_);
}

void TileModificationCommand::SetTilePreviousTextures(
    eastl::hash_map<TileCoord, SDL_GPUTexture*> previousTileTextures) {
    ZoneScoped;

    // Hopefully this is removed by the optimizer in release build
    for (const auto& [coord, texture] : previousTileTextures) {
        SDL_assert(coord.layer == layer_ && "Tile is on another layer");
        SDL_assert(texture && "Texture is invalid");
    }

    previousTileTextures_ = std::move(DuplicateTileTextures(previousTileTextures));
}

void TileModificationCommand::SetTileNewTextures(eastl::hash_map<TileCoord, SDL_GPUTexture*> newTileTextures) {
    ZoneScoped;

    // Hopefully this is removed by the optimizer in release build
    for (const auto& [coord, texture] : newTileTextures) {
        SDL_assert(coord.layer == layer_ && "Tile is on another layer");
        SDL_assert(texture && "Texture is invalid");
    }

    newTileTextures_ = std::move(DuplicateTileTextures(newTileTextures));
}

void TileModificationCommand::ApplyTilesTexture(
    const eastl::hash_map<TileCoord, SDL_GPUTexture*>& tileToChange) const {
    if (tileToChange.empty()) {
        return;
    }

    auto copiedTileToChange = DuplicateTileTextures(tileToChange);

    for (const auto& [coord, texture] : copiedTileToChange) {
        SDL_assert(layer_ == coord.layer && "The tile is from another layer");
        SDL_assert(texture && "Texture is invalid");
        Tile tile = canvas_.GetTileAt(coord.layer, coord.pos);
        if (tile == TILE_INVALID) {
            tile = canvas_.CreateTile(coord.layer, coord.pos).value();
            SDL_assert(tile != TILE_INVALID && "Failed to create tile");
            canvas_.UnloadTile(coord.layer, tile);
        }

        SDL_assert(canvas_.app->renderer.tile_textures.contains(tile) && "No tile texture found");

        SDL_GPUTexture* originalTexture = canvas_.app->renderer.tile_textures.at(tile);
        SDL_ReleaseGPUTexture(canvas_.app->renderer.device, originalTexture);
        canvas_.app->renderer.tile_textures[tile] = texture;
        canvas_.layerTilesModified.at(coord.layer).insert(tile);
    }
}

eastl::hash_map<TileCoord, SDL_GPUTexture*> TileModificationCommand::DuplicateTileTextures(
    eastl::hash_map<TileCoord, SDL_GPUTexture*> tileTextures) const {
    // The textures must be duplicated. The texture saved in the command don't share the same lifetime as the one
    // loaded/unloaded in runtime. The  textures here should be kept as archive versions only and never used as is to
    // avoid leaks/crash

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(canvas_.app->renderer.device);
    SDL_assert(cmd && "Failed to create copyPass command buffer");
    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);
    for (auto& [coord, texture] : tileTextures) {
        const auto oldTexture = texture;
        texture = canvas_.app->renderer.DuplicateTileTexture(copyPass, oldTexture);
        SDL_assert(texture && "Failed to duplicate tile texture");
    }
    SDL_EndGPUCopyPass(copyPass);
    auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    SDL_assert(fence && "Failed to submit copyPass command buffer");

    // TODO: Think about a way to make this less blocking. Especialy when
    // spamming Undo or Redo
    SDL_WaitForGPUFences(canvas_.app->renderer.device, true, &fence, 1);
    SDL_ReleaseGPUFence(canvas_.app->renderer.device, fence);

    return tileTextures;
}

}  // namespace Midori