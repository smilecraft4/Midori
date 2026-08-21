#pragma once

#include "brushes.hpp"
#include "canvas.hpp"
#include "defines.hpp"
#include "renderer.hpp"
#include "states.hpp"
#include "tiles.hpp"
#include "window.hpp"
#include "base.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <condition_variable>
#include <cstdint>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <imgui.h>
#include <optional>
#include <string>
#include <vector>

namespace Midori {

struct App;

struct WorkerCommand {
    enum class Type : uint8_t {
        Read,
        Write,
    } type;
    CanvasFileID canvasId;
    bool staged;
    Tile tile;
    TileTransfer::ID id;
};

struct WriteCommand {
    enum class Type : uint8_t {
        StagePut,
        StageDel,
        Commit, // this is a big operation
        Put,
        Del,
    };

    Type type;
    CanvasFileID canvas;
    TileTransfer::ID id;
    MDB_dbi dbi;
    uint8_t keySize;
    std::byte key[16];
    size_t dataSize;
    void* data;
};

struct App {
    App(const App&) = delete;
    App(App&&) = delete;
    App& operator=(const App&) = delete;
    App& operator=(App&&) = delete;

    App(int argc, char* argv[]);
    ~App() = default;

    bool OnEvent(const SDL_Event* event);

    // TODO consildate the quit sequence
    void ShouldQuit();
    bool CanQuit() const;
    void Save();
    void Fullscreen(bool enable);

    void Quit();
    bool Init();
    void Update();

    bool Resize(int width, int height);
    void CursorMove(glm::vec2 newPos);
    void CursorPress(Uint8 button);
    void CursorRelease(Uint8 button);
    void KeyPress(SDL_Keycode key, SDL_Keymod mods);
    void KeyRelease(SDL_Keycode key, SDL_Keymod mods);

    void OpenBrushes();
    void SaveBrushes();

    void DrawUI();
    void DebugTileCulling(glm::vec2 viewportSize, ImDrawList* drawList);
    void DrawTileDebug(glm::ivec2 pos, ImDrawList* drawList, ImU32 col, const std::string& label = "") const;

    Error NewTileCommand(Tile tile, bool first = false, bool last = false);
    Error LoadTileCommand(Tile tile, bool first = false, bool last = false);
    Error MergeTileCommand(Tile srcTile, Tile dstTile, Renderer::MergeInfo info, bool first = false,
                           bool last = false);
    Error DuplicateTileCommand(Tile srcTile, Tile dstTile, bool first = false, bool last = false);
    Error PaintTileCommand(Tile tile, bool first = false, bool last = false);
    Error EraseTileCommand(Tile tile, bool first = false, bool last = false);
    Error RenderTileCommand(Tile tile, bool first = false, bool last = false);
    Error StageTileCommand(Tile tile, bool first = false, bool last = false);
    Error DeleteTileCommand(Tile tile, bool first = false, bool last = false);
    Error DestroyTileCommand(Tile tile, bool first = false, bool last = false);

    arena<> globalArena{1_MiB};
    arena<> toolArena{2_MiB};
    arena<> frameArena{1_MiB};

    uint32_t frame;

    slot_map<size_t, CanvasFile> canvasFiles{CANVASFILE_MAX};
    slot_map<size_t, CanvasView> canvasViews{CANVASVIEW_MAX};
    std::optional<size_t> currentCanvasView;

    uint32_t pendingTileEdits{};
    std::vector<Tile> strokeTiles;
    std::vector<StrokePoint> strokePoints;

    std::mutex tileCommandsMutex;
    std::atomic<uint32_t> tileCommandCounter;
    std::deque<TileCommand> tileCommandsQueue;
    std::vector<TileCommand> pendingTileCommands;
    TileCommand::Type pendingTileCommandCurrentType;
    slot_map<TileTransfer::ID, TileTransfer> tileTransfers{2048};

    std::mutex workerMutex;
    std::condition_variable workerCv;
    std::queue<WorkerCommand> workerQueue;
    std::vector<std::thread> workerThreads;

    std::deque<WriteCommand> writeQueue;
    std::condition_variable writeCv;
    std::mutex writeMutex;
    std::thread writeThread;

    BrushSettings paintBrush;
    BrushSettings eraseBrush;

    Tool currentTool{Tool::Paint};
    ToolState currentToolState{ToolState::None};

    SDL_Window* window = nullptr;
    std::vector<std::string> args;

    std::atomic<bool> running{false};
    bool saving = false;
    bool shouldQuit = false;
    bool hidden = false;
    bool fullscreen = false;
    bool uiFocus = false;
    bool hideUi = false;
    bool changeCursorSize = false;

    bool colorPicker = false;
    glm::vec4 sampledColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);

    StateManager stateManager;
    Renderer renderer;

    glm::vec4 bgColor = {1.0F, 1.0F, 1.0F, 1.0F};
    glm::ivec2 windowPos = {SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED};
    glm::ivec2 windowSize = {1280, 720};

    glm::vec2 cursorLastPos = glm::vec2(0.0f);
    glm::vec2 cursorCurrentPos = glm::vec2(0.0f);
    glm::vec2 cursorDeltaPos = glm::vec2(0.0f);
    bool cursorLeftPressed = false;
    bool cursorRightPressed = false;

    bool spacePressed = false;
    bool shiftPressed = false;
    bool ctrlPressed = false;
    bool altPressed = false;
    bool penInRange = false;
    float penPressure = 1.0f;
};
} // namespace Midori
