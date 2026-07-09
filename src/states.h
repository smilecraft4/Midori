#pragma once

#include <SDL3/SDL_events.h>
#include <glm/vec2.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace Midori {

class App;

struct IState {
    explicit IState(App* app);
    virtual ~IState() = default;

    [[nodiscard]] virtual std::string Name() const = 0;
    virtual void OnEvent(const SDL_Event* event) = 0;
    virtual void DrawUI() const = 0;

    // When true, pointer events reach the state even while the cursor hovers a UI window.
    // A stroke must keep updating and must end on release, wherever the cursor is.
    [[nodiscard]] virtual bool CapturesPointer() const { return false; }

    App* app;
};

struct StateManager {
    explicit StateManager(App* app);

    // The new state becomes active after the current OnEvent returns,
    // so a state can call Switch from inside its own OnEvent.
    void Switch(std::unique_ptr<IState> state);

    void OnEvent(const SDL_Event* event);

    App* app;
    std::unique_ptr<IState> current;
    std::unique_ptr<IState> next;
};

// Shortcuts active, waiting for input. Pointer down starts a stroke, space enters navigation.
struct IdleState : IState {
    explicit IdleState(App* app);

    [[nodiscard]] std::string Name() const override;
    void OnEvent(const SDL_Event* event) override;
    void DrawUI() const override;
};

// Space held: drag to pan, ctrl-drag to zoom, shift-drag to rotate, R resets the view.
struct NavigateState : IState {
    explicit NavigateState(App* app);

    [[nodiscard]] std::string Name() const override;
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

// Brush or eraser stroke in progress; entered on pointer down, left on pointer up.
struct StrokeState : IState {
    StrokeState(App* app, bool erasing);

    [[nodiscard]] std::string Name() const override;
    void OnEvent(const SDL_Event* event) override;
    void DrawUI() const override;
    [[nodiscard]] bool CapturesPointer() const override { return true; }

    bool erasing{false};
};

} // namespace Midori
