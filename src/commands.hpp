#pragma once

#include "layers.h"
#include "tiles.h"
#include "viewport.h"
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_assert.h>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>

namespace Midori {

struct Canvas;

struct ICommand {
    ICommand() = default;
    virtual ~ICommand() = default;

    [[nodiscard]] virtual std::string Name() const = 0;
    virtual void Execute() = 0;
    virtual void Revert() = 0;
};

struct CommandHistory final {
    CommandHistory(size_t capacity);
    ~CommandHistory();

    void Push(std::unique_ptr<ICommand> command);

    void Undo();
    void Redo();
    void Clear();

    [[nodiscard]] const ICommand* Get(size_t index) const;
    [[nodiscard]] size_t Index(size_t pos) const;
    [[nodiscard]] size_t Empty() const;
    [[nodiscard]] size_t Count() const;

    std::vector<std::unique_ptr<ICommand>> commands;
    // TODO extract into a RingArray<T>
    size_t position{0};
    size_t count{0};
    size_t start{0};
};

struct TileModificationCommand final : public ICommand {
    explicit TileModificationCommand(Canvas* canvas, Layer layer);
    ~TileModificationCommand() override;

    [[nodiscard]] std::string Name() const override;
    void Execute() override;
    void Revert() override;

    void SavePreviousTilesTexture(std::span<Tile> tiles);
    void SaveNewTilesTexture(std::span<Tile> tiles);

    Canvas* canvas;
    Layer layer;
    std::unordered_map<TileCoord, SDL_GPUTexture*> previousTileTextures;
    std::unordered_map<TileCoord, SDL_GPUTexture*> newTileTextures;

private:
    void ApplyTilesTexture(const std::unordered_map<TileCoord, SDL_GPUTexture*>& tileCoordsTexture) const;

    // TODO: Move to the renderer directly
    [[nodiscard]] std::vector<SDL_GPUTexture*> DuplicateTileTextures(std::span<Tile> tiles) const;
};

struct ViewportChangeCommand final : public ICommand {
    explicit ViewportChangeCommand(Canvas* canvas);
    ~ViewportChangeCommand() override;

    [[nodiscard]] std::string Name() const override;
    void Execute() override;
    void Revert() override;

    void SetPreviousViewport(Viewport viewport);
    void SetNewViewport(Viewport viewport);

    Canvas* canvas;
    Viewport previousViewport;
    Viewport newViewport;
};
} // namespace Midori
