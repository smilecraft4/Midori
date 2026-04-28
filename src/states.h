#pragma once

#include <EASTL/stack.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_events.h>
#include <glm/vec2.hpp>
#include <memory>

namespace Midori {

struct App;
struct StateManager;

struct IState {
    explicit IState(StateManager& sm);
    virtual ~IState() = default;

    virtual void OnEvent(const SDL_Event* event);

protected:
    StateManager& sm_;
};

struct StateManager {
    explicit StateManager(App& app);
    ~StateManager();

    void Switch(std::unique_ptr<IState> state);
    void Push(std::unique_ptr<IState> state);
    void Pop();

    void OnEvent(const SDL_Event* event);

private:
    App& app_;
    eastl::vector<std::unique_ptr<IState>> states_;
};

struct NavigateState : IState {
    explicit NavigateState(StateManager& sm, App& app);

    void OnEvent(const SDL_Event* event) override;

    bool panning = false;
    bool rotating = false;
    bool zooming = false;
};

struct PaintState : IState {
    explicit PaintState(StateManager& sm, App& app);

    void OnEvent(const SDL_Event* event) override;
};

struct EraseState : IState {
    explicit EraseState(StateManager& sm, App& app);

    void OnEvent(const SDL_Event* event) override;
};

struct SelectState : IState {
    explicit SelectState(StateManager& sm, App& app);

    void OnEvent(const SDL_Event* event) override;
};

struct TransformState : IState {
    explicit TransformState(StateManager& sm, App& app);

    void OnEvent(const SDL_Event* event) override;
};

} // namespace Midori
