#pragma once

#include "base.hpp"
#include "tiles.hpp"
#include "viewport.hpp"

#include <SDL3/SDL_gpu.h>
#include <memory>
#include <string>

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
    struct Textures {
        SDL_GPUTexture* revert{};
        SDL_GPUTexture* execute{};
    };

    explicit TileModificationCommand(Canvas* canvas, Layer layer);
    ~TileModificationCommand() override;

    [[nodiscard]] std::string Name() const override;
    void Execute() override;
    void Revert() override;

    void TrackTile(TileCoord tile);
    void SaveTrackedTilesForPrevious(bool overwrite = true);
    void SaveTrackedTilesForNew(bool overwrite = true);

    Canvas* canvas;
    Layer layer;
    hash_map<TileCoord, Textures> tileTextures{256};
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
