#include "states.hpp"

#include "SDL3/SDL_events.h"
#include "app.hpp"
#include "brushes.hpp"
#include "commands.hpp"
#include "defines.hpp"
#include "renderer.hpp"

#include <backends/imgui_impl_sdl3.h>
#include <imgui.h>
#include <memory>
#include <optional>
#include <tracy/Tracy.hpp>
#include <utility>
#include <variant>

namespace Midori {

namespace {

// Collapse left/right variants so a shortcut works with either key.
SDL_Keymod NormalizeMods(const SDL_Keymod mod) {
    SDL_Keymod out = SDL_KMOD_NONE;
    if ((mod & SDL_KMOD_CTRL) != 0u) {
        out |= SDL_KMOD_CTRL;
    }
    if ((mod & SDL_KMOD_SHIFT) != 0u) {
        out |= SDL_KMOD_SHIFT;
    }
    if ((mod & SDL_KMOD_ALT) != 0u) {
        out |= SDL_KMOD_ALT;
    }
    if ((mod & SDL_KMOD_GUI) != 0u) {
        out |= SDL_KMOD_GUI;
    }
    return out;
}

bool IsKeyPressed(const SDL_Event* event, const SDL_Keycode key, const SDL_Keymod mods) {
    return event->type == SDL_EVENT_KEY_DOWN && event->key.key == key && NormalizeMods(event->key.mod) == mods;
}

// While a pen is in range SDL synthesizes mouse events for it; use only the pen events then.
bool IsPointerDown(const App* app, const SDL_Event* event) {
    if (event->type == SDL_EVENT_PEN_DOWN) {
        return true;
    }
    return !app->penInRange && event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT;
}

bool IsPointerUp(const App* app, const SDL_Event* event) {
    if (event->type == SDL_EVENT_PEN_UP) {
        return true;
    }
    return !app->penInRange && event->type == SDL_EVENT_MOUSE_BUTTON_UP && event->button.button == SDL_BUTTON_LEFT;
}

bool IsPointerMove(const App* app, const SDL_Event* event) {
    if (event->type == SDL_EVENT_PEN_MOTION) {
        return true;
    }
    return !app->penInRange && event->type == SDL_EVENT_MOUSE_MOTION;
}
} // namespace

StateManager::StateManager(App* app) : app(app), current(IdleState(app)) {}

void StateManager::Switch(State&& state) {
    next = std::move(state);
}

void StateManager::OnEvent(const SDL_Event* event) {
    ZoneScoped;
    ImGui_ImplSDL3_ProcessEvent(event);

    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || io.WantCaptureMouse) {
        return;
    }

    if (event->type == SDL_EVENT_PEN_PROXIMITY_IN) {
        app->penInRange = true;
        app->penPressure = 0.0f;
    }
    if (event->type == SDL_EVENT_PEN_PROXIMITY_OUT) {
        app->penInRange = false;
        app->penPressure = 1.0f;
    }
    if (event->type == SDL_EVENT_PEN_AXIS && event->paxis.axis == SDL_PEN_AXIS_PRESSURE) {
        app->penPressure = event->paxis.value;
    }

    if (event->type == SDL_EVENT_MOUSE_MOTION && !app->penInRange) {
        app->cursorLastPos = app->cursorCurrentPos;
        app->cursorCurrentPos = glm::vec2(event->motion.x, event->motion.y);
        app->cursorDeltaPos = app->cursorCurrentPos - app->cursorLastPos;
    }
    if (event->type == SDL_EVENT_PEN_MOTION) {
        app->cursorLastPos = app->cursorCurrentPos;
        app->cursorCurrentPos = glm::vec2(event->pmotion.x, event->pmotion.y);
        app->cursorDeltaPos = app->cursorCurrentPos - app->cursorLastPos;
    }

