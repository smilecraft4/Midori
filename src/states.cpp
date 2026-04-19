#include "states.h"

#include "app.h"
#include <SDL3/SDL_assert.h>
#include <cstddef>
#include <memory>

namespace Midori {

// ---------------------------------------------------------------------------
// IState
// ---------------------------------------------------------------------------

IState::IState(StateManager& sm, App& app) : sm_(sm), app_(app) {}

// ---------------------------------------------------------------------------
// StateManager
// ---------------------------------------------------------------------------

StateManager::StateManager(App& app) : app_(app) {
    Push(std::make_unique<NavigateState>(*this, app_));
}

StateManager::~StateManager() {
    while (!states_.empty()) {
        Pop();
    }
}

void StateManager::Switch(std::unique_ptr<IState> state) {
    Pop();
    Push(std::move(state));
}

void StateManager::Push(std::unique_ptr<IState> state) {
    states_.push(std::move(state));
}

void StateManager::Pop() {
    SDL_assert(!states_.empty());
    states_.pop();
}

void StateManager::OnCursorMove(glm::vec2 pos)                         { if (!states_.empty()) states_.top()->OnCursorMove(pos); }
void StateManager::OnCursorPress(Uint8 button)                         { if (!states_.empty()) states_.top()->OnCursorPress(button); }
void StateManager::OnCursorRelease(Uint8 button)                       { if (!states_.empty()) states_.top()->OnCursorRelease(button); }
void StateManager::OnKeyPress(SDL_Keycode key, SDL_Keymod mods)        { if (!states_.empty()) states_.top()->OnKeyPress(key, mods); }
void StateManager::OnKeyRelease(SDL_Keycode key, SDL_Keymod mods)      { if (!states_.empty()) states_.top()->OnKeyRelease(key, mods); }
void StateManager::OnEvent()                                           { if (!states_.empty()) states_.top()->OnEvent(); }

// ---------------------------------------------------------------------------
// NavigateState  —  default state: viewport pan/rotate/zoom + tool dispatch
// ---------------------------------------------------------------------------

NavigateState::NavigateState(StateManager& sm, App& app) : IState(sm, app) {}

void NavigateState::OnCursorMove(glm::vec2 pos) {
    if (app_.changeCursorSize) {
        static glm::vec2 last = pos;
        const glm::vec2 delta = pos - last;
        last = pos;
        app_.canvas.ChangeRadiusSize(delta, app_.shift_pressed);
        return;
    }

    app_.cursor_last_pos    = app_.cursor_current_pos;
    app_.cursor_current_pos = pos;
    app_.cursor_delta_pos   = pos - app_.cursor_last_pos;

    if (!app_.cursor_left_pressed) {
        app_.canvas.viewCursorStart = pos;
    } else {
        if ((app_.canvas.viewPanning || app_.canvas.viewRotating || app_.canvas.viewZooming) &&
            glm::length(app_.cursor_delta_pos) > 0 &&
            app_.canvas.currentViewportChangeCommand == nullptr)
        {
            app_.canvas.currentViewportChangeCommand = std::make_unique<ViewportChangeCommand>(&app_.canvas);
            app_.canvas.currentViewportChangeCommand->SetPreviousViewport(app_.canvas.viewport);
        }
        app_.canvas.ViewUpdateCursor(pos);
    }

    app_.canvas.viewCursorPrevious = pos;
    app_.cursor_delta_pos = glm::vec2(0.0f);
}

void NavigateState::OnCursorPress(Uint8 button) {
    if (button != SDL_BUTTON_LEFT) {
        return;
    }

    app_.cursor_left_pressed = true;

    if (app_.canvas.viewPanning || app_.canvas.viewZooming || app_.canvas.viewRotating) {
        return;
    }
    if (app_.canvas.selectedLayer == 0) {
        return;
    }

    const auto pos = app_.canvas.viewport.ScreenToCanvas(app_.cursor_current_pos);

    if (app_.canvas.brushMode) {
        app_.canvas.StartBrushStroke(Canvas::StrokePoint{
            .color    = app_.canvas.brushOptions.color,
            .position = pos,
            .radius   = app_.canvas.brushOptions.radius,
            .flow     = app_.canvas.brushOptions.flow,
            .hardness = app_.canvas.brushOptions.hardness,
        });
        sm_.Push(std::make_unique<PaintState>(sm_, app_));
    } else if (app_.canvas.eraserMode) {
        app_.canvas.StartEraserStroke(Canvas::StrokePoint{
            .color    = {0.0f, 0.0f, 0.0f, app_.canvas.eraserOptions.opacity},
            .position = pos,
            .radius   = app_.canvas.eraserOptions.radius,
            .flow     = app_.canvas.eraserOptions.flow,
            .hardness = app_.canvas.eraserOptions.hardness,
        });
        sm_.Push(std::make_unique<EraseState>(sm_, app_));
    }
}

void NavigateState::OnCursorRelease(Uint8 button) {
    if (button == SDL_BUTTON_LEFT) {
        app_.cursor_left_pressed = false;
    }
    if (button == SDL_BUTTON_RIGHT) {
        app_.cursor_right_pressed = false;
    }
}

void NavigateState::OnKeyPress(SDL_Keycode key, SDL_Keymod mods) {
    (void)mods;

    if (key == SDLK_LSHIFT || key == SDLK_RSHIFT) { app_.shift_pressed = true; }
    if (key == SDLK_LCTRL  || key == SDLK_RCTRL)  { app_.ctrl_pressed  = true; }
    if (key == SDLK_LALT   || key == SDLK_RALT)   { app_.alt_pressed   = true; }
    if (key == SDLK_SPACE)                         { app_.space_pressed = true; }

    if (app_.alt_pressed && app_.canvas.brushMode) {
        app_.colorPicker    = true;
        app_.canvas.canvasTexture_ = app_.canvas.DownloadCanvasTexture(app_.canvas.canvasTextureSize_);
        Color color;
        auto samplePos = app_.cursor_current_pos;
        samplePos.y = static_cast<float>(app_.window_size.y) - samplePos.y;
        app_.canvas.SampleTexture(app_.canvas.canvasTexture_, app_.canvas.canvasTextureSize_, samplePos, color);
        app_.sampledColor = {
            color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f,
        };
        sm_.Push(std::make_unique<SelectState>(sm_, app_));
        app_.canvas.ViewUpdateState(app_.cursor_current_pos);
        return;
    }

    if (app_.changeCursorSize) {
        return;
    }

    if (key == SDLK_F11)  { app_.Fullscreen(!app_.fullscreen); }
    if (key == SDLK_TAB)  { app_.hide_ui = !app_.hide_ui; }

    if (key == SDLK_F && app_.space_pressed) { app_.canvas.viewport.FlipHorizontal(); }
    if (key == SDLK_G && app_.alt_pressed)   {
        app_.canvas.viewport.SetRotation(0.0f);
        app_.canvas.viewport.SetZoom(glm::vec2(0.0f), glm::vec2(1.0f));
    }

    if (key == SDLK_F && !app_.ctrl_pressed)  { app_.changeCursorSize = true; }

    if (key == SDLK_B && app_.canvas.eraserMode) {
        app_.canvas.eraserMode = false;
        app_.canvas.brushMode  = true;
    }
    if (key == SDLK_E && app_.canvas.brushMode) {
        app_.canvas.brushMode  = false;
        app_.canvas.eraserMode = true;
    }

    if (key == SDLK_Z && app_.ctrl_pressed) {
        if (app_.shift_pressed) {
            app_.canvas.canvasCommands.Redo();
        } else {
            app_.canvas.canvasCommands.Undo();
        }
    }

    if (key == SDLK_LEFT  && app_.alt_pressed) { app_.canvas.viewCommands.Undo(); }
    if (key == SDLK_RIGHT && app_.alt_pressed) { app_.canvas.viewCommands.Redo(); }

    if (key == SDLK_S && app_.ctrl_pressed && !app_.shift_pressed && !app_.alt_pressed) {
        app_.Save();
    }

    app_.canvas.ViewUpdateState(app_.cursor_current_pos);
}

void NavigateState::OnKeyRelease(SDL_Keycode key, SDL_Keymod mods) {
    (void)mods;

    if (key == SDLK_LSHIFT || key == SDLK_RSHIFT) { app_.shift_pressed = false; }
    if (key == SDLK_LCTRL  || key == SDLK_RCTRL)  { app_.ctrl_pressed  = false; }
    if (key == SDLK_LALT   || key == SDLK_RALT)   { app_.alt_pressed   = false; }
    if (key == SDLK_SPACE) {
        app_.space_pressed = false;
        if (app_.canvas.currentViewportChangeCommand != nullptr) {
            app_.canvas.currentViewportChangeCommand->SetNewViewport(app_.canvas.viewport);
            app_.canvas.viewCommands.Push(std::move(app_.canvas.currentViewportChangeCommand));
            SDL_assert(app_.canvas.currentViewportChangeCommand == nullptr);
        }
    }

    if (key == SDLK_F && app_.changeCursorSize) {
        app_.changeCursorSize = false;
        return;
    }

    app_.canvas.ViewUpdateState(app_.cursor_current_pos);
}

// ---------------------------------------------------------------------------
// PaintState  —  active during a brush stroke
// ---------------------------------------------------------------------------

PaintState::PaintState(StateManager& sm, App& app) : IState(sm, app) {}

void PaintState::OnCursorMove(glm::vec2 pos) {
    app_.cursor_last_pos    = app_.cursor_current_pos;
    app_.cursor_current_pos = pos;

    if (!app_.canvas.stroke_started) {
        return;
    }

    app_.canvas.UpdateBrushStroke(Canvas::StrokePoint{
        .color    = app_.canvas.brushOptions.color,
        .position = app_.canvas.viewport.ScreenToCanvas(pos),
        .radius   = app_.canvas.brushOptions.radius,
        .flow     = app_.canvas.brushOptions.flow,
        .hardness = app_.canvas.brushOptions.hardness,
    });
}

void PaintState::OnCursorRelease(Uint8 button) {
    if (button != SDL_BUTTON_LEFT) {
        return;
    }
    app_.cursor_left_pressed = false;

    if (app_.canvas.stroke_started) {
        app_.canvas.EndBrushStroke(Canvas::StrokePoint{
            .color    = app_.canvas.brushOptions.color,
            .position = app_.canvas.viewport.ScreenToCanvas(app_.cursor_current_pos),
            .radius   = app_.canvas.brushOptions.radius,
            .flow     = app_.canvas.brushOptions.flow,
            .hardness = app_.canvas.brushOptions.hardness,
        });
    }

    sm_.Pop();
}

void PaintState::OnKeyPress(SDL_Keycode key, SDL_Keymod mods) {
    (void)mods;
    // modifier tracking still needed during strokes
    if (key == SDLK_LSHIFT || key == SDLK_RSHIFT) { app_.shift_pressed = true; }
    if (key == SDLK_LCTRL  || key == SDLK_RCTRL)  { app_.ctrl_pressed  = true; }
    if (key == SDLK_LALT   || key == SDLK_RALT)   { app_.alt_pressed   = true; }
    if (key == SDLK_SPACE)                         { app_.space_pressed = true; }
}

// ---------------------------------------------------------------------------
// EraseState  —  active during an eraser stroke
// ---------------------------------------------------------------------------

EraseState::EraseState(StateManager& sm, App& app) : IState(sm, app) {}

void EraseState::OnCursorMove(glm::vec2 pos) {
    app_.cursor_last_pos    = app_.cursor_current_pos;
    app_.cursor_current_pos = pos;

    if (!app_.canvas.stroke_started) {
        return;
    }

    app_.canvas.UpdateEraserStroke(Canvas::StrokePoint{
        .color    = {0.0f, 0.0f, 0.0f, app_.canvas.eraserOptions.opacity},
        .position = app_.canvas.viewport.ScreenToCanvas(pos),
        .radius   = app_.canvas.eraserOptions.radius,
        .flow     = app_.canvas.eraserOptions.flow,
        .hardness = app_.canvas.eraserOptions.hardness,
    });
}

void EraseState::OnCursorRelease(Uint8 button) {
    if (button != SDL_BUTTON_LEFT) {
        return;
    }
    app_.cursor_left_pressed = false;

    if (app_.canvas.stroke_started) {
        app_.canvas.EndEraserStroke(Canvas::StrokePoint{
            .color    = {0.0f, 0.0f, 0.0f, app_.canvas.eraserOptions.opacity},
            .position = app_.canvas.viewport.ScreenToCanvas(app_.cursor_current_pos),
            .radius   = app_.canvas.eraserOptions.radius,
            .flow     = app_.canvas.eraserOptions.flow,
            .hardness = app_.canvas.eraserOptions.hardness,
        });
    }

    sm_.Pop();
}

void EraseState::OnKeyPress(SDL_Keycode key, SDL_Keymod mods) {
    (void)mods;
    if (key == SDLK_LSHIFT || key == SDLK_RSHIFT) { app_.shift_pressed = true; }
    if (key == SDLK_LCTRL  || key == SDLK_RCTRL)  { app_.ctrl_pressed  = true; }
    if (key == SDLK_LALT   || key == SDLK_RALT)   { app_.alt_pressed   = true; }
    if (key == SDLK_SPACE)                         { app_.space_pressed = true; }
}

// ---------------------------------------------------------------------------
// SelectState  —  color picker (alt held while in brush mode)
// ---------------------------------------------------------------------------

SelectState::SelectState(StateManager& sm, App& app) : IState(sm, app) {}

void SelectState::OnCursorMove(glm::vec2 pos) {
    app_.cursor_current_pos = pos;

    Color color;
    auto samplePos = pos;
    samplePos.y = static_cast<float>(app_.window_size.y) - samplePos.y;
    app_.canvas.SampleTexture(app_.canvas.canvasTexture_, app_.canvas.canvasTextureSize_, samplePos, color);
    app_.sampledColor = {
        color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f,
    };
}

void SelectState::OnCursorPress(Uint8 button) {
    if (button != SDL_BUTTON_LEFT) {
        return;
    }
    app_.colorPicker = false;
    app_.canvas.brushOptions.color = app_.sampledColor;
    app_.canvas.canvasTexture_.clear();
    sm_.Pop();
}

void SelectState::OnKeyRelease(SDL_Keycode key, SDL_Keymod mods) {
    (void)mods;
    if (key == SDLK_LALT || key == SDLK_RALT) {
        app_.alt_pressed = false;
        app_.colorPicker = false;
        app_.canvas.canvasTexture_.clear();
        sm_.Pop();
    }
}

void SelectState::OnEvent() {
    // pop if alt somehow released without an event
    if (!app_.alt_pressed) {
        app_.colorPicker = false;
        app_.canvas.canvasTexture_.clear();
        sm_.Pop();
    }
}

// ---------------------------------------------------------------------------
// TransformState  —  placeholder, requires active selection
// ---------------------------------------------------------------------------

TransformState::TransformState(StateManager& sm, App& app) : IState(sm, app) {}

void TransformState::OnEvent() {}

} // namespace Midori
