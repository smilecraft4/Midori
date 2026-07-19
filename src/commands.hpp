#pragma once

#include "base.hpp"
#include "tiles.hpp"
#include "viewport.hpp"

#include <SDL3/SDL_gpu.h>
#include <memory>
#include <string>
#include <variant>

namespace Midori {

struct Canvas;

struct TileModificationCommand final {
    struct Textures {
        SDL_GPUTexture* revert{};
        SDL_GPUTexture* execute{};
    };

    explicit TileModificationCommand(Canvas* canvas, Layer layer);
    ~TileModificationCommand();

    TileModificationCommand(const TileModificationCommand&) = delete;
    TileModificationCommand& operator=(const TileModificationCommand&) = delete;
    TileModificationCommand(TileModificationCommand&&) noexcept = default;
    TileModificationCommand& operator=(TileModificationCommand&&) noexcept = default;

    static std::string Name();
    void Execute();
    void Revert();

    void TrackTile(Tile tile);
    void SaveTrackedTilesForPrevious(bool overwrite = true);
    void SaveTrackedTilesForNew(bool overwrite = true);

    Canvas* canvas;
    Layer layer;
    hash_map<Tile, Textures> tileTextures{256};
};

struct ViewportChangeCommand final {
    explicit ViewportChangeCommand(Canvas* canvas);
    ~ViewportChangeCommand();

    ViewportChangeCommand(const ViewportChangeCommand&) = delete;
    ViewportChangeCommand& operator=(const ViewportChangeCommand&) = delete;
    ViewportChangeCommand(ViewportChangeCommand&&) noexcept = default;
    ViewportChangeCommand& operator=(ViewportChangeCommand&&) noexcept = default;

    static std::string Name();
    void Execute();
    void Revert();

    void SetPreviousViewport(Viewport viewport);
    void SetNewViewport(Viewport viewport);

    Canvas* canvas;
    Viewport previousViewport;
    Viewport newViewport;
};

using Command = std::variant<std::monostate, TileModificationCommand, ViewportChangeCommand>;

struct CommandHistory final {
    CommandHistory(size_t capacity);
    ~CommandHistory();

    void Push(Command&& command);

    void Undo();
    void Redo();
    void Clear();
    void DrawUI();

    [[nodiscard]] const Command* Get(size_t index) const;
    [[nodiscard]] size_t Index(size_t pos) const;
    [[nodiscard]] size_t Empty() const;
    [[nodiscard]] size_t Count() const;

    std::vector<Command> commands;
    size_t position{0};
    size_t count{0};
    size_t start{0};
};
} // namespace Midori
