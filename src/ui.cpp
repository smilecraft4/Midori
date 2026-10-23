#include "midori/ui.hpp"
#include "midori/app.hpp"
#include "midori/graphics.hpp"
#include "midori/log.hpp"
#include "midori/tile.hpp"
#include "midori/tool.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <format>
#include <numbers>
#include <utility>

#include <imgui.h>
#include <tracy/Tracy.hpp>

namespace Midori {

UiState gUiState{};

namespace {

constexpr float kFirstFrameSeconds = 1.0f / 60.0f;

constexpr std::array<const char*, std::to_underlying(BlendMode::Count_)> kBlendModeNames = {
    "Normal", "Multiply", "Add", "Screen", "Normal (OkLab)",
};

[[nodiscard]] ImGuiKey ImGuiKeyOffset(ImGuiKey first, Key key, Key firstKey) noexcept {
    return static_cast<ImGuiKey>(first + (std::to_underlying(key) - std::to_underlying(firstKey)));
}

[[nodiscard]] ImGuiKey ImGuiKeyFromKey(Key key) noexcept {
    if (key >= Key::A && key <= Key::Z) {
        return ImGuiKeyOffset(ImGuiKey_A, key, Key::A);
    }
    if (key >= Key::Num0 && key <= Key::Num9) {
        return ImGuiKeyOffset(ImGuiKey_0, key, Key::Num0);
    }
    if (key >= Key::F1 && key <= Key::F24) {
        return ImGuiKeyOffset(ImGuiKey_F1, key, Key::F1);
    }
    if (key >= Key::NumPad0 && key <= Key::NumPad9) {
        return ImGuiKeyOffset(ImGuiKey_Keypad0, key, Key::NumPad0);
    }
    switch (key) {
        case Key::Escape:
            return ImGuiKey_Escape;
        case Key::Tab:
            return ImGuiKey_Tab;
        case Key::CapsLock:
            return ImGuiKey_CapsLock;
        case Key::Backspace:
            return ImGuiKey_Backspace;
        case Key::Enter:
            return ImGuiKey_Enter;
        case Key::Space:
            return ImGuiKey_Space;
        case Key::Insert:
            return ImGuiKey_Insert;
        case Key::Delete:
            return ImGuiKey_Delete;
        case Key::Home:
            return ImGuiKey_Home;
        case Key::End:
            return ImGuiKey_End;
        case Key::PageUp:
            return ImGuiKey_PageUp;
        case Key::PageDown:
            return ImGuiKey_PageDown;
        case Key::Left:
            return ImGuiKey_LeftArrow;
        case Key::Right:
            return ImGuiKey_RightArrow;
        case Key::Up:
            return ImGuiKey_UpArrow;
        case Key::Down:
            return ImGuiKey_DownArrow;
        case Key::Minus:
            return ImGuiKey_Minus;
        case Key::Equal:
            return ImGuiKey_Equal;
        case Key::LeftBracket:
            return ImGuiKey_LeftBracket;
        case Key::RightBracket:
            return ImGuiKey_RightBracket;
        case Key::Backslash:
            return ImGuiKey_Backslash;
        case Key::Semicolon:
            return ImGuiKey_Semicolon;
        case Key::Apostrophe:
            return ImGuiKey_Apostrophe;
        case Key::Grave:
            return ImGuiKey_GraveAccent;
        case Key::Comma:
            return ImGuiKey_Comma;
        case Key::Period:
            return ImGuiKey_Period;
        case Key::Slash:
            return ImGuiKey_Slash;
        case Key::NumPadDecimal:
            return ImGuiKey_KeypadDecimal;
        case Key::NumPadDivide:
            return ImGuiKey_KeypadDivide;
        case Key::NumPadMultiply:
            return ImGuiKey_KeypadMultiply;
        case Key::NumPadSubtract:
            return ImGuiKey_KeypadSubtract;
        case Key::NumPadAdd:
            return ImGuiKey_KeypadAdd;
        case Key::NumPadEnter:
            return ImGuiKey_KeypadEnter;
        case Key::NumLock:
            return ImGuiKey_NumLock;
        case Key::ScrollLock:
            return ImGuiKey_ScrollLock;
        case Key::PrintScreen:
            return ImGuiKey_PrintScreen;
        case Key::Pause:
            return ImGuiKey_Pause;
        case Key::Menu:
            return ImGuiKey_Menu;
        case Key::LeftShift:
            return ImGuiKey_LeftShift;
        case Key::RightShift:
            return ImGuiKey_RightShift;
        case Key::LeftCtrl:
            return ImGuiKey_LeftCtrl;
        case Key::RightCtrl:
            return ImGuiKey_RightCtrl;
        case Key::LeftAlt:
            return ImGuiKey_LeftAlt;
        case Key::RightAlt:
            return ImGuiKey_RightAlt;
        case Key::LeftSuper:
            return ImGuiKey_LeftSuper;
        case Key::RightSuper:
            return ImGuiKey_RightSuper;
        default:
            return ImGuiKey_None;
    }
}

[[nodiscard]] ImGuiMouseButton ImGuiButtonFromButton(Button button) noexcept {
    switch (button) {
        case Button::Left:
            return ImGuiMouseButton_Left;
        case Button::Right:
            return ImGuiMouseButton_Right;
        case Button::Middle:
            return ImGuiMouseButton_Middle;
        case Button::Unknown:
            break;
    }
    return -1;
}

[[nodiscard]] ImGuiMouseSource ImGuiSourceFromDevice(Device device) noexcept {
    return device == kDeviceSystem ? ImGuiMouseSource_Mouse : ImGuiMouseSource_Pen;
}

[[nodiscard]] Window EventWindow(const Event& event) noexcept {
    switch (event.type) {
        case EventType::WindowResize:
            return event.windowResize.window;
        case EventType::WindowFocus:
            return event.windowFocus.window;
        case EventType::WindowClose:
            return event.windowClose.window;
        case EventType::WindowDestroy:
            return event.windowDestroy.window;
        case EventType::Key:
            return event.key.window;
        case EventType::Text:
            return event.text.window;
        case EventType::Button:
            return event.button.window;
        case EventType::Pointer:
            return event.pointer.window;
        case EventType::Wheel:
            return event.wheel.window;
        case EventType::Unknown:
            break;
    }
    return kWindowInvalid;
}

void BrushEdit(BrushState& brush) noexcept {
    ZoneScopedN("BrushEdit");
    ImGui::SliderFloat("Radius (F)", &brush.radius, kBrushRadiusMin, kBrushRadiusMax, "%.1f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Hardness (Ctrl+F)", &brush.hardness, 0.0f, 1.0f);
    ImGui::SliderFloat("Flow", &brush.flow, 0.0f, 1.0f);
    ImGui::SliderFloat("Opacity (Shift+F)", &brush.opacity, 0.0f, 1.0f);
    ImGui::SliderFloat("Spacing", &brush.spacing, 0.01f, 1.0f);
    ImGui::Checkbox("Pressure radius", &brush.pressureRadius);
    ImGui::Checkbox("Pressure opacity", &brush.pressureOpacity);
}

// Outlines every tile of the window's canvas view on screen, labeled with its atlas cell and grid position. Drawn
// behind the UI windows, over the canvas.
void TileCellsOverlay(Window window, const WindowState& windowState) noexcept {
    ZoneScopedN("TileCellsOverlay");
    const auto view = std::ranges::find(gAppState.canvasViews, window, &CanvasViewState::window);
    if (view == gAppState.canvasViews.end() || view->id == kCanvasViewInvalid) {
        return;
    }
    // Canvas to view DIPs as in the renderer, then to client pixels around the floored center.
    const Viewport& viewport = view->viewport;
    const float c = std::cos(viewport.rot);
    const float s = std::sin(viewport.rot);
    const float centerX = std::floor(static_cast<float>(windowState.width) * 0.5f);
    const float centerY = std::floor(static_cast<float>(windowState.height) * 0.5f);
    const float scale = windowState.dpiScale;
    const auto toScreen = [&](double x, double y) {
        const auto dx = static_cast<float>(x - viewport.posX);
        const auto dy = static_cast<float>(y - viewport.posY);
        return ImVec2(centerX + (((dx * c) - (dy * s)) * viewport.zoomX * scale),
                      centerY + (((dx * s) + (dy * c)) * viewport.zoomY * scale));
    };

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    constexpr auto kSize = static_cast<double>(kTileSize);
    const CanvasState& canvas = gAppState.canvases[view->canvas];
    const TileMip level = TileLevelForZoom(viewport.zoomX);
    for (const TileState& tile : canvas.tiles) {
        if (!TileDrawn(canvas, tile, level)) {
            continue;
        }
        const TileGridPos gridPos = TileKeyGridPos(tile.key);
        const double x = static_cast<double>(gridPos.x) * kSize;
        const double y = static_cast<double>(gridPos.y) * kSize;
        drawList->AddQuad(toScreen(x, y), toScreen(x + kSize, y), toScreen(x + kSize, y + kSize),
                          toScreen(x, y + kSize), IM_COL32(255, 0, 255, 255));

        std::array<char, 64> text{};
        const auto layer = static_cast<Layer>((tile.key & kTileLayerMask) >> kTileLayerBitOffset);
        std::format_to_n(text.data(), text.size() - 1, "p{} b{}\n{}, {}\nL{} m{}", TileAtlasCellPage(tile.cell),
                         TileAtlasCellBlock(tile.cell), gridPos.x, gridPos.y, layer, TileKeyMip(tile.key));
        const ImVec2 center = toScreen(x + (kSize * 0.5), y + (kSize * 0.5));
        const ImVec2 textSize = ImGui::CalcTextSize(text.data());
        const ImVec2 pos(center.x - (textSize.x * 0.5f), center.y - (textSize.y * 0.5f));
        // Shadowed so it reads over any paint.
        drawList->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32_BLACK, text.data());
        drawList->AddText(pos, IM_COL32_WHITE, text.data());
    }
}

// The brush value being adjusted, around its center. Radius shows the brush outline with its hard core. Opacity and
// hardness show the value's ring inside the faint ring of 1; hardness also draws the dab's falloff across it.
void BrushAdjustOverlay(Window window, const WindowState& windowState, const ToolsState& tools) noexcept {
    ZoneScopedN("BrushAdjustOverlay");
    const BrushAdjustState& adjust = gAppState.brushAdjust;
    if (adjust.mode == BrushAdjustMode::None || gAppState.canvasViews[adjust.view].window != window) {
        return;
    }
    const BrushState& brush = ToolBrush(tools, adjust.tool);
    const float scale = windowState.dpiScale;
    const ImVec2 center(adjust.centerX, adjust.centerY);
    constexpr ImU32 kStrong = IM_COL32(40, 130, 230, 255);
    constexpr ImU32 kFaint = IM_COL32(40, 130, 230, 120);
    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    float value = 0.0f;
    if (adjust.mode == BrushAdjustMode::Radius) {
        const float radius = brush.radius * std::abs(gAppState.canvasViews[adjust.view].viewport.zoomX) * scale;
        drawList->AddCircle(center, radius, kStrong, 0, 2.0f * scale);
        drawList->AddCircle(center, radius * brush.hardness, kFaint, 0, scale);
        value = brush.radius;
    } else {
        const float range = kBrushAdjustRangeDips * scale;
        value = adjust.mode == BrushAdjustMode::Opacity ? brush.opacity : brush.hardness;
        drawList->AddCircle(center, range, kFaint, 0, scale);
        drawList->AddCircle(center, value * range, kStrong, 0, 2.0f * scale);
        // if (adjust.mode == BrushAdjustMode::Hardness) {
        //     // DabMask in stroke.cs.hlsl, for a dab as wide as the range.
        //     constexpr std::size_t kSegments = 64;
        //     std::array<ImVec2, kSegments + 1> curve{};
        //     const float edge = std::max((1.0f - brush.hardness) * range, 1.0f);
        //     for (std::size_t i = 0; i <= kSegments; ++i) {
        //         const float x = ((2.0f * static_cast<float>(i) / static_cast<float>(kSegments)) - 1.0f) * range;
        //         const float t = std::clamp((range - std::abs(x)) / edge, 0.0f, 1.0f);
        //         curve[i] = ImVec2(center.x + x, center.y - (t * t * (3.0f - (2.0f * t)) * range));
        //     }
        //     drawList->AddPolyline(curve.data(), static_cast<int>(curve.size()), IM_COL32(220, 220, 220, 255), 0,
        //                           scale);
        // }
    }

    std::array<char, 16> text{};
    std::format_to_n(text.data(), text.size() - 1, "{:.{}f}", value, adjust.mode == BrushAdjustMode::Radius ? 1 : 3);
    const ImVec2 textSize = ImGui::CalcTextSize(text.data());
    const ImVec2 pos(center.x - (textSize.x * 0.5f), center.y - (textSize.y * 0.5f));
    drawList->AddCircle(center, (textSize.x * 0.5f) + (6.0f * scale), kFaint, 0, scale);
    drawList->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32_BLACK, text.data());
    drawList->AddText(pos, IM_COL32_WHITE, text.data());
}