    if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
        const bool down = event->type == SDL_EVENT_KEY_DOWN;
        switch (event->key.key) {
            case SDLK_LSHIFT:
            case SDLK_RSHIFT:
                app->shiftPressed = down;
                break;
            case SDLK_LCTRL:
            case SDLK_RCTRL:
                app->ctrlPressed = down;
                break;
            case SDLK_LALT:
            case SDLK_RALT:
                app->altPressed = down;
                break;
            case SDLK_SPACE:
                app->spacePressed = down;
                break;
            default:
                break;
        }
    }

    if (app->shouldQuit) {
        return;
    }

    if ((event->type == SDL_EVENT_QUIT)
        || (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED
            && event->window.windowID == SDL_GetWindowID(app->window))) {
        SDL_HideWindow(app->window);
        app->hidden = true;
        app->ShouldQuit();
        return;
    }
    if (event->type == SDL_EVENT_WINDOW_RESIZED) {
        app->Resize(event->window.data1, event->window.data2);
        return;
    }

    std::visit(
        [event](auto&& state) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(state)>, std::monostate>) {
                state.OnEvent(event);
            }
        },
        current);

    if (!std::holds_alternative<std::monostate>(next)) {
        current = std::move(next);
        next = std::monostate();
    }
}

void StateManager::DrawUI() const {
    std::visit(
        [](auto&& state) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(state)>, std::monostate>) {
                state.DrawUI();
            }
        },
        current);
}

// ---------------------------------------------------------------------------
// IdleState

IdleState::IdleState(App* app) : app(app) {}

void IdleState::OnEvent(const SDL_Event* event) {
    ZoneScoped;

    if (app->currentCanvasView.has_value()) {
        auto& canvasView = app->canvasViews.at(app->currentCanvasView.value());
        auto& canvasFile = app->canvasFiles.at(canvasView.canvasFileId);

        if (IsPointerDown(app, event) && canvasView.selectedLayer.has_value()) {
            switch (app->currentTool) {
                case Tool::Paint: {
                    app->stateManager.Switch(PaintState(app));
                } break;
                case Tool::Erase: {
                    app->stateManager.Switch(EraseState(app));
                } break;
                default:
                    SDL_assert(false && "Uknown tool");
                    break;
            }

            return;
        }

        if (event->key.key == SDLK_SPACE && !event->key.repeat) {
            app->stateManager.Switch(NavigateState(app));
            return;
        }
        if (IsKeyPressed(event, SDLK_S, SDL_KMOD_CTRL)) {
            canvasFile.Save();
            return;
        }
        if (IsKeyPressed(event, SDLK_TAB, SDL_KMOD_NONE)) {
            app->hideUi = !app->hideUi;
            return;
        }
        if (IsKeyPressed(event, SDLK_Z, SDL_KMOD_CTRL)) {
            canvasFile.canvasHistory.Undo();
            return;
        }
        if (IsKeyPressed(event, SDLK_Z, SDL_KMOD_CTRL | SDL_KMOD_SHIFT)) {
            canvasFile.canvasHistory.Redo();
            return;
        }
        if (IsKeyPressed(event, SDLK_LEFT, SDL_KMOD_ALT)) {
            canvasView.viewHistory.Undo();
            return;
        }
        if (IsKeyPressed(event, SDLK_RIGHT, SDL_KMOD_ALT)) {
            canvasView.viewHistory.Redo();
            return;
        }
        if (IsKeyPressed(event, SDLK_B, SDL_KMOD_NONE)) {
            app->currentTool = Tool::Paint;
            return;
        }
        if (IsKeyPressed(event, SDLK_E, SDL_KMOD_NONE)) {
            app->currentTool = Tool::Erase; // TODO swith to the app level
            return;
        }
    }

    if (IsKeyPressed(event, SDLK_F11, SDL_KMOD_NONE)) {
        app->Fullscreen(!app->fullscreen);
        return;
    }
}

void IdleState::DrawUI() const {
    const auto& canvasView = app->canvasViews.at(app->currentCanvasView.value());
    ImGui::Text("IdleState");
    ImGui::Text("Tool: %s", TOOL_NAME[std::to_underlying(app->currentTool)]);
}

// ---------------------------------------------------------------------------
// NavigateState

