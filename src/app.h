#pragma once

#include "canvas.h"
#include "renderer.h"
#include "states.h"
#include "base.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <imgui.h>
#include <string>

namespace Midori {

class App {
public:
    App(const App&) = delete;
    App(App&&) = delete;
    App& operator=(const App&) = delete;
    App& operator=(App&&) = delete;

    App(int argc, char* argv[]);
    ~App() = default;

    bool OnEvent(const SDL_Event* event);

    // TODO consildate the quit sequence
    void ShouldQuit();
    bool CanQuit();
    void Save();
    void Fullscreen(bool enable);

    // Event responses
    void Quit();
    bool Init();

    /**
     * @brief Sync multithreaded operations, Render needed window, Fetch semaphores and fences from the gpu, etc...
     *
     * @return true
     * @return false
     */
    void Update();

    bool Resize(int width, int height);
    void CursorMove(glm::vec2 newPos);
    void CursorPress(Uint8 button);
    void CursorRelease(Uint8 button);
    void KeyPress(SDL_Keycode key, SDL_Keymod mods);
    void KeyRelease(SDL_Keycode key, SDL_Keymod mods);

    // TODO: this should not be there but somewhere else
    void DebugTileCulling(glm::vec2 viewportSize, ImDrawList* drawList);
    void DrawTileDebug(glm::ivec2 pos, ImDrawList* drawList, ImU32 col, const std::string& label = "");

    SDL_Window* window = nullptr;
    std::vector<std::string> args;

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
    Canvas canvas;

    glm::vec4 bgColor = {1.0F, 1.0F, 1.0F, 1.0F};
    glm::ivec2 windowPos = {SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED};
    glm::ivec2 windowSize = {1280, 720};

    /* should this be in canvas space ?? */
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

    Arena<> frameArena{8_MiB};
};
} // namespace Midori
