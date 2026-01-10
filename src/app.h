#pragma once

#include <random>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include "canvas.h"
#include "renderer.h"

namespace Midori {

class App {
   public:
    App(const App &) = delete;
    App(App &&) = delete;
    App &operator=(const App &) = delete;
    App &operator=(App &&) = delete;

    App(int argc, char *argv[]);
    ~App() = default;

    bool Init();
    bool Update();
    bool Resize(int width, int height);
    void ShouldQuit();
    bool CanQuit();
    void Quit();

    void Fullscreen(bool enable);

    void CursorMove(glm::vec2 new_pos);
    void CursorPress(Uint8 button);
    void CursorRelease(Uint8 button);
    void KeyPress(SDL_Keycode key, SDL_Keymod mods);
    void KeyRelease(SDL_Keycode key, SDL_Keymod mods);

    SDL_Window *window = nullptr;
    std::vector<std::string> args;

    bool should_quit = false;
    bool hidden = false;
    bool fullscreen = false;
    bool ui_focus = false;
    bool hide_ui = false;
    bool changeCursorSize = false;

    bool colorPicker = false;
    glm::vec4 sampledColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);

    Canvas canvas;
    Renderer renderer;

    glm::vec4 bg_color = {1.0F, 1.0F, 1.0F, 1.0F};
    glm::ivec2 window_pos = {SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED};
    glm::ivec2 window_size = {1280, 720};

    /* should this be in canvas space ?? */
    glm::vec2 cursor_last_pos = glm::vec2(0.0f);
    glm::vec2 cursor_current_pos = glm::vec2(0.0f);
    glm::vec2 cursor_delta_pos = glm::vec2(0.0f);
    bool cursor_left_pressed = false;
    bool cursor_right_pressed = false;

    bool space_pressed = false;
    bool shift_pressed = false;
    bool ctrl_pressed = false;
    bool alt_pressed = false;
    bool pen_in_range = false;
    float pen_pressure = 1.0f;
};
}  // namespace Midori