NavigateState::NavigateState(App* app) : app(app) {
    ZoneScoped;
    SDL_assert(app->currentCanvasView.has_value());

    auto& canvasView = app->canvasViews.at(app->currentCanvasView.value());
    SDL_assert(!canvasView.currentViewportCommand.has_value());

    canvasView.currentViewportCommand = ViewportChangeCommand(app, app->currentCanvasView.value());
    canvasView.currentViewportCommand->SetPreviousViewport(canvasView.viewport);
}

void NavigateState::OnEvent(const SDL_Event* event) {
    ZoneScoped;
    SDL_assert(app->currentCanvasView.has_value());
    auto& canvasView = app->canvasViews.at(app->currentCanvasView.value());

    if (event->type == SDL_EVENT_KEY_UP && event->key.key == SDLK_SPACE) {
        canvasView.currentViewportCommand->SetNewViewport(canvasView.viewport);
        if (canvasView.currentViewportCommand.value().previousViewport
            != canvasView.currentViewportCommand.value().newViewport) {
            canvasView.viewHistory.Push(std::move(canvasView.currentViewportCommand.value()));
        }
        canvasView.currentViewportCommand = std::nullopt;
        app->stateManager.Switch(IdleState(app));
        return;
    }

    if (IsPointerDown(app, event)) {
        startPos = app->cursorCurrentPos;
        dragging = true;
        return;
    }
    if (IsPointerUp(app, event)) {
        dragging = false;
        return;
    }

    if (IsKeyPressed(event, SDLK_R, SDL_KMOD_NONE)) {
        canvasView.viewport.SetRotation(0.0f);
        canvasView.viewport.SetZoom(glm::vec2(1.0f));
        return;
    }

    if (app->ctrlPressed && !app->shiftPressed) {
        mode = Mode::Zoom;
    } else if (app->shiftPressed && !app->ctrlPressed) {
        mode = Mode::Rotate;
    } else {
        mode = Mode::Pan;
    }

    if (IsPointerMove(app, event) && dragging) {
        const glm::vec2 center = glm::vec2(app->windowSize) / 2.0f;
        switch (mode) {
            case Mode::Pan:
                canvasView.viewport.Translate(app->cursorDeltaPos);
                break;
            case Mode::Zoom: {
                canvasView.viewport.Zoom(startPos - center, glm::vec2(app->cursorDeltaPos.x / 256.0f));
                break;
            }
            case Mode::Rotate: {
                const auto previousAngle = std::atan2(app->cursorLastPos.x - center.x, app->cursorLastPos.y - center.y);
                const auto currentAngle =
                    std::atan2(app->cursorCurrentPos.x - center.x, app->cursorCurrentPos.y - center.y);
                canvasView.viewport.Rotate(previousAngle - currentAngle);
                break;
            }
        }
    }
}

void NavigateState::DrawUI() const {
    ImGui::Text("NavigateState");
    ImGui::Text("Dragging: %s", dragging ? "true" : "false");
    switch (mode) {
        case Mode::Pan:
            ImGui::Text("Mode: Pan");
            break;
        case Mode::Rotate:
            ImGui::Text("Mode: Rotate");
            break;
        case Mode::Zoom:
            ImGui::Text("Mode: Zoom");
            break;
    }
}

StrokePoint MakeStrokePoint(const App* app, BrushSettings settings) {
    ZoneScoped;
    SDL_assert(app->currentCanvasView.has_value());

    auto& canvasView = app->canvasViews.at(app->currentCanvasView.value());
    auto& canvasFile = app->canvasFiles.at(canvasView.canvasFileId);

    StrokePoint point{};
    point.position = canvasView.viewport.ScreenToCanvas(app->cursorCurrentPos);
    point.color = settings.color;
    point.color.a = std::lerp(settings.minPressureOpacity, settings.maxPressureOpacity, app->penPressure);
    point.radius = std::lerp(settings.minPressureRadius, settings.maxPressureRadius, app->penPressure);
    point.hardness = std::lerp(settings.minPressureHardness, settings.maxPressureHardness, app->penPressure);
    point.flow = std::lerp(settings.minPressureFlow, settings.maxPressureFlow, app->penPressure);
    return point;
}

