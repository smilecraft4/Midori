#pragma once

#include "defines.hpp"

#include <SDL3/SDL_video.h>
#include <array>
// #include <memory>
#include <optional>
#include <string>

namespace Midori {

struct App;

struct Window {
    struct CreateInfo {
        std::string name;
        std::optional<int> x;
        std::optional<int> y;
        std::optional<int> width;
        std::optional<int> height;
    };

    Window(const Window&) = delete;
    Window(Window&&) noexcept = delete;
    Window& operator=(const Window&) = delete;
    Window& operator=(Window&&) noexcept = delete;

    explicit Window(App* app, const CreateInfo& createInfo);
    ~Window();

    // void Init();
    // void Update();
    // void Render();
    // void Close();

    // void OnResize(int newWidth, int newHeight);
    // void OnMove(int newX, int newY);
    // void OnFocus();

    // void Focus();
    // void Hide();
    // void Fullscreen();
    // void Maximize();
    // void Minimize();

    App* app;
    SDL_Window* window{};
    int x{};
    int y{};
    int width{};
    int height{};
    std::string name;
    std::array<std::optional<size_t>, CANVASVIEW_MAX> canvasViewIds{};

    // struct Impl {};
    // std::unique_ptr<Impl> impl;
};

} // namespace Midori
