#pragma once

#include <EASTL/stack.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <glm/vec2.hpp>
#include <memory>
#include <string>

namespace Midori {

struct App;
struct StateManager;

enum class State : std::uint8_t {
    App,
    Navigate,
    Paint,
    Erase,
    Select,
    Transform,
};

struct IState {
    explicit IState(App* app);
    virtual ~IState() = default;

    virtual bool OnEvent(const SDL_Event* event) = 0;
    virtual std::string Name() const = 0;
    virtual void DrawUI() const = 0;

    App* app_;
};

struct StateManager {
    explicit StateManager(App* app);
    ~StateManager();

    void Switch(std::unique_ptr<IState> state);
    void Push(std::unique_ptr<IState> state);
    void Pop();

    void OnEvent(const SDL_Event* event);

    App* app_;
    eastl::vector<std::unique_ptr<IState>> states_;
};

struct AppState : IState {
    explicit AppState(App* app);
    ~AppState();

    bool OnEvent(const SDL_Event* event) override;
    std::string Name() const override;
    void DrawUI() const override;
};

struct NavigateState : IState {
    explicit NavigateState(App* app);
    ~NavigateState();

    bool OnEvent(const SDL_Event* event) override;
    std::string Name() const override;
    void DrawUI() const override;

    enum class Mode : std::uint8_t {
        Pan,
        Rotate,
        Zoom,
    } mode{Mode::Pan};

    glm::vec2 startPos{0.0f};
    glm::vec2 previousPos{0.0f};
    bool clicking{false};
    bool space{false};
    bool ctrl{false};
    bool shift{false};
};

} // namespace Midori