std::vector<glm::ivec2, arena_allocator<glm::ivec2>> GetTilePosAffectedByStrokePoint(StrokePoint point,
                                                                                     arena<>& arena) {
    constexpr glm::vec2 TILE_SIZE = glm::vec2(TILE_WIDTH, TILE_HEIGHT);
    std::vector<glm::ivec2, arena_allocator<glm::ivec2>> tilesPos(arena);

    glm::ivec2 tilePosMin = glm::floor((point.position - (point.radius + 16.0f)) / TILE_SIZE);
    glm::ivec2 tilePosMax = glm::ceil((point.position + (point.radius + 16.0f)) / TILE_SIZE);

    const glm::ivec2 tileDistance = tilePosMax - tilePosMin;
    tilesPos.reserve((size_t)tileDistance.x * tileDistance.y);
    glm::ivec2 tilePos;
    for (tilePos.y = tilePosMin.y; tilePos.y < tilePosMax.y; tilePos.y++) {
        for (tilePos.x = tilePosMin.x; tilePos.x < tilePosMax.x; tilePos.x++) {
            tilesPos.push_back(tilePos);
        }
    }

    return tilesPos;
}

PaintState::PaintState(App* app) : app(app), strokePoints(app->strokeArena), strokeTileAffected(app->strokeArena) {
    ZoneScoped;
    SDL_assert(app->currentCanvasView.has_value());

    auto& canvasView = app->canvasViews.at(app->currentCanvasView.value());
    auto& canvasFile = app->canvasFiles.at(canvasView.canvasFileId);

    SDL_assert(app->currentTool == Tool::Paint);
    SDL_assert(app->currentToolState == ToolState::None);
    SDL_assert(canvasView.selectedLayer.has_value());
    SDL_assert(!canvasFile.currentTileCommand.has_value());

    app->strokeArena.reset();
    std::construct_at(&strokePoints, app->strokeArena);
    strokePoints.reserve(STROKE_MAX_POINTS);
    strokeTileAffected.reserve(256);

    canvasFile.currentTileCommand = TileModificationCommand(&canvasView, canvasView.selectedLayer.value());

    const auto& selectedLayerInfo = canvasFile.layers.at(canvasView.selectedLayer.value());
    LayerCreateInfo strokeLayerCreateInfo{};
    strokeLayerCreateInfo.name = "Stroke Layer";
    strokeLayerCreateInfo.height = selectedLayerInfo.height;
    strokeLayerCreateInfo.opacity =
        selectedLayerInfo.opacity; // FIXME: this is broken, the blending is bad. Needs to folder to be fixed
    strokeLayerCreateInfo.internal = true;
    if (auto res = canvasFile.NewLayer(strokeLayerCreateInfo); !res.has_value()) {
        SDL_assert(false && "Failed to create new layer");
    } else {
        strokeLayer = res.value();
    }

    const auto point = MakeStrokePoint(app, app->paintBrush);
    const auto tilesPos = GetTilePosAffectedByStrokePoint(point, app->frameArena);
    for (const auto& tilePos : tilesPos) {
        const Tile strokeTile{
            .layer = strokeLayer,
            .y = static_cast<TileAxis>(tilePos.y),
            .x = static_cast<TileAxis>(tilePos.x),
        };
        if (!canvasFile.tilesLoaded.contains(strokeTile)) {
            if (auto err = canvasFile.NewTile(strokeTile); err != Error::None) {
                SDL_assert(false && "Failed to create tile on stroke layer");
            }
        }
        strokeTileAffected.insert(strokeTile);

        const Tile underTile{
            .layer = canvasView.selectedLayer.value(),
            .y = static_cast<TileAxis>(tilePos.y),
            .x = static_cast<TileAxis>(tilePos.x),
        };
        canvasFile.currentTileCommand->TrackTile(underTile);
    }

    strokePoints.push_back(point);
    previousPoint = point;
    app->currentToolState = ToolState::Started;
}