// The chosen atlas's cells at mip 0, kTileAtlasDebugColumns per row, sized to the window width. Premultiplied cells are
// drawn as straight alpha, so translucent paint looks darker than on the canvas.
void TileAtlasWindow(Window window) noexcept {
    ZoneScopedN("TileAtlasWindow");
    ImGui::SetNextWindowSize(ImVec2(400.0f, 450.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Tile atlas", &gUiState.tileAtlasShown[window])) {
        ImGui::End();
        return;
    }
    const TileAtlasState& atlasState = gAppState.atlas;
    const int atlasCount = atlasState.count;
    int usedBlocks = 0;
    for (std::size_t page = 0; page < std::size_t{atlasState.count} * kTileAtlasPages; ++page) {
        usedBlocks += std::popcount(atlasState.used[page]);
    }
    ImGui::Text("%d of %d blocks used, %d atlases of %d", usedBlocks, atlasCount * kTileAtlasPages * kTileAtlasBlocks,
                atlasCount, static_cast<int>(kTileAtlasMax));
    const auto busy = [](const auto& slots) {
        return static_cast<int>(
            std::ranges::count_if(slots, [](const TileSlot& slot) { return slot.state != TileSlotState::Free; }));
    };
    ImGui::Text("%d uploads, %d downloads in flight of %d", busy(gAppState.transfers.uploads),
                busy(gAppState.transfers.downloads), static_cast<int>(kTileTransferSlots));
    int& atlas = gUiState.tileAtlasIndex[window];
    ImGui::SliderInt("Atlas", &atlas, 0, atlasCount - 1, "%d", ImGuiSliderFlags_AlwaysClamp);
    atlas = std::clamp(atlas, 0, atlasCount - 1);

    const std::uint64_t texture = GraphicsTileAtlasDebugTexture(static_cast<TileAtlas>(atlas));
    if (texture == 0) {
        ImGui::TextUnformatted("The atlas texture could not be created.");
        ImGui::End();
        return;
    }
    constexpr float kColumns = kTileAtlasDebugColumns;
    constexpr float kRows = kTileAtlasDebugRows;
    const float cellSize = std::max(ImGui::GetContentRegionAvail().x / kColumns, 16.0f);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    for (TileAtlasPage slice = 0; slice < kTileAtlasPages; ++slice) {
        const int cellRow = slice / kTileAtlasDebugColumns;
        const auto column = static_cast<float>(slice % kTileAtlasDebugColumns);
        const auto row = static_cast<float>(cellRow);
        const ImVec2 min(origin.x + (column * cellSize), origin.y + (row * cellSize));
        const ImVec2 max(min.x + cellSize, min.y + cellSize);
        drawList->AddImage(ImTextureRef(texture), min, max, ImVec2(column / kColumns, row / kRows),
                           ImVec2((column + 1.0f) / kColumns, (row + 1.0f) / kRows));
        drawList->AddRect(min, max, IM_COL32(128, 128, 128, 255));
    }
    ImGui::Dummy(ImVec2(cellSize * kColumns, cellSize * kRows));
    ImGui::End();
}

// ScaleAllSizes is not reversible, so a DPI change rebuilds the style from the defaults.
void ApplyDpiScale(float dpiScale) noexcept {
    ZoneScopedN("ApplyDpiScale");
    ImGuiStyle style{};
    ImGui::StyleColorsDark(&style);
    style.ScaleAllSizes(dpiScale);
    style.FontScaleDpi = dpiScale;
    ImGui::GetStyle() = style;
}

} // namespace

