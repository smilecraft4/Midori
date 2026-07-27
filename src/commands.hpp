#pragma once

#include "base.hpp"
#include "errors.hpp"
#include "tiles.hpp"
#include "viewport.hpp"

#include <SDL3/SDL_gpu.h>
#include <string>
#include <variant>

namespace Midori {

struct App;
struct CanvasView;

struct TileModificationCommand final {
    struct Textures {
        SDL_GPUTexture* revert{};
        SDL_GPUTexture* execute{};
    };

    explicit TileModificationCommand(CanvasView* canvasView, Layer layer);
    ~TileModificationCommand();

    TileModificationCommand(const TileModificationCommand&) = delete;
    TileModificationCommand& operator=(const TileModificationCommand&) = delete;
    TileModificationCommand(TileModificationCommand&&) noexcept = default;
    TileModificationCommand& operator=(TileModificationCommand&&) noexcept = default;

    static std::string Name();
    void Execute();
    void Revert();

    Error TrackTile(Tile tile);
    void SaveTrackedTilesForPrevious(bool overwrite = true);
    void SaveTrackedTilesForNew(bool overwrite = true);

    App* app;
    size_t canvasFileId;
    Layer layer;
    hash_map<Tile, Textures> tileTextures{256};
};

struct ViewportChangeCommand final {
    struct ViewportPos {
        glm::vec2 trans{0.0f};
        glm::vec2 scale{1.0f};
        float rot{0.0f};

        bool operator==(const ViewportPos& other) const noexcept {
            return trans == other.trans && scale == other.trans && rot == other.rot;
        }
        bool operator!=(const ViewportPos& other) const noexcept { return !(*this == other); }
    };

    explicit ViewportChangeCommand(App* app, size_t canvasViewId);

    ViewportChangeCommand(const ViewportChangeCommand&) = delete;
    ViewportChangeCommand& operator=(const ViewportChangeCommand&) = delete;
    ViewportChangeCommand(ViewportChangeCommand&&) noexcept = default;
    ViewportChangeCommand& operator=(ViewportChangeCommand&&) noexcept = default;

    static std::string Name();
    void Execute() const;
    void Revert() const;

    void SetPreviousViewport(Viewport viewport);
    void SetNewViewport(Viewport viewport);

    App* app;
    size_t canvasViewId;
    ViewportPos previousViewport;
    ViewportPos newViewport;
};

using Command = std::variant<std::monostate, TileModificationCommand, ViewportChangeCommand>;

struct CommandHistory final {
    CommandHistory(size_t capacity);

    void Push(Command&& command);

    void Undo();
    void Redo();
    void Clear();
    void DrawUI();

    [[nodiscard]] const Command* Get(size_t index) const;
    [[nodiscard]] bool Empty() const;
    [[nodiscard]] size_t Count() const;

    ring_array<Command> ring;
    size_t position{0};
};
} // namespace Midori
