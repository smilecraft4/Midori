#pragma once

#include <SDL3/SDL_events.h>
#include <glm/vec2.hpp>
#include <cstdint>
#include <memory>

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

struct IState {
    explicit IState(App* app);
    virtual ~IState() = default;

    virtual void OnEvent(const SDL_Event* event) = 0;
    virtual void DrawUI() const = 0;

    App* app;
};

struct StateManager {
    explicit StateManager(App* app);

    void Switch(std::unique_ptr<IState> state);
    void OnEvent(const SDL_Event* event);

    App* app;
    std::unique_ptr<IState> current;
    std::unique_ptr<IState> next;
};

// Shortcuts active, waiting for input. Pointer down starts a stroke, space enters navigation.
struct IdleState : IState {
    explicit IdleState(App* app);

    void OnEvent(const SDL_Event* event) override;
    void DrawUI() const override;
};

// Space held: drag to pan, ctrl-drag to zoom, shift-drag to rotate, R resets the view.
struct NavigateState : IState {
    explicit NavigateState(App* app);

    void OnEvent(const SDL_Event* event) override;
    void DrawUI() const override;

    enum class Mode : std::uint8_t {
        Pan,
        Rotate,
        Zoom,
    } mode{Mode::Pan};

    glm::vec2 startPos{0.0f};
    bool dragging{false};
};

struct PaintState : IState {
    PaintState(App* app);

    void OnEvent(const SDL_Event* event) override;
    void DrawUI() const override;
};

struct EraseState : IState {
    EraseState(App* app);

    void OnEvent(const SDL_Event* event) override;
    void DrawUI() const override;
};

struct SampleState : IState {
    SampleState(App* app);

    void OnEvent(const SDL_Event* event) override;
    void DrawUI() const override;
};

} // namespace Midori