Result<ImGuiContext*> UiWindowInit(Window window) noexcept {
    ZoneScopedN("UiWindowInit");
    MIDORI_ASSERT(window < kWindowMax && !gUiState.contexts[window], "UiWindowInit: window {} taken", window);
    const auto state = WindowGetState(window);
    if (!state) {
        return std::unexpected(state.error());
    }
    const auto hwnd = WindowGetNativeHandle(window);
    if (!hwnd) {
        return std::unexpected(hwnd.error());
    }

    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
    ImGuiIO& io = ImGui::GetIO();
    // Every context would write the same imgui.ini.
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.BackendPlatformName = "midori";
    // No keyboard navigation: Space would press the focused widget instead of navigating the canvas.
    // imgui.cpp's default Win32 IME handler places the candidate window over this handle.
    ImGui::GetMainViewport()->PlatformHandleRaw = *hwnd;
    ApplyDpiScale(state->dpiScale);

    gUiState.contexts[window] = context;
    gUiState.frameTimes[window] = 0;
    gUiState.tileAtlasShown[window] = false;
    gUiState.tileAtlasIndex[window] = 0;
    gUiState.tileCellsShown[window] = false;
    gUiState.hidden[window] = false;
    return context;
}

void UiWindowTerminate(Window window) noexcept {
    ZoneScopedN("UiWindowTerminate");
    if (window >= kWindowMax || !gUiState.contexts[window]) {
        return;
    }
    ImGui::DestroyContext(gUiState.contexts[window]);
    gUiState.contexts[window] = nullptr;
    gUiState.frameTimes[window] = 0;
}

