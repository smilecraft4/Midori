#pragma once

#include "layers.h"
#include "tiles.h"
#include "viewport.h"
#include <EASTL/vector.h>
#include <EASTL/unordered_map.h>
#include <EASTL/unordered_set.h>
#include <EASTL/queue.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_assert.h>
#include <memory>
#include <string>

namespace Midori {

struct Canvas;

struct ICommand {
    ICommand() = default;
    virtual ~ICommand() = default;
    // ICommand(ICommand&&) = delete;
    // ICommand& operator=(ICommand&&) = delete;
    // ICommand(const ICommand&) = delete;
    // ICommand& operator=(const ICommand&) = delete;

    [[nodiscard]] virtual std::string Name() const = 0;
    virtual void Execute() = 0;
    virtual void Revert() = 0;

};


struct CommandHistory final {
    // CommandHistory(CommandHistory&&) = delete;
    // CommandHistory& operator=(CommandHistory&&) = delete;
    // CommandHistory(const CommandHistory&) = delete;
    // CommandHistory& operator=(const CommandHistory&) = delete;

    CommandHistory(size_t capacity);
    ~CommandHistory();
    
    void Push(std::unique_ptr<ICommand> command);

    void Undo();
    void Redo();
    void Clear();

    const ICommand* Get(size_t index) const;
    size_t Index(size_t pos) const;

    [[nodiscard]] size_t Empty() const;
    [[nodiscard]] size_t Count() const;

    eastl::vector<std::unique_ptr<ICommand>> commands;
    size_t position{0}; // starts at 1 and end at capacity, 0 means nothing
    size_t count{0}; // starts at 1 and end at capacity, 0 means no element
    size_t start{0};
};

struct TileModificationCommand final : public ICommand {
    explicit TileModificationCommand(Canvas* canvas, Layer layer);
    virtual ~TileModificationCommand();

    std::string Name() const override;
    void Execute() override;
    void Revert() override;

    void SavePreviousTilesTexture(const eastl::hash_set<Tile>& tiles);
    void SaveNewTilesTexture(const eastl::hash_set<Tile>& tiles);

    Canvas* canvas_;
    Layer layer_;
    eastl::hash_map<TileCoord, SDL_GPUTexture*> previousTileTextures_;
    eastl::hash_map<TileCoord, SDL_GPUTexture*> newTileTextures_;

private:
    void ApplyTilesTexture(const eastl::hash_map<TileCoord, SDL_GPUTexture*>& tileCoordsTexture) const;

    // TODO: Move to the renderer directly
    eastl::hash_map<TileCoord, SDL_GPUTexture*>
    DuplicateTileTextures(const eastl::hash_set<Tile>& tiles) const;
};

struct ViewportChangeCommand final : public ICommand {
    explicit ViewportChangeCommand(Canvas* canvas);
    virtual ~ViewportChangeCommand();

    std::string Name() const override;
    void Execute() override;
    void Revert() override;

    void SetPreviousViewport(Viewport viewport);
    void SetNewViewport(Viewport viewport);

    Canvas* canvas_;
    Viewport previousViewport_;
    Viewport newViewport_;
};

/*
struct LayerCreateCommand final : public ICommand {
    explicit LayerCreateCommand(Canvas& canvas, Layer layer, LayerInfo layerInfo);
    virtual ~LayerCreateCommand();

    virtual std::string Name() const;
    virtual void Execute();
    virtual void Revert();

    Canvas& canvas_;
    Layer& layer_;
    LayerInfo& layerInfo_;
};

struct LayerDeleteCommand final : public ICommand {
    explicit LayerDeleteCommand(Canvas& canvas, Layer layer, LayerInfo layerInfo);
    virtual ~LayerDeleteCommand();

    virtual std::string Name() const;
    virtual void Execute();
    virtual void Revert();

    Canvas& canvas_;
    Layer& layer_;
    LayerInfo& layerInfo_;
};

struct LayerEditCommand final : public ICommand {
    explicit LayerEditCommand(Canvas& canvas, Layer layer);
    virtual ~LayerEditCommand();

    void SetPreviousLayerInfo(LayerInfo previousInfo);
    void SetNewLayerInfo(LayerInfo newInfo);

    virtual std::string Name() const;
    virtual void Execute();
    virtual void Revert();

    Canvas& canvas_;
    Layer& layer_;
    LayerInfo previousInfo_;
    LayerInfo newInfo_;
};

*/
} // namespace Midori