void PaintState::OnEvent(const SDL_Event* event) {
    ZoneScoped;
    SDL_assert(app->currentCanvasView.has_value());

    auto& canvasView = app->canvasViews.at(app->currentCanvasView.value());
    auto& canvasFile = app->canvasFiles.at(canvasView.canvasFileId);

    if (IsPointerMove(app, event)) {
        const auto point = MakeStrokePoint(app, app->paintBrush);
        SDL_assert(app->currentTool == Tool::Paint);
        SDL_assert(app->currentToolState == ToolState::None);
        SDL_assert(canvasView.selectedLayer.has_value());
        SDL_assert(canvasFile.currentTileCommand.has_value());
        SDL_assert(canvasFile.layers.contains(canvasView.selectedLayer.value()));
        SDL_assert(canvasFile.layers.contains(strokeLayer));

        app->strokeArena.reset();
        std::construct_at(&strokePoints, app->strokeArena);
        strokePoints.reserve(STROKE_MAX_POINTS);
        strokeTileAffected.reserve(256);

        canvasFile.currentTileCommand = TileModificationCommand(&canvasView, canvasView.selectedLayer.value());

        const auto& selectedLayerInfo = canvasFile.layers.at(canvasView.selectedLayer.value());
        LayerCreateInfo strokeLayerCreateInfo{};
        strokeLayerCreateInfo.name = "Stroke Layer";
        strokeLayerCreateInfo.height = selectedLayerInfo.height;
        strokeLayerCreateInfo.opacity = selectedLayerInfo.opacity; // FIXME: this is broken, the blending is bad
        strokeLayerCreateInfo.internal = true;
        if (auto res = canvasFile.NewLayer(strokeLayerCreateInfo); !res.has_value()) {
            SDL_assert(false && "Failed to create layer");
        } else {
            strokeLayer = res.value();
        }

        const auto tilesPos = GetTilePosAffectedByStrokePoint(point, app->frameArena);
        for (const auto& tilePos : tilesPos) {
            const Tile strokeTile{
                .layer = strokeLayer,
                .y = static_cast<TileAxis>(tilePos.y),
                .x = static_cast<TileAxis>(tilePos.x),
            };
            if (!canvasFile.tilesLoaded.contains(strokeTile)) {
                if (auto err = canvasFile.NewTile(strokeTile); err != Error::None) {
                    SDL_assert(false && "Failed to create tile on stroke layer");
                }
            }
            strokeTileAffected.insert(strokeTile);

            const Tile underTile{
                .layer = canvasView.selectedLayer.value(),
                .y = static_cast<TileAxis>(tilePos.y),
                .x = static_cast<TileAxis>(tilePos.x),
            };
            canvasFile.currentTileCommand->TrackTile(underTile);
        }

        strokePoints.push_back(point);
        previousPoint = point;
        app->currentToolState = ToolState::Started;
    }

    if (IsPointerUp(app, event)) {
        SDL_assert(app->currentTool == Tool::Paint);
        SDL_assert(app->currentToolState == ToolState::Started);
        SDL_assert(canvasView.selectedLayer.has_value());
        SDL_assert(canvasFile.currentTileCommand.has_value());
        SDL_assert(canvasFile.layers.contains(canvasView.selectedLayer.value()));
        SDL_assert(canvasFile.layers.contains(strokeLayer));

        canvasFile.currentTileCommand->SaveTrackedTilesForPrevious();

        canvasFile.MergeLayers(strokeLayer, canvasView.selectedLayer.value());
        canvasFile.DeleteLayer(strokeLayer);

        canvasFile.currentTileCommand->SaveTrackedTilesForNew();
        canvasFile.canvasHistory.Push(std::move(canvasFile.currentTileCommand.value()));
        canvasFile.currentTileCommand = std::nullopt;

        strokeLayer = LAYER_INVALID;
        strokePoints.clear();
        app->currentToolState = ToolState::None;

        app->stateManager.Switch(IdleState(app));
    }
}

void PaintState::DrawUI() const {
    auto& canvasView = app->canvasViews.at(app->currentCanvasView.value());
    auto& canvasFile = app->canvasFiles.at(canvasView.canvasFileId);
    ImGui::Text("PaintState");
    ImGui::Text("Points: %zu", strokePoints.size());
}

