#pragma once

#include "../defines.h"
#include <SDL3/SDL_render.h>
#include <string>
#include <EASTL/unordered_map.h>

namespace Midori {

struct Canvas;
struct Brush;

struct Command {
    enum class Type : uint8_t {
        None,
        Paint,
        Erase,
        Select,
        Transform,
    };

    explicit Command(Type type) : type_(type) {};
    virtual ~Command() = default;

    virtual std::string Name() const = 0;
    virtual void Execute() = 0;
    virtual void Revert() = 0;

    Type type_;
};

struct TileModificationCommand : public Command {
    explicit TileModificationCommand(Canvas& canvas, Layer layer, Type type);
    virtual ~TileModificationCommand();

    virtual std::string Name() const;
    virtual void Execute();
    virtual void Revert();

    void SetTilePreviousTextures(
        eastl::hash_map<TileCoord, SDL_GPUTexture*> previousTileTextures);
    void SetTileNewTextures(
        eastl::hash_map<TileCoord, SDL_GPUTexture*> newTileTextures);

    Canvas& canvas_;
    Layer layer_;
    eastl::hash_map<TileCoord, SDL_GPUTexture*> previousTileTextures_;
    eastl::hash_map<TileCoord, SDL_GPUTexture*> newTileTextures_;

   private:
    void ApplyTilesTexture(const eastl::hash_map<TileCoord, SDL_GPUTexture*>&
                               copiedTileToChange) const;
    eastl::hash_map<TileCoord, SDL_GPUTexture*> DuplicateTileTextures(
        eastl::hash_map<TileCoord, SDL_GPUTexture*> tileTextures) const;
};

}  // namespace Midori