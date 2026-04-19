#pragma once

#include <EASTL/stack.h>
#include <SDL3/SDL_keycode.h>
#include <glm/vec2.hpp>
#include <memory>

namespace Midori {

struct App;
struct StateManager;

struct IState {
    explicit IState(StateManager& sm, App& app);
    virtual ~IState() = default;

    virtual void OnCursorMove(glm::vec2 pos) {}
    virtual void OnCursorPress(Uint8 button) {}
    virtual void OnCursorRelease(Uint8 button) {}
    virtual void OnKeyPress(SDL_Keycode key, SDL_Keymod mods) {}
    virtual void OnKeyRelease(SDL_Keycode key, SDL_Keymod mods) {}
    virtual void OnEvent() {}

protected:
    StateManager& sm_;
    App& app_;
};

struct StateManager {
    explicit StateManager(App& app);
    ~StateManager();

    void Switch(std::unique_ptr<IState> state);
    void Push(std::unique_ptr<IState> state);
    void Pop();

    void OnCursorMove(glm::vec2 pos);
    void OnCursorPress(Uint8 button);
    void OnCursorRelease(Uint8 button);
    void OnKeyPress(SDL_Keycode key, SDL_Keymod mods);
    void OnKeyRelease(SDL_Keycode key, SDL_Keymod mods);
    void OnEvent();

private:
    App& app_;
    eastl::stack<std::unique_ptr<IState>> states_;
};

struct NavigateState : IState {
    explicit NavigateState(StateManager& sm, App& app);

    void OnCursorMove(glm::vec2 pos) override;
    void OnCursorPress(Uint8 button) override;
    void OnCursorRelease(Uint8 button) override;
    void OnKeyPress(SDL_Keycode key, SDL_Keymod mods) override;
    void OnKeyRelease(SDL_Keycode key, SDL_Keymod mods) override;

    bool panning = false;
    bool rotating = false;
    bool zooming = false;
};

struct PaintState : IState {
    explicit PaintState(StateManager& sm, App& app);

    void OnCursorMove(glm::vec2 pos) override;
    void OnCursorRelease(Uint8 button) override;
    void OnKeyPress(SDL_Keycode key, SDL_Keymod mods) override;
};

struct EraseState : IState {
    explicit EraseState(StateManager& sm, App& app);

    void OnCursorMove(glm::vec2 pos) override;
    void OnCursorRelease(Uint8 button) override;
    void OnKeyPress(SDL_Keycode key, SDL_Keymod mods) override;
};

struct SelectState : IState {
    explicit SelectState(StateManager& sm, App& app);

    void OnCursorMove(glm::vec2 pos) override;
    void OnCursorPress(Uint8 button) override;
    void OnKeyRelease(SDL_Keycode key, SDL_Keymod mods) override;
    void OnEvent() override;
};

struct TransformState : IState {
    explicit TransformState(StateManager& sm, App& app);

    void OnEvent() override;
};

} // namespace Midori