EraseState::EraseState(App* app) : app(app), strokePoints(app->strokeArena), strokeTileAffected(app->strokeArena) {
    ZoneScoped;
    SDL_assert(app->currentCanvasView.has_value());

    auto& canvasView = app->canvasViews.at(app->currentCanvasView.value());
    auto& canvasFile = app->canvasFiles.at(canvasView.canvasFileId);

    SDL_assert(app->currentTool == Tool::Erase);
    SDL_assert(app->currentToolState == ToolState::None);
    SDL_assert(canvasView.selectedLayer.has_value());
    SDL_assert(!canvasFile.currentTileCommand.has_value() && "TileModification already started");

    app->strokeArena.reset();
    std::construct_at(&strokePoints, app->strokeArena);
    strokeTileAffected.reserve(256);
    strokePoints.reserve(STROKE_MAX_POINTS);

    canvasFile.currentTileCommand = TileModificationCommand(&canvasView, canvasView.selectedLayer.value());

    const auto point = MakeStrokePoint(app, app->eraseBrush);
    const auto tilesPos = GetTilePosAffectedByStrokePoint(point, app->frameArena);

    // TODO: find a better algorithm when erasing on layers. Right now the opacity of the erase brush does not work.
    // TODO: find a way to duplicate the layer this early and instead use a temporary internal layer as the
    // modification source
    for (const auto& tilePos : tilesPos) {
        const Tile tile{
            .layer = canvasView.selectedLayer.value(),
            .y = static_cast<TileAxis>(tilePos.y),
            .x = static_cast<TileAxis>(tilePos.x),
        };
        if (canvasFile.tilesLoaded.contains(tile)) {
            strokeTileAffected.insert(tile);
            canvasFile.MarkTileDirty(tile);
            canvasFile.currentTileCommand->TrackTile(tile);
        }
    }
    canvasFile.currentTileCommand->SaveTrackedTilesForPrevious();

    previousPoint = point;
    app->currentToolState = ToolState::Started;
}

void EraseState::OnEvent(const SDL_Event* event) {
    ZoneScoped;
    SDL_assert(app->currentCanvasView.has_value());

    auto& canvasView = app->canvasViews.at(app->currentCanvasView.value());
    auto& canvasFile = app->canvasFiles.at(canvasView.canvasFileId);

    if (IsPointerMove(app, event)) {
        const auto point = MakeStrokePoint(app, app->eraseBrush);
        SDL_assert(app->currentTool == Tool::Erase);
        SDL_assert(app->currentToolState == ToolState::Started);
        SDL_assert(canvasView.selectedLayer.has_value());
        SDL_assert(canvasFile.currentTileCommand.has_value() && "TileModification not started");

        const StrokePoint startPoint = previousPoint;
        const auto distance = glm::distance(startPoint.position, point.position);
        float distanceCovered{app->eraseBrush.maxPressureSpacing};
        while (distanceCovered < distance) {
            ZoneScoped;

            const float t = distanceCovered / distance;
            const StrokePoint newPoint = StrokePoint::Lerp(startPoint, point, t);
            strokePoints.push_back(newPoint);
            previousPoint = newPoint;
            distanceCovered += app->eraseBrush.maxPressureSpacing;

            const auto tilesPos = GetTilePosAffectedByStrokePoint(newPoint, app->frameArena);
            for (const auto& tilePos : tilesPos) {
                const Tile tile{
                    .layer = canvasView.selectedLayer.value(),
                    .y = static_cast<TileAxis>(tilePos.y),
                    .x = static_cast<TileAxis>(tilePos.x),
                };
                if (canvasFile.tilesLoaded.contains(tile)) {
                    strokeTileAffected.insert(tile);
                    canvasFile.MarkTileDirty(tile);
                    canvasFile.currentTileCommand->TrackTile(tile);
                }
            }
        }

        canvasFile.currentTileCommand->SaveTrackedTilesForPrevious(false);
    }

    if (IsPointerUp(app, event)) {
        const auto point = MakeStrokePoint(app, app->eraseBrush);

        SDL_assert(app->currentTool == Tool::Erase);
        SDL_assert(app->currentToolState == ToolState::Started);
        SDL_assert(canvasView.selectedLayer.has_value());
        SDL_assert(canvasFile.currentTileCommand && "TileModification not started");

        canvasFile.currentTileCommand->SaveTrackedTilesForNew();
        canvasFile.canvasHistory.Push(std::move(canvasFile.currentTileCommand.value()));
        canvasFile.currentTileCommand = std::nullopt;

        strokePoints.clear();

        app->currentToolState = ToolState::None;

        app->stateManager.Switch(IdleState(app));
    }
}