bool UiEvent(const Event& event) noexcept {
    ZoneScopedN("UiEvent");
    const Window window = EventWindow(event);
    if (window >= kWindowMax || !gUiState.contexts[window]) {
        return false;
    }
    ImGui::SetCurrentContext(gUiState.contexts[window]);
    ImGuiIO& io = ImGui::GetIO();

    switch (event.type) {
        case EventType::WindowResize:
            ApplyDpiScale(event.windowResize.dpiScale);
            return false;
        case EventType::WindowFocus:
            io.AddFocusEvent(event.windowFocus.focused);
            return false;
        case EventType::WindowClose:
        case EventType::WindowDestroy:
            return false;
        case EventType::Key: {
            // ImGui repeats held keys itself.
            if (event.key.inputState == InputState::Hold) {
                return io.WantCaptureKeyboard;
            }
            const KeyMod mods = event.key.keyMods;
            io.AddKeyEvent(ImGuiMod_Ctrl, HasAnyFlag(mods, KeyMod::Ctrl));
            io.AddKeyEvent(ImGuiMod_Shift, HasAnyFlag(mods, KeyMod::Shift));
            io.AddKeyEvent(ImGuiMod_Alt, HasAnyFlag(mods, KeyMod::Alt));
            io.AddKeyEvent(ImGuiMod_Super, HasAnyFlag(mods, KeyMod::Super));
            if (const ImGuiKey key = ImGuiKeyFromKey(event.key.key); key != ImGuiKey_None) {
                io.AddKeyEvent(key, event.key.inputState == InputState::Press);
            }
            return io.WantCaptureKeyboard;
        }
        case EventType::Text:
            io.AddInputCharacter(static_cast<unsigned int>(event.text.codepoint));
            return io.WantTextInput;
        case EventType::Button:
            if (const ImGuiMouseButton button = ImGuiButtonFromButton(event.button.button); button >= 0) {
                io.AddMouseSourceEvent(ImGuiSourceFromDevice(event.button.device));
                io.AddMouseButtonEvent(button, event.button.inputState == InputState::Press);
            }
            return io.WantCaptureMouse;
        case EventType::Pointer:
            io.AddMouseSourceEvent(ImGuiSourceFromDevice(event.pointer.device));
            io.AddMousePosEvent(event.pointer.posX, event.pointer.posY);
            return io.WantCaptureMouse;
        case EventType::Wheel:
            io.AddMouseSourceEvent(ImGuiSourceFromDevice(event.wheel.device));
            io.AddMouseWheelEvent(-event.wheel.deltaX, event.wheel.deltaY);
            return io.WantCaptureMouse;
        case EventType::Unknown:
            break;
    }
    return false;
}

