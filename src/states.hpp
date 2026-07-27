#pragma once

#include "defines.hpp"
#include "renderer.hpp"
#include <SDL3/SDL_events.h>
#include <glm/vec2.hpp>
#include <cstdint>
#include <variant>
#include <vector>

namespace Midori {

class App;

enum class Tool : uint8_t {
    Paint,
    Erase,
    Sample,
};

enum class ToolState : uint8_t {
    None,
    Started,
};

constexpr const char* TOOL_NAME[] = {"Paint", "Erase", "Sample"};

struct IdleState {
    explicit IdleState(App* app);

    void OnEvent(const SDL_Event* event);
    void DrawUI() const;

    App* app;
};

struct NavigateState {
    enum class Mode : std::uint8_t {
        Pan,
        Rotate,
        Zoom,
    };

    explicit NavigateState(App* app);

    void OnEvent(const SDL_Event* event);
    void DrawUI() const;

    App* app;
    Mode mode;
    glm::vec2 startPos{0.0f};
    bool dragging{false};
};

struct PaintState {
    explicit PaintState(App* app);

    void OnEvent(const SDL_Event* event);
    void DrawUI() const;

    App* app;
    std::optional<Layer> strokeLayer;
    StrokePoint previousPoint{};
};

struct EraseState {
    explicit EraseState(App* app);

    void OnEvent(const SDL_Event* event);
    void DrawUI() const;

    App* app;
    StrokePoint previousPoint{};
};

/*
struct SampleState {
    explicit SampleState(App* app);

    void OnEvent(const SDL_Event* event);
    void DrawUI() const;

    App* app;
    glm::ivec2 size;
    std::vector<std::byte> texture;
};
*/

using State = std::variant<std::monostate, IdleState, NavigateState, PaintState, EraseState>;

struct StateManager {
    explicit StateManager(App* app);

    void Switch(State&& state);
    void OnEvent(const SDL_Event* event);
    void DrawUI() const;

    App* app;
    State current;
    State next;
};

} // namespace Midori