void EraseState::DrawUI() const {
    auto& canvasView = app->canvasViews.at(app->currentCanvasView.value());
    auto& canvasFile = app->canvasFiles.at(canvasView.canvasFileId);

    ImGui::Text("EraseState");
    ImGui::Text("Points: %zu", strokePoints.size());
}

/*
SampleState::SampleState(App* app) : app(app) {
    ZoneScoped;
    SDL_assert(app->currentCanvasView.has_value());
    // TODO take another snapshot of the whole screen (windows, DMW)

    SDL_assert(app->windowSize.x > 0);
    SDL_assert(app->windowSize.y > 0);
    SDL_assert(app->renderer.canvasTexture);

    size.x = app->windowSize.x;
    size.y = app->windowSize.y;

    std::vector<Color> canvasTexture(static_cast<std::size_t>(app->windowSize.x)
                                     * static_cast<std::size_t>(app->windowSize.y));

    const SDL_GPUTransferBufferCreateInfo transferBufferCreateInfo = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        .size = static_cast<Uint32>(canvasTexture.size() * sizeof(Color)),
    };
    SDL_GPUTransferBuffer* transferBuffer =
        SDL_CreateGPUTransferBuffer(app->renderer.device, &transferBufferCreateInfo);
    SDL_assert(transferBuffer);

    SDL_GPUCommandBuffer* cmdBuf = SDL_AcquireGPUCommandBuffer(app->renderer.device);
    SDL_assert(cmdBuf);

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmdBuf);
    SDL_GPUTextureRegion region = {
        .texture = app->renderer.canvasTexture,
        .mip_level = 0,
        .layer = 0,
        .x = 0,
        .y = 0,
        .z = 0,
        .w = static_cast<Uint32>(app->windowSize.x),
        .h = static_cast<Uint32>(app->windowSize.y),
        .d = 1,
    };
    const SDL_GPUTextureTransferInfo transferInfo = {
        .transfer_buffer = transferBuffer,
        .pixels_per_row = static_cast<Uint32>(app->windowSize.x),
        .rows_per_layer = static_cast<Uint32>(app->windowSize.y),
    };
    SDL_DownloadFromGPUTexture(copyPass, &region, &transferInfo);

    SDL_EndGPUCopyPass(copyPass);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmdBuf);
    SDL_assert(fence);
    SDL_WaitForGPUFences(app->renderer.device, true, &fence, 1);
    SDL_ReleaseGPUFence(app->renderer.device, fence);

    const auto* colors = static_cast<Color*>(SDL_MapGPUTransferBuffer(app->renderer.device, transferBuffer, false));

    std::memcpy(canvasTexture.data(), colors, canvasTexture.size() * sizeof(Color));

    SDL_UnmapGPUTransferBuffer(app->renderer.device, transferBuffer);
    SDL_ReleaseGPUTransferBuffer(app->renderer.device, transferBuffer);
}

void SampleState::OnEvent(const SDL_Event* event) {
    SDL_assert(size.x > 0);
    SDL_assert(size.y > 0);
    SDL_assert(texture.size() == static_cast<std::size_t>(size.x) * size.y && "SampleTexture is of wrong dimensions");

    const auto rx = static_cast<int>(std::round(app->cursorCurrentPos.x));
    const auto ry = static_cast<int>(std::round(app->cursorCurrentPos.y));

    if (rx < 0 || rx >= size.x) {
        return;
    }
    if (ry < 0 || ry >= size.y) {
        return;
    }

    const std::size_t index = static_cast<std::size_t>(rx) + (static_cast<std::size_t>(ry) * size.x);
    SDL_assert(index < texture.size() && "Sample out of bound");

    auto color = texture[index];
}
*/

} // namespace Midori