UiCommand UiWindowFrame(Window window, Viewport* viewport, Color::Srgb* backgroundColor, ToolsState& tools) noexcept {
    ZoneScopedN("UiWindowFrame");
    MIDORI_ASSERT(window < kWindowMax && gUiState.contexts[window], "UiWindowFrame: no context for window {}", window);
    const auto state = WindowGetState(window);
    if (!state) {
        return UiCommand::None;
    }
    ImGui::SetCurrentContext(gUiState.contexts[window]);
    ImGuiIO& io = ImGui::GetIO();

    const std::int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::int64_t last = std::exchange(gUiState.frameTimes[window], now);
    io.DeltaTime = kFirstFrameSeconds;
    if (last != 0 && now > last) {
        using Ticks = std::chrono::steady_clock::duration;
        io.DeltaTime = std::chrono::duration<float>(Ticks(now - last)).count();
    }
    io.DisplaySize = ImVec2(static_cast<float>(state->width), static_cast<float>(state->height));

    ImGui::NewFrame();
    // ImGui::ShowDemoWindow();
    if (gUiState.hidden[window]) {
        BrushAdjustOverlay(window, *state, tools);
        ImGui::Render();
        return UiCommand::None;
    }
    UiCommand command = UiCommand::None;
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New", "Ctrl+N")) {
                command = UiCommand::CanvasNew;
            }
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                command = UiCommand::CanvasOpen;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S", false, viewport != nullptr)) {
                command = UiCommand::CanvasSave;
            }
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S", false, viewport != nullptr)) {
                command = UiCommand::CanvasSaveAs;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4")) {
                command = UiCommand::Exit;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, viewport != nullptr)) {
                command = UiCommand::Undo;
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, viewport != nullptr)) {
                command = UiCommand::Redo;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window")) {
            if (ImGui::MenuItem("Fullscreen", "F11", state->displayMode == WindowDisplayMode::Fullscreened)) {
                command = UiCommand::ToggleFullscreen;
            }
            ImGui::MenuItem("Hide UI", "Tab", &gUiState.hidden[window]);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Debug")) {
            ImGui::MenuItem("Tile atlas", nullptr, &gUiState.tileAtlasShown[window]);
            ImGui::MenuItem("Tile cells on canvas", nullptr, &gUiState.tileCellsShown[window]);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    if (viewport) {
        if (ImGui::Begin("View")) {
            ImGui::Text("Zoom %.1f%%  Rotation %.1f deg", std::abs(viewport->zoomX) * 100.0f,
                        viewport->rot * 180.0f / std::numbers::pi_v<float>);
            // The pointer in canvas units, as painting places it.
            const float centerX = std::floor(static_cast<float>(state->width) * 0.5f);
            const float centerY = std::floor(static_cast<float>(state->height) * 0.5f);
            const Viewport under = ViewportMove(*viewport, -(gAppState.pointerX - centerX) / state->dpiScale,
                                                -(gAppState.pointerY - centerY) / state->dpiScale);
            ImGui::Text("Center %.1f, %.1f", viewport->posX, viewport->posY);
            ImGui::Text("Pointer %.1f, %.1f", under.posX, under.posY);
            if (ImGui::Button("Flip X")) {
                *viewport = ViewportZoom(*viewport, -1.0f, 1.0f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Flip Y")) {
                *viewport = ViewportZoom(*viewport, 1.0f, -1.0f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset (Space+R)")) {
                *viewport = ViewportSnap(ViewportResetZoomRotation(*viewport), state->dpiScale);
            }
            ImGui::ColorEdit4("Background", &backgroundColor->r);
        }
        ImGui::End();
    }
    if (ImGui::Begin("Tool")) {
        int tool = std::to_underlying(tools.tool);
        ImGui::RadioButton("Paint (B)", &tool, std::to_underlying(Tool::Paint));
        ImGui::SameLine();
        ImGui::RadioButton("Erase (E)", &tool, std::to_underlying(Tool::Erase));
        tools.tool = static_cast<Tool>(tool);
        ImGui::Separator();
        if (tools.tool == Tool::Paint) {
            ImGui::ColorEdit4("Color", &tools.paint.color.r);
            int blendMode = std::to_underlying(tools.paint.blendMode);
            if (ImGui::Combo("Blend", &blendMode, kBlendModeNames.data(), static_cast<int>(kBlendModeNames.size()))) {
                tools.paint.blendMode = static_cast<BlendMode>(blendMode);
            }
            BrushEdit(tools.paint.brush);
        } else {
            ImGui::ColorEdit4("Removes", &tools.erase.color.r);
            BrushEdit(tools.erase.brush);
        }
    }
    ImGui::End();
    if (gUiState.tileAtlasShown[window]) {
        TileAtlasWindow(window);
    }
    if (gUiState.tileCellsShown[window]) {
        TileCellsOverlay(window, *state);
    }
    BrushAdjustOverlay(window, *state, tools);
    ImGui::Render();
    return command;
}

} // namespace Midori
