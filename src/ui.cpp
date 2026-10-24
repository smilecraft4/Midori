#include "midori/ui.hpp"
#include "midori/app.hpp"
#include "midori/graphics.hpp"
#include "midori/layer.hpp"
#include "midori/log.hpp"
#include "midori/tile.hpp"
#include "midori/tool.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

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

// Whether pressure drives a property, and its value at no pressure and at full pressure, as a share of the slider's.
void PressureEdit(const char* label, bool& enabled, float& min, float& max) noexcept {
    ImGui::PushID(label);
    ImGui::Checkbox("##enabled", &enabled);
    ImGui::SameLine();
    ImGui::BeginDisabled(!enabled);
    ImGui::DragFloatRange2(label, &min, &max, 0.005f, 0.0f, 1.0f, "Min %.2f", "Max %.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    ImGui::PopID();
}

void BrushEdit(BrushState& brush) noexcept {
    ZoneScopedN("BrushEdit");
    ImGui::SliderFloat("Radius (F)", &brush.radius, kBrushRadiusMin, kBrushRadiusMax, "%.1f",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Hardness (Ctrl+F)", &brush.hardness, 0.0f, 1.0f);
    ImGui::SliderFloat("Flow", &brush.flow, 0.0f, 1.0f);
    ImGui::SliderFloat("Opacity (Shift+F)", &brush.opacity, 0.0f, 1.0f);
    ImGui::SliderFloat("Spacing", &brush.spacing, 0.01f, 1.0f);
    PressureEdit("Pressure radius", brush.pressureRadius, brush.pressureRadiusMin, brush.pressureRadiusMax);
    PressureEdit("Pressure hardness", brush.pressureHardness, brush.pressureHardnessMin, brush.pressureHardnessMax);
    PressureEdit("Pressure flow", brush.pressureFlow, brush.pressureFlowMin, brush.pressureFlowMax);
    PressureEdit("Pressure opacity", brush.pressureOpacity, brush.pressureOpacityMin, brush.pressureOpacityMax);
}

void ToolWindow(Window window, ToolsState& tools) noexcept {
    ZoneScopedN("ToolWindow");
    if (ImGui::Begin("Tool", &gUiState.toolShown[window])) {
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

// The view's position, zoom and rotation in the bottom right corner, and above it the active layer's name, blend mode
// and opacity, with how many more are selected. Small and faint so it never draws the eye, behind the UI windows and
// shown with them hidden too.
void ViewInfoOverlay(const WindowState& windowState, const Viewport& viewport, const CanvasState& canvas) noexcept {
    ZoneScopedN("ViewInfoOverlay");
    std::array<char, 128> viewText{};
    const char* flip = "";
    if (viewport.zoomX < 0.0f && viewport.zoomY < 0.0f) {
        flip = " flip XY";
    } else if (viewport.zoomX < 0.0f) {
        flip = " flip X";
    } else if (viewport.zoomY < 0.0f) {
        flip = " flip Y";
    }
    // The texel under the view center.
    std::format_to_n(viewText.data(), viewText.size() - 1, "{}, {} {:.1f}% {:.1f}\u00B0{}",
                     static_cast<std::int64_t>(std::floor(viewport.posX)),
                     static_cast<std::int64_t>(std::floor(viewport.posY)), std::abs(viewport.zoomX) * 100.0f,
                     viewport.rot * 180.0f / std::numbers::pi_v<float>, flip);
    std::array<char, 128> layerText{};
    if (const Layer active = canvas.activeLayer; active != kLayerInvalid) {
        const LayerState& layer = canvas.layers[active];
        const std::size_t selected = LayersSelected(canvas).size();
        const char* name = canvas.layerNames[active].data();
        const char* blendMode = kBlendModeNames[std::to_underlying(layer.blendMode)];
        const float opacity = layer.opacity * 100.0f;
        if (selected > 1) {
            std::format_to_n(layerText.data(), layerText.size() - 1, "{} (+{}) {} {:.0f}%", name, selected - 1,
                             blendMode, opacity);
        } else {
            std::format_to_n(layerText.data(), layerText.size() - 1, "{} {} {:.0f}%", name, blendMode, opacity);
        }
    }

    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * 0.85f;
    const float margin = 8.0f * windowState.dpiScale;
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    // Right aligned, from the bottom line up.
    float bottom = static_cast<float>(windowState.height) - margin;
    for (const char* text : {viewText.data(), layerText.data()}) {
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
        const ImVec2 pos(static_cast<float>(windowState.width) - extent.x - margin, bottom - extent.y);
        drawList->AddText(font, size, ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 90), text);
        drawList->AddText(font, size, pos, IM_COL32(255, 255, 255, 150), text);
        bottom = pos.y;
    }
}

// The brush value being adjusted, around its center. Radius shows the brush outline. Opacity and
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
        value = brush.radius;
    } else {
        const float range = kBrushAdjustRangeDips * scale;
        value = adjust.mode == BrushAdjustMode::Opacity ? brush.opacity : brush.hardness;
        if (adjust.mode == BrushAdjustMode::LayerOpacity) {
            value = gAppState.canvases[gAppState.canvasViews[adjust.view].canvas].layers[adjust.layer].opacity;
        }
        drawList->AddCircle(center, range, kFaint, 0, scale);
        drawList->AddCircle(center, value * range, kStrong, 0, 2.0f * scale);
    }

    // Shadowed so it reads over any paint, centered on x with its top at y.
    const auto label = [drawList](const char* text, float x, float y) {
        const ImVec2 pos(x - (ImGui::CalcTextSize(text).x * 0.5f), y);
        drawList->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32_BLACK, text);
        drawList->AddText(pos, IM_COL32_WHITE, text);
    };
    constexpr std::array kNames = {"", "Radius", "Opacity", "Hardness", "Layer opacity"};
    std::array<char, 16> text{};
    // A radius is whole from 1 up, see BrushAdjustApply.
    int decimals = 3;
    if (adjust.mode == BrushAdjustMode::Radius) {
        decimals = value >= 1.0f ? 0 : 1;
    }
    std::format_to_n(text.data(), text.size() - 1, "{:.{}f}", value, decimals);
    // The value at the center, the property it belongs to right above.
    const float lineHeight = ImGui::GetTextLineHeight();
    label(kNames[std::to_underlying(adjust.mode)], center.x, center.y - (lineHeight * 1.5f));
    label(text.data(), center.x, center.y - (lineHeight * 0.5f));
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

constexpr const char* kLayerDragPayload = "MIDORI_LAYER";

// Where `layer` lands when dropped on `target`, as a LayerMove height: right above a layer, inside a folder on top.
// kLayerInvalid when `target` is the layer or inside it.
[[nodiscard]] Layer LayerDropHeight(const CanvasState& canvas, Layer layer, Layer target) noexcept {
    const Layer top = canvas.layerHeights[layer];
    const auto count = static_cast<Layer>(canvas.layers[layer].descendants + 1);
    Layer height = canvas.layerHeights[target];
    if (height + count > top && height <= top) {
        return kLayerInvalid;
    }
    if (height > top) {
        height = static_cast<Layer>(height - count);
    }
    return HasAnyFlag(canvas.layers[target].flags, LayerFlags::Folder) ? height : static_cast<Layer>(height + 1);
}

void LayerPropertiesEdit(const CanvasState& canvas, Layer layer, const LayerProperties& before,
                         const LayerProperties& after) noexcept {
    if (LayerPropertiesSet(canvas.id, layer, after)) {
        LayerPropertiesCommit(canvas.id, std::span(&layer, 1), std::span(&before, 1));
    }
}

// The multi-select scope's requests on the rows shown, by index. Clearing clears the layers of closed folders too.
void LayerSelectionApply(CanvasState& canvas, std::span<const Layer> rows, const ImGuiMultiSelectIO& io) noexcept {
    std::vector<Layer>& selected = canvas.selectedLayers;
    for (const ImGuiSelectionRequest& request : io.Requests) {
        if (request.Type == ImGuiSelectionRequestType_SetAll) {
            selected.clear();
            if (request.Selected) {
                selected.assign(rows.begin(), rows.end());
            }
            continue;
        }
        if (request.Type != ImGuiSelectionRequestType_SetRange) {
            continue;
        }
        for (auto i = request.RangeFirstItem; i <= request.RangeLastItem; ++i) {
            const Layer layer = rows[static_cast<std::size_t>(i)];
            std::erase(selected, layer);
            if (request.Selected) {
                selected.push_back(layer);
            }
        }
    }
}

// The window's canvas layers, top first, one line each as in Photoshop, folders indented under theirs. Click picks the
// active layer, Ctrl+click and Shift+click select more, the blend mode and opacity then set all of them. Double click
// renames, drag moves one layer: onto a layer right above it, onto a folder inside it on top.
void LayersWindow(Window window) noexcept {
    ZoneScopedN("LayersWindow");
    ImGui::SetNextWindowSize(ImVec2(300.0f, 420.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Layers", &gUiState.layersShown[window])) {
        ImGui::End();
        return;
    }
    const auto view = std::ranges::find(gAppState.canvasViews, window, &CanvasViewState::window);
    if (view == gAppState.canvasViews.end() || view->id == kCanvasViewInvalid) {
        ImGui::TextUnformatted("No canvas");
        ImGui::End();
        return;
    }
    CanvasState& canvas = gAppState.canvases[view->canvas];

    // The blend mode and opacity over the list: the active layer's shown, every selected one's set.
    const Layer active = canvas.activeLayer;
    const std::vector<Layer> selected = LayersSelected(canvas);
    // As shown, so Ctrl+click adds to the active layer selected alone, and layers gone leave it.
    canvas.selectedLayers = selected;
    if (active != kLayerInvalid && !selected.empty()) {
        const LayerProperties shown = LayerPropertiesGet(canvas, active);
        int blendMode = std::to_underlying(shown.blendMode);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
        if (ImGui::Combo("##blend", &blendMode, kBlendModeNames.data(), static_cast<int>(kBlendModeNames.size()))) {
            AppLayerBlendMode(window, static_cast<BlendMode>(blendMode));
        }
        ImGui::SameLine();
        float percent = shown.opacity * 100.0f;
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool changed =
            ImGui::SliderFloat("##opacity", &percent, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
        // One undo step per drag, from where each layer started.
        std::vector<float>& before = gUiState.layerOpacitiesBefore[window];
        if (ImGui::IsItemActivated()) {
            before.clear();
            for (const Layer layer : selected) {
                before.push_back(canvas.layers[layer].opacity);
            }
        }
        if (changed) {
            for (const Layer layer : selected) {
                LayerProperties properties = LayerPropertiesGet(canvas, layer);
                properties.opacity = percent / 100.0f;
                if (!LayerPropertiesSet(canvas.id, layer, properties)) {
                    break;
                }
            }
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && before.size() == selected.size()) {
            std::vector<LayerProperties> start;
            for (std::size_t i = 0; i < selected.size(); ++i) {
                start.push_back(LayerPropertiesGet(canvas, selected[i]));
                start.back().opacity = before[i];
            }
            LayerPropertiesCommit(canvas.id, selected, start);
        }
    }

    if (canvas.merge.result != kLayerInvalid) {
        ImGui::Text("Merging, %d tiles left",
                    static_cast<int>(canvas.merge.remaining.size() + canvas.merge.queued.size()));
    }

    const float footer = ImGui::GetFrameHeightWithSpacing();
    struct Drop {
        Layer layer;
        Layer target;
    };
    Drop drop{.layer = kLayerInvalid, .target = kLayerInvalid};
    if (ImGui::BeginChild("rows", ImVec2(0.0f, -footer), ImGuiChildFlags_Borders)) {
        // The rows shown, top first: a closed folder's layers sit right below it.
        std::vector<std::uint16_t>& closed = gUiState.layerFoldersClosed[window];
        std::vector<Layer> rows;
        for (int h = static_cast<int>(canvas.layersOrder.size()) - 2; h >= 0; --h) {
            const Layer id = canvas.layersOrder[static_cast<std::size_t>(h)];
            rows.push_back(id);
            if (std::ranges::contains(closed, id)) {
                h -= canvas.layers[id].descendants;
            }
        }
        int selectedShown = 0;
        for (const Layer id : rows) {
            selectedShown += std::ranges::contains(selected, id) ? 1 : 0;
        }
        const ImGuiMultiSelectFlags flags =
            ImGuiMultiSelectFlags_ClearOnEscape | ImGuiMultiSelectFlags_ClearOnClickVoid;
        if (ImGuiMultiSelectIO* io = ImGui::BeginMultiSelect(flags, selectedShown, static_cast<int>(rows.size()))) {
            LayerSelectionApply(canvas, rows, *io);
        }
        const float indent = ImGui::GetStyle().IndentSpacing;
        for (std::size_t row = 0; row < rows.size(); ++row) {
            const Layer id = rows[row];
            const LayerState& layer = canvas.layers[id];
            const bool folder = HasAnyFlag(layer.flags, LayerFlags::Folder);
            int depth = 0;
            for (Layer p = canvas.layerParents[id]; canvas.layerParents[p] != kLayerInvalid;
                 p = canvas.layerParents[p]) {
                ++depth;
            }
            ImGui::PushID(id);

            bool visible = !HasAnyFlag(layer.flags, LayerFlags::Hidden);
            if (ImGui::Checkbox("##visible", &visible)) {
                const LayerProperties before = LayerPropertiesGet(canvas, id);
                LayerProperties after = before;
                after.hidden = !visible;
                LayerPropertiesEdit(canvas, id, before, after);
            }
            ImGui::SameLine();
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (static_cast<float>(depth) * indent));
            if (folder) {
                const bool open = !std::ranges::contains(closed, id);
                if (ImGui::ArrowButton("##folder", open ? ImGuiDir_Down : ImGuiDir_Right)) {
                    if (open) {
                        closed.push_back(id);
                    } else {
                        std::erase(closed, id);
                    }
                }
                ImGui::SameLine();
            }

            if (gUiState.layerRenaming[window] == id) {
                std::array<char, 64>& text = gUiState.layerRename[window];
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) {
                    ImGui::SetKeyboardFocusHere();
                }
                const bool entered =
                    ImGui::InputText("##rename", text.data(), text.size(),
                                     ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if (entered) {
                    const LayerProperties before = LayerPropertiesGet(canvas, id);
                    LayerProperties after = before;
                    after.name = LayerName(text.data());
                    LayerPropertiesEdit(canvas, id, before, after);
                }
                if (entered || ImGui::IsItemDeactivated()) {
                    gUiState.layerRenaming[window] = -1;
                }
            } else {
                ImGui::SetNextItemSelectionUserData(static_cast<ImGuiSelectionUserData>(row));
                if (ImGui::Selectable(canvas.layerNames[id].data(), std::ranges::contains(selected, id),
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    canvas.activeLayer = id;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        gUiState.layerRenaming[window] = id;
                        gUiState.layerRename[window] = canvas.layerNames[id];
                    }
                }
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload(kLayerDragPayload, &id, sizeof(id));
                    ImGui::TextUnformatted(canvas.layerNames[id].data());
                    ImGui::EndDragDropSource();
                }
                // Moved once the list is drawn, not while it is walked.
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(kLayerDragPayload)) {
                        std::memcpy(&drop.layer, payload->Data, sizeof(drop.layer));
                        drop.target = id;
                    }
                    ImGui::EndDragDropTarget();
                }
            }
            ImGui::PopID();
        }
        if (ImGuiMultiSelectIO* io = ImGui::EndMultiSelect()) {
            LayerSelectionApply(canvas, rows, *io);
        }
    }
    ImGui::EndChild();
    if (drop.layer != kLayerInvalid) {
        if (const Layer height = LayerDropHeight(canvas, drop.layer, drop.target); height != kLayerInvalid) {
            if (const auto res = LayerMove(canvas.id, drop.layer, height); !res) {
                MIDORI_LOG_TRACE("layers window: move: {}", ErrorMessage(res.error()));
            }
        }
    }

    if (ImGui::Button("+ Layer")) {
        AppLayerNew(window, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Folder")) {
        AppLayerNew(window, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        AppLayerDelete(window);
    }
    ImGui::SameLine();
    if (ImGui::Button("Up")) {
        AppLayerMove(window, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Down")) {
        AppLayerMove(window, false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Merge")) {
        AppLayerMerge(window);
    }
    ImGui::End();
}

// What applies to the whole app and is kept between runs, one section per topic.
void SettingsWindow(Window window) noexcept {
    ZoneScopedN("SettingsWindow");
    ImGui::SetNextWindowSize(ImVec2(360.0f, 200.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &gUiState.settingsShown[window])) {
        ImGui::End();
        return;
    }
    ImGui::SeparatorText("Display");
    constexpr std::array<const char*, std::to_underlying(CanvasFilter::Count_)> kFilterNames = {
        "Nearest", "Bilinear", "Bicubic (Catmull-Rom)", "Mitchell", "Lanczos 2", "Lanczos 3",
    };
    int filter = std::to_underlying(gAppState.canvasFilter);
    if (ImGui::Combo("Canvas filter", &filter, kFilterNames.data(), static_cast<int>(kFilterNames.size()))) {
        gAppState.canvasFilter = static_cast<CanvasFilter>(filter);
    }
    ImGui::SetItemTooltip("How the canvas is drawn between its pixels: wider filters stay sharp when rotated or zoomed "
                          "between steps, Nearest keeps hard pixels.");
    ImGui::End();
}

struct HelpEntry {
    const char* keys;
    const char* action;
};

constexpr std::array kHelpPaint = {
    HelpEntry{.keys = "Left drag", .action = "Paint with the brush"},
    HelpEntry{.keys = "E", .action = "Switch to the eraser"},
};
constexpr std::array kHelpErase = {
    HelpEntry{.keys = "Left drag", .action = "Erase with the brush; its color picks the channels removed"},
    HelpEntry{.keys = "B", .action = "Switch to the paint brush"},
};
constexpr std::array kHelpLayers = {
    HelpEntry{.keys = "Ctrl+Shift+N", .action = "New layer above the active one"},
    HelpEntry{.keys = "Ctrl+G", .action = "New folder above the active layer"},
    HelpEntry{.keys = "Ctrl+E", .action = "Merge the active layer down, or a folder into one layer"},
    HelpEntry{.keys = "Page Up, Page Down", .action = "Select the layer above, below"},
    HelpEntry{.keys = "Ctrl+Page Up, Ctrl+Page Down", .action = "Move the active layer up, down"},
    HelpEntry{.keys = "Ctrl+,", .action = "Hide or show the selected layers"},
    HelpEntry{.keys = "Alt+Page Up, Alt+Page Down", .action = "Next, previous blend mode"},
    HelpEntry{.keys = "O", .action = "Change the active layer's opacity, like Shift+F"},
};
constexpr std::array kHelpBrush = {
    HelpEntry{.keys = "F", .action = "Change the brush radius"},
    HelpEntry{.keys = "Shift+F", .action = "Change the brush opacity"},
    HelpEntry{.keys = "Ctrl+F", .action = "Change the brush hardness"},
    HelpEntry{.keys = "Space", .action = "Hold to move the view"},
    HelpEntry{.keys = "Space+R", .action = "Reset zoom and rotation"},
    HelpEntry{.keys = "Space+Z", .action = "Back to the previous view"},
    HelpEntry{.keys = "Ctrl+Z", .action = "Undo the last stroke"},
    HelpEntry{.keys = "Ctrl+Shift+Z, Ctrl+Y", .action = "Redo"},
};
constexpr std::array kHelpStroke = {
    HelpEntry{.keys = "Release", .action = "Finish the stroke"},
    HelpEntry{.keys = "Ctrl+Z", .action = "Undo it once finished"},
};
constexpr std::array kHelpNavigate = {
    HelpEntry{.keys = "Space + drag", .action = "Pan"},
    HelpEntry{.keys = "Ctrl+Space + drag", .action = "Zoom, right or up zooms in"},
    HelpEntry{.keys = "Shift+Space + drag", .action = "Rotate around the center"},
    HelpEntry{.keys = "Alt", .action = "Snap the zoom to texel-friendly steps, the rotation to 15 degrees"},
    HelpEntry{.keys = "Space+R", .action = "Reset zoom and rotation"},
    HelpEntry{.keys = "Space+F", .action = "Flip the view horizontally"},
    HelpEntry{.keys = "Space+Shift+F", .action = "Flip the view vertically"},
    HelpEntry{.keys = "Space+Z", .action = "Back to the previous view"},
    HelpEntry{.keys = "Space+Shift+Z", .action = "Forward to the next view"},
};
constexpr std::array kHelpAdjust = {
    HelpEntry{.keys = "Move", .action = "Away from the center raises the value, toward it lowers it"},
    HelpEntry{.keys = "Shift", .action = "Change it 10 times slower"},
    HelpEntry{.keys = "Alt", .action = "Snap it to powers of two, a layer opacity to multiples of 5%"},
    HelpEntry{.keys = "Left click, Enter", .action = "Keep the value"},
    HelpEntry{.keys = "Right click, Escape", .action = "Restore the previous value"},
};
constexpr std::array kHelpGeneral = {
    HelpEntry{.keys = "Tab", .action = "Hide or show the UI"},
    HelpEntry{.keys = "F11", .action = "Toggle fullscreen"},
    HelpEntry{.keys = "Ctrl+N", .action = "New canvas"},
    HelpEntry{.keys = "Ctrl+O", .action = "Open a canvas"},
    HelpEntry{.keys = "Ctrl+S", .action = "Save"},
    HelpEntry{.keys = "Ctrl+Shift+S", .action = "Save as"},
    HelpEntry{.keys = "Ctrl+Q", .action = "Quit, closing every window"},
};

void HelpTable(const char* id, std::span<const HelpEntry> entries) noexcept {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        return;
    }
    for (const HelpEntry& entry : entries) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(entry.keys);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(entry.action);
    }
    ImGui::EndTable();
}

// Tips for what the user is doing right now: adjusting the brush, navigating, stroking, or else the current tool. The
// shortcuts that work everywhere follow, folded.
void HelpWindow(Window window, const ToolsState& tools) noexcept {
    ZoneScopedN("HelpWindow");
    ImGui::SetNextWindowSize(ImVec2(420.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Help", &gUiState.helpShown[window])) {
        ImGui::End();
        return;
    }
    const AppState& app = gAppState;
    constexpr std::array kAdjustNames = {"", "brush radius", "brush opacity", "brush hardness", "layer opacity"};
    if (app.brushAdjust.mode != BrushAdjustMode::None) {
        ImGui::Text("Adjusting the %s", kAdjustNames[std::to_underlying(app.brushAdjust.mode)]);
        HelpTable("adjust", kHelpAdjust);
    } else if (app.spaceDown || app.navigate.mode != NavigateMode::None) {
        ImGui::TextUnformatted("Moving the view");
        HelpTable("navigate", kHelpNavigate);
    } else if (app.stroke.canvas != kCanvasInvalid && !app.stroke.ended) {
        ImGui::TextUnformatted(app.stroke.mode == StrokeMode::Erase ? "Erasing" : "Painting");
        HelpTable("stroke", kHelpStroke);
    } else {
        ImGui::TextUnformatted(tools.tool == Tool::Erase ? "Eraser" : "Paint brush");
        HelpTable("tool", tools.tool == Tool::Erase ? std::span<const HelpEntry>(kHelpErase)
                                                    : std::span<const HelpEntry>(kHelpPaint));
        HelpTable("brush", kHelpBrush);
    }
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Everywhere")) {
        HelpTable("general", kHelpGeneral);
    }
    if (ImGui::CollapsingHeader("Layers")) {
        HelpTable("layers", kHelpLayers);
    }
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
    // Its windows open where the preferences keep them instead.
    ImGui::LoadIniSettingsFromMemory(gUiState.preferences.imguiSettings.data(),
                                     gUiState.preferences.imguiSettings.size());
    io.BackendPlatformName = "midori";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
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
    gUiState.helpShown[window] = gUiState.preferences.helpShown;
    gUiState.viewShown[window] = gUiState.preferences.viewShown;
    gUiState.toolShown[window] = gUiState.preferences.toolShown;
    gUiState.layersShown[window] = gUiState.preferences.layersShown;
    gUiState.settingsShown[window] = false;
    gUiState.layerFoldersClosed[window].clear();
    gUiState.layerRenaming[window] = -1;
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

std::optional<WindowCursor> UiCursor(Window window) noexcept {
    if (window >= kWindowMax || !gUiState.contexts[window]) {
        return std::nullopt;
    }
    ImGui::SetCurrentContext(gUiState.contexts[window]);
    if (!ImGui::GetIO().WantCaptureMouse) {
        return std::nullopt;
    }
    switch (ImGui::GetMouseCursor()) {
        case ImGuiMouseCursor_None:
            return WindowCursor::None;
        case ImGuiMouseCursor_TextInput:
            return WindowCursor::Text;
        case ImGuiMouseCursor_ResizeAll:
            return WindowCursor::Move;
        case ImGuiMouseCursor_ResizeNS:
            return WindowCursor::ResizeNS;
        case ImGuiMouseCursor_ResizeEW:
            return WindowCursor::ResizeEW;
        case ImGuiMouseCursor_ResizeNESW:
            return WindowCursor::ResizeNESW;
        case ImGuiMouseCursor_ResizeNWSE:
            return WindowCursor::ResizeNWSE;
        case ImGuiMouseCursor_Hand:
            return WindowCursor::Hand;
        case ImGuiMouseCursor_NotAllowed:
            return WindowCursor::NotAllowed;
        default:
            return WindowCursor::Arrow;
    }
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
    if (const auto view = std::ranges::find(gAppState.canvasViews, window, &CanvasViewState::window);
        viewport && view != gAppState.canvasViews.end() && view->id != kCanvasViewInvalid) {
        ViewInfoOverlay(*state, *viewport, gAppState.canvases[view->canvas]);
    }
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
            // The file name, its folder on the right.
            std::vector<std::filesystem::path>& recent = gAppState.recentFiles;
            if (ImGui::BeginMenu("Open Recent", !recent.empty())) {
                for (std::size_t i = 0; i < recent.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::MenuItem(PathUtf8(recent[i].filename()).c_str(),
                                        PathUtf8(recent[i].parent_path()).c_str())) {
                        command = UiCommand::CanvasOpenRecent;
                        gAppState.commandRecent = static_cast<std::uint8_t>(i);
                    }
                    ImGui::PopID();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear")) {
                    recent.clear();
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Ctrl+Q")) {
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
            ImGui::Separator();
            ImGui::MenuItem("Settings...", nullptr, &gUiState.settingsShown[window]);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window")) {
            if (ImGui::MenuItem("Fullscreen", "F11", state->displayMode == WindowDisplayMode::Fullscreened)) {
                command = UiCommand::ToggleFullscreen;
            }
            ImGui::MenuItem("Hide UI", "Tab", &gUiState.hidden[window]);
            ImGui::Separator();
            ImGui::MenuItem("View", nullptr, &gUiState.viewShown[window]);
            ImGui::MenuItem("Tool", nullptr, &gUiState.toolShown[window]);
            ImGui::MenuItem("Layers", nullptr, &gUiState.layersShown[window]);
            ImGui::MenuItem("Help", nullptr, &gUiState.helpShown[window]);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Debug")) {
            ImGui::MenuItem("Tile atlas", nullptr, &gUiState.tileAtlasShown[window]);
            ImGui::MenuItem("Tile cells on canvas", nullptr, &gUiState.tileCellsShown[window]);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    if (viewport && gUiState.viewShown[window]) {
        if (ImGui::Begin("View", &gUiState.viewShown[window])) {
            ImGui::Text("Zoom %.1f%%  Rotation %.1f deg", std::abs(viewport->zoomX) * 100.0f,
                        viewport->rot * 180.0f / std::numbers::pi_v<float>);
            // The pointer in canvas units, as painting places it.
            const float centerX = std::floor(static_cast<float>(state->width) * 0.5f);
            const float centerY = std::floor(static_cast<float>(state->height) * 0.5f);
            const Viewport under = ViewportMove(*viewport, -(gAppState.pointerX - centerX) / state->dpiScale,
                                                -(gAppState.pointerY - centerY) / state->dpiScale);
            ImGui::Text("Center %.1f, %.1f", viewport->posX, viewport->posY);
            ImGui::Text("Pointer %.1f, %.1f", under.posX, under.posY);
            if (ImGui::Button("Flip X (Space+F)")) {
                *viewport = ViewportZoom(*viewport, -1.0f, 1.0f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Flip Y (Space+Shift+F)")) {
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
    if (gUiState.toolShown[window]) {
        ToolWindow(window, tools);
    }
    if (gUiState.tileAtlasShown[window]) {
        TileAtlasWindow(window);
    }
    if (gUiState.layersShown[window]) {
        LayersWindow(window);
    }
    if (gUiState.settingsShown[window]) {
        SettingsWindow(window);
    }
    if (gUiState.helpShown[window]) {
        HelpWindow(window, tools);
    }
    if (gUiState.tileCellsShown[window]) {
        TileCellsOverlay(window, *state);
    }
    BrushAdjustOverlay(window, *state, tools);
    ImGui::Render();
    UiPreferences& preferences = gUiState.preferences;
    preferences.viewShown = gUiState.viewShown[window];
    preferences.toolShown = gUiState.toolShown[window];
    preferences.layersShown = gUiState.layersShown[window];
    preferences.helpShown = gUiState.helpShown[window];
    // With no ini file, ImGui only flags that its windows moved or resized.
    if (io.WantSaveIniSettings) {
        preferences.imguiSettings = ImGui::SaveIniSettingsToMemory();
        io.WantSaveIniSettings = false;
    }
    return command;
}

} // namespace Midori
