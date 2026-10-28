#include "midori/ui.hpp"
#include "midori/app.hpp"
#include "midori/graphics.hpp"
#include "midori/history.hpp"
#include "midori/job.hpp"
#include "midori/layer.hpp"
#include "midori/log.hpp"
#include "midori/text.hpp"
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
#include <fstream>
#include <ios>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <imgui.h>
#include <tracy/Tracy.hpp>

namespace Midori {

UiState gUiState{};

namespace {

constexpr float kFirstFrameSeconds = 1.0f / 60.0f;
// Past Dear ImGui's tooltip delay, so a tooltip shows under a pointer that stopped.
constexpr std::chrono::milliseconds kUiSettle{600};
constexpr float kFontSize = 15.0f; // DIPs

constexpr std::array<Text, std::to_underlying(BlendMode::Count_)> kBlendModeTexts = {
    Text::BlendNormal, Text::BlendMultiply, Text::BlendAdd, Text::BlendScreen, Text::BlendNormalOkLab, Text::Erase,
};
constexpr std::array<Text, std::to_underlying(BrushInput::Count_)> kBrushInputTexts = {
    Text::BrushInputNone,  Text::BrushInputPressure, Text::BrushInputTiltX,
    Text::BrushInputTiltY, Text::BrushInputSpeed,    Text::BrushInputRandom,
};
constexpr std::array<Text, std::to_underlying(SelectionOp::Count_)> kSelectionOpTexts = {
    Text::SelectionReplace,   Text::SelectionAdd,     Text::SelectionSubtract,
    Text::SelectionIntersect, Text::SelectionExclude,
};
// ToolShape's, from Box: the shapes of a Selection.
constexpr std::array<Text, 2> kSelectionShapeTexts = {Text::Box, Text::Lasso};
constexpr std::array<Text, 3> kShapeTexts = {Text::ShapePath, Text::Box, Text::Lasso};
constexpr std::array<Text, std::to_underlying(CanvasFilter::Count_)> kFilterTexts = {
    Text::FilterNearest,  Text::FilterBilinear, Text::FilterBicubic,
    Text::FilterMitchell, Text::FilterLanczos2, Text::FilterLanczos3,
};

// For Combo, which takes the names as one array.
template<std::size_t N>
[[nodiscard]] std::array<const char*, N> TextsGet(const std::array<Text, N>& texts) noexcept {
    std::array<const char*, N> names{};
    for (std::size_t i = 0; i < N; ++i) {
        names[i] = TextGet(texts[i]);
    }
    return names;
}

void ActionRequestPush(Window window, ActionId action, std::uint32_t arg = 0) noexcept {
    gAppState.actionRequests.push_back(ActionRequest{.arg = arg, .action = action, .window = window});
}

// The keys of the first binding of `action` with `arg`, "" when none.
[[nodiscard]] const char* BindingKeys(ActionId action, std::uint32_t arg) noexcept {
    const KeymapState& keymap = gAppState.keymap;
    for (std::size_t i = 0; i < keymap.bindings.size(); ++i) {
        if (keymap.bindings[i].action == action && keymap.bindings[i].arg == arg) {
            return keymap.keys[i].c_str();
        }
    }
    return "";
}

// Runs `action` from a menu: its keys on the right, greyed out while blocked, the reason shown on hover.
void MenuAction(Window window, Text label, ActionId action, bool selected = false, std::uint32_t arg = 0) noexcept {
    const Text blocked = ActionBlocked(window, action, arg);
    if (ImGui::MenuItem(TextLabel(label), BindingKeys(action, arg), selected, blocked == Text::Count_)) {
        ActionRequestPush(window, action, arg);
    }
    if (blocked != Text::Count_ && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", TextGet(blocked));
    }
}

// "Radius (F)###radius": the text with the keys of the action's first binding, its id the text's. On the stack, as it
// is made every frame.
[[nodiscard]] std::array<char, 256> ShortcutLabel(Text text, ActionId action) noexcept {
    std::array<char, 256> label{};
    const char* keys = KeymapKeys(gAppState.keymap, action);
    if (*keys == '\0') {
        std::format_to_n(label.data(), label.size() - 1, "{}", TextLabel(text));
    } else {
        std::format_to_n(label.data(), label.size() - 1, "{} ({})###{}", TextGet(text), keys,
                         kTexts[std::to_underlying(text)].key);
    }
    return label;
}

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

// The curve over [min, max], 0 at the bottom, its points marked when `points`.
void CurveDraw(ImDrawList* draw, ImVec2 min, ImVec2 max, const Curve& curve, bool points) noexcept {
    const float width = max.x - min.x;
    const float height = max.y - min.y;
    draw->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
    for (int i = 1; i < 4; ++i) {
        const float t = static_cast<float>(i) / 4.0f;
        draw->AddLine(ImVec2(min.x + (t * width), min.y), ImVec2(min.x + (t * width), max.y),
                      ImGui::GetColorU32(ImGuiCol_Border));
        draw->AddLine(ImVec2(min.x, min.y + (t * height)), ImVec2(max.x, min.y + (t * height)),
                      ImGui::GetColorU32(ImGuiCol_Border));
    }
    constexpr int kSegments = 48;
    std::array<ImVec2, kSegments + 1> line{};
    for (int i = 0; i <= kSegments; ++i) {
        const float x = static_cast<float>(i) / kSegments;
        line[static_cast<std::size_t>(i)] = ImVec2(min.x + (x * width), max.y - (CurveEval(curve, x) * height));
    }
    draw->AddPolyline(line.data(), static_cast<int>(line.size()), ImGui::GetColorU32(ImGuiCol_PlotLines),
                      std::max(1.0f, ImGui::GetFontSize() / 10.0f), ImDrawFlags_None);
    if (!points) {
        return;
    }
    for (std::uint32_t i = 0; i < curve.count; ++i) {
        draw->AddCircleFilled(ImVec2(min.x + (curve.points[i].x * width), max.y - (curve.points[i].y * height)),
                              ImGui::GetFontSize() * 0.25f, ImGui::GetColorU32(ImGuiCol_SliderGrabActive));
    }
}

// A click adds a point, a drag moves the one under it, a right-click removes it. The first and last points stay at
// x 0 and 1, and the others between their neighbours, so the curve stays valid.
void CurveEdit(Curve& curve) noexcept {
    constexpr float kGap = 0.01f;
    const float side = ImGui::GetFontSize() * 12.0f;
    ImGui::InvisibleButton("##edit", ImVec2(side, side));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const CurvePoint at{
        .x = std::clamp((mouse.x - min.x) / side, 0.0f, 1.0f),
        .y = std::clamp((max.y - mouse.y) / side, 0.0f, 1.0f),
    };
    int hovered = -1;
    float nearest = ImGui::GetFontSize() * 0.6f;
    for (std::uint32_t i = 0; i < curve.count; ++i) {
        const float distance =
            std::hypot(min.x + (curve.points[i].x * side) - mouse.x, max.y - (curve.points[i].y * side) - mouse.y);
        if (distance < nearest) {
            nearest = distance;
            hovered = static_cast<int>(i);
        }
    }
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID draggingId = ImGui::GetID("##dragging");
    int dragging = storage->GetInt(draggingId, -1);
    if (ImGui::IsItemActivated()) {
        dragging = hovered;
        if (dragging < 0 && curve.count < kCurvePointsMax) {
            std::uint32_t next = 1;
            while (next < curve.count - 1 && curve.points[next].x < at.x) {
                ++next;
            }
            if (at.x > curve.points[next - 1].x + kGap && at.x < curve.points[next].x - kGap) {
                std::copy_backward(curve.points.begin() + next, curve.points.begin() + curve.count,
                                   curve.points.begin() + curve.count + 1);
                curve.points[next] = at;
                ++curve.count;
                dragging = static_cast<int>(next);
            }
        }
    }
    if (ImGui::IsItemActive() && dragging >= 0) {
        const auto i = static_cast<std::uint32_t>(dragging);
        CurvePoint& point = curve.points[i];
        point.y = at.y;
        if (i == 0 || i == curve.count - 1) {
            point.x = i == 0 ? 0.0f : 1.0f;
        } else {
            point.x = std::clamp(at.x, curve.points[i - 1].x + kGap, curve.points[i + 1].x - kGap);
        }
    } else {
        dragging = -1;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hovered > 0
        && hovered < static_cast<int>(curve.count) - 1) {
        std::copy(curve.points.begin() + hovered + 1, curve.points.begin() + curve.count,
                  curve.points.begin() + hovered);
        --curve.count;
        dragging = -1;
    }
    storage->SetInt(draggingId, dragging);
    CurveDraw(ImGui::GetWindowDrawList(), min, max, curve, true);
    if (ImGui::Button(TextLabel(Text::CurveReset))) {
        curve = Curve{};
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + side);
    ImGui::TextDisabled("%s", TextGet(Text::CurveHelp));
    ImGui::PopTextWrapPos();
}

// The curve drawn as wide as `width`, a frame high; a click on it opens its editor.
void CurveButton(Curve& curve, float width) noexcept {
    ImGui::InvisibleButton("##curve", ImVec2(width, ImGui::GetFrameHeight()));
    if (ImGui::IsItemClicked()) {
        ImGui::OpenPopup("##curveEdit");
    }
    CurveDraw(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), curve, false);
    if (ImGui::BeginPopup("##curveEdit")) {
        CurveEdit(curve);
        ImGui::EndPopup();
    }
}

void BrushEdit(Window window, BrushState& brush, const ToolShader& shader) noexcept {
    ZoneScopedN("BrushEdit");
    // The brush's params, named by its source and not translated: each with the input that drives it, and with one,
    // the curve from the input to the share of the value. Then its curves and its textures.
    const MdslBrush& source = shader.brush;
    const auto inputNames = TextsGet(kBrushInputTexts);
    float inputWidth = 0.0f;
    for (const char* name : inputNames) {
        inputWidth = std::max(inputWidth, ImGui::CalcTextSize(name).x);
    }
    inputWidth += (ImGui::GetStyle().FramePadding.x * 2.0f) + ImGui::GetFrameHeight();
    if (!ImGui::BeginTable("##params", 4, ImGuiTableFlags_SizingStretchProp)) {
        return;
    }
    ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed);
    ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("##input", ImGuiTableColumnFlags_WidthFixed, inputWidth);
    ImGui::TableSetupColumn("##inputCurve", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    const auto name = [](const std::string& text) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text.c_str());
        ImGui::TableNextColumn();
    };
    for (std::size_t i = 0; i < source.params.size(); ++i) {
        const MdslParam& param = source.params[i];
        BrushParam& value = brush.params[i];
        ImGui::PushID(static_cast<int>(i));
        name(param.name);
        // A range over two decades, like a size, gets a logarithmic slider so its small values stay reachable.
        const bool logarithmic = param.min > 0.0f && param.max >= 100.0f * param.min;
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::SliderScalarN("##value", ImGuiDataType_Float, value.value.data(),
                             static_cast<int>(MdslComponents(param.type)), &param.min, &param.max, "%.3g",
                             logarithmic ? ImGuiSliderFlags_Logarithmic : ImGuiSliderFlags_None);
        ImGui::TableNextColumn();
        int input = std::to_underlying(value.input);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##input", &input, inputNames.data(), static_cast<int>(inputNames.size()))) {
            value.input = static_cast<BrushInput>(input);
        }
        ImGui::SetItemTooltip("%s", TextGet(Text::BrushInput));
        ImGui::TableNextColumn();
        if (value.input != BrushInput::None) {
            CurveButton(value.curve, ImGui::GetContentRegionAvail().x);
        }
        ImGui::PopID();
    }
    for (std::size_t i = 0; i < source.curves.size(); ++i) {
        ImGui::PushID(static_cast<int>(kMdslParamFloatsMax + i));
        name(source.curves[i].name);
        CurveButton(brush.curves[i], ImGui::GetContentRegionAvail().x);
        ImGui::PopID();
    }
    for (std::size_t i = 0; i < source.textures.size(); ++i) {
        const BrushTextureState& texture = shader.textures[i];
        ImGui::PushID(static_cast<int>(kMdslParamFloatsMax + kMdslCurvesMax + i));
        name(source.textures[i].name);
        const std::string file =
            texture.path.empty() ? std::string(TextGet(Text::BrushTextureNone)) : PathUtf8(texture.path.filename());
        if (ImGui::Button(std::format("{}###texture", file).c_str(), ImVec2(-FLT_MIN, 0.0f))) {
            ActionRequestPush(window, ActionId::BrushTexture, static_cast<std::uint32_t>(i));
        }
        if (!texture.error.empty()) {
            ImGui::TableNextColumn();
            ImGui::TextColored(ImVec4(0.9f, 0.31f, 0.27f, 1.0f), "(!)");
            ImGui::SetItemTooltip("%s", texture.error.c_str());
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// A square button with a circular arrow, for reading a file again.
bool ReloadButton(const char* id) noexcept {
    const float size = ImGui::GetFrameHeight();
    const bool pressed = ImGui::Button(id, ImVec2(size, size));
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 center(min.x + (size * 0.5f), min.y + (size * 0.5f));
    const float radius = size * 0.26f;
    const float head = size * 0.14f;
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_Text);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // Clockwise from the right to the top, the arrow pointing on along the circle.
    draw->PathArcTo(center, radius, 0.0f, 1.5f * std::numbers::pi_v<float>, 16);
    draw->PathStroke(color, std::max(1.0f, size * 0.08f), ImDrawFlags_None);
    const ImVec2 end(center.x, center.y - radius);
    draw->AddTriangleFilled(ImVec2(end.x + head, end.y), ImVec2(end.x - (head * 0.5f), end.y - head),
                            ImVec2(end.x - (head * 0.5f), end.y + head), color);
    return pressed;
}

// Where the tool's shader comes from, and why it, or its last change, does not paint yet.
void BrushSourceEdit(Window window, const ToolShader& source) noexcept {
    const std::string name =
        source.path.empty() ? std::string(TextGet(Text::BrushBuiltIn)) : PathUtf8(source.path.filename());
    // The example brushes, picked like any brush file.
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##source", name.c_str())) {
        const std::vector<std::filesystem::path>& examples = gAppState.brushExamples;
        for (std::size_t i = 0; i < examples.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(PathUtf8(examples[i].filename()).c_str(), examples[i] == source.path)) {
                ActionRequestPush(window, ActionId::BrushExample, static_cast<std::uint32_t>(i));
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    if (ImGui::Button(TextLabel(Text::BrushOpen))) {
        ActionRequestPush(window, ActionId::BrushOpen);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(source.path.empty() || gAppState.stroke.canvas != kCanvasInvalid);
    if (ImGui::Button(TextLabel(Text::BrushBuiltIn))) {
        ActionRequestPush(window, ActionId::BrushBuiltIn);
    }
    ImGui::EndDisabled();
#if !defined(MIDORI_DEBUG)
    // Debug builds read it again whenever its file changes.
    ImGui::SameLine();
    ImGui::BeginDisabled(ActionBlocked(window, ActionId::BrushReload, 0) != Text::Count_);
    if (ReloadButton("##reload")) {
        ActionRequestPush(window, ActionId::BrushReload);
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", TextGet(Text::BrushReload));
#endif
    if (source.compiling || source.ready) {
        ImGui::TextDisabled("%s", TextGet(Text::BrushCompiling));
    }
    if (!source.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 80, 70, 255));
        ImGui::TextWrapped("%s", source.error.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Separator();
}

void ToolWindow(Window window, ToolsState& tools) noexcept {
    ZoneScopedN("ToolWindow");
    if (ImGui::Begin(TextLabel(Text::Tool), &gUiState.toolShown[window])) {
        ToolState& tool = ToolCurrent(tools);
        ImGui::TextUnformatted(tool.name.data());
        if (ImGui::RadioButton(TextLabel(Text::Layer), tool.target == ToolTarget::Layer)) {
            tool.target = ToolTarget::Layer;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton(TextLabel(Text::Selection), tool.target == ToolTarget::Selection)) {
            tool.target = ToolTarget::Selection;
            if (tool.shape == ToolShape::Path) {
                tool.shape = ToolShape::Box;
            }
        }
        ImGui::Separator();
        if (tool.target == ToolTarget::Layer) {
            const auto shapeNames = TextsGet(kShapeTexts);
            int shape = std::to_underlying(tool.shape);
            if (ImGui::Combo(TextLabel(Text::Shape), &shape, shapeNames.data(), static_cast<int>(shapeNames.size()))) {
                tool.shape = static_cast<ToolShape>(shape);
            }
            ImGui::ColorEdit4(TextLabel(Text::Color), &tools.color.r);
            int blendMode = std::to_underlying(tool.blendMode);
            const auto blendModeNames = TextsGet(kBlendModeTexts);
            if (ImGui::Combo(TextLabel(Text::Blend), &blendMode, blendModeNames.data(),
                             static_cast<int>(blendModeNames.size()))) {
                tool.blendMode = static_cast<BlendMode>(blendMode);
            }
            BrushSourceEdit(window, tools.shaders[tool.shader]);
            BrushEdit(window, tool.brush, tools.shaders[tool.shader]);
        } else {
            const auto shapeNames = TextsGet(kSelectionShapeTexts);
            int shape = std::to_underlying(tool.shape) - std::to_underlying(ToolShape::Box);
            if (ImGui::Combo(TextLabel(Text::Shape), &shape, shapeNames.data(), static_cast<int>(shapeNames.size()))) {
                tool.shape = static_cast<ToolShape>(shape + std::to_underlying(ToolShape::Box));
            }
            const auto opNames = TextsGet(kSelectionOpTexts);
            int op = std::to_underlying(tool.selectionOp);
            if (ImGui::Combo(TextLabel(Text::SelectionOpLabel), &op, opNames.data(),
                             static_cast<int>(opNames.size()))) {
                ActionRequestPush(window, ActionId::SelectionOp, static_cast<std::uint32_t>(op));
            }
        }
    }
    ImGui::End();
}

// keybinds.json's form of the slot's keys, to edit: Enter binds the slot to them in place of its keys, empty unbinds
// it.
void SlotKeysEdit(Window window, std::uint32_t slot) noexcept {
    std::array<char, 64>& text = gUiState.slotKeys[window];
    if (ImGui::IsWindowAppearing()) {
        text = {};
        gUiState.slotKeysInvalid[window] = false;
        const KeymapState& keymap = gAppState.keymap;
        const auto binding = std::ranges::find_if(
            keymap.bindings, [&](const KeyBinding& b) { return b.action == ActionId::ToolbarSlot && b.arg == slot; });
        if (binding != keymap.bindings.end()) {
            const std::string keys = KeyBindingKeys(*binding);
            std::copy_n(keys.begin(), std::min(keys.size(), text.size() - 1), text.begin());
        }
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
    if (ImGui::InputText(TextLabel(Text::Keys), text.data(), text.size(), ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (KeymapBind(gAppState.keymap, text.data(), ActionId::ToolbarSlot, slot, ActionContext::Canvas)) {
            ImGui::CloseCurrentPopup();
        } else {
            gUiState.slotKeysInvalid[window] = true;
        }
    }
    if (gUiState.slotKeysInvalid[window]) {
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(230, 80, 70, 255));
        ImGui::TextUnformatted(TextGet(Text::KeysInvalid));
        ImGui::PopStyleColor();
    }
    ImGui::TextDisabled("%s", TextGet(Text::KeysHelp));
}

// A button per slot, showing the tool it picks, its corner marked when it is a drawer. Right-click one to set its keys
// or remove it, or elsewhere to make the current tool a new slot.
void ToolbarWindow(Window window, ToolsState& tools) noexcept {
    ZoneScopedN("ToolbarWindow");
    if (ImGui::Begin(TextLabel(Text::Toolbar), &gUiState.toolbarShown[window])) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        float lastRight = 0.0f;
        std::uint32_t removed = 0;
        for (std::size_t i = 0; i < tools.slots.size(); ++i) {
            const ToolbarSlot& slot = tools.slots[i];
            const bool active = tools.slot == slot.id;
            const ToolState* tool =
                ToolFind(tools, active && std::ranges::contains(slot.tools, tools.current) ? tools.current : slot.last);
            const char* name = tool ? tool->name.data() : "";
            const float width = ImGui::CalcTextSize(name, nullptr, true).x + (style.FramePadding.x * 2.0f);
            if (i > 0 && lastRight + style.ItemSpacing.x + width <= right) {
                ImGui::SameLine();
            }
            ImGui::PushID(static_cast<int>(slot.id));
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
            }
            if (ImGui::Button(name)) {
                ActionRequestPush(window, ActionId::ToolbarSlot, slot.id);
            }
            if (active) {
                ImGui::PopStyleColor();
            }
            const ImVec2 max = ImGui::GetItemRectMax();
            lastRight = max.x;
            if (slot.tools.size() > 1) {
                const float size = std::floor(ImGui::GetFontSize() * 0.4f);
                ImGui::GetWindowDrawList()->AddTriangleFilled(
                    ImVec2(max.x - size, max.y), max, ImVec2(max.x, max.y - size), ImGui::GetColorU32(ImGuiCol_Text));
            }
            if (const char* keys = BindingKeys(ActionId::ToolbarSlot, slot.id);
                *keys != '\0' && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
                ImGui::SetTooltip("%s", keys);
            }
            if (ImGui::BeginPopupContextItem("slot")) {
                SlotKeysEdit(window, slot.id);
                ImGui::Separator();
                if (ImGui::MenuItem(TextLabel(Text::SlotRemove))) {
                    removed = slot.id;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (removed != 0) {
            std::erase_if(tools.slots, [&](const ToolbarSlot& slot) { return slot.id == removed; });
            if (tools.slot == removed) {
                tools.slot = 0;
            }
        }
        if (ImGui::BeginPopupContextWindow("toolbar",
                                           ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::MenuItem(TextLabel(Text::SlotNew))) {
                ToolbarSlotAdd(tools, tools.current);
            }
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

// The active slot's tools: a click picks one, a double-click renames it. Right-click one to rename, duplicate, remove
// or delete it, or elsewhere to add one of the library's.
void ToolsWindow(Window window, ToolsState& tools) noexcept {
    ZoneScopedN("ToolsWindow");
    if (ImGui::Begin(TextLabel(Text::Tools), &gUiState.toolsShown[window])) {
        ToolbarSlot* slot = ToolbarSlotFind(tools, tools.slot);
        if (!slot) {
            ImGui::TextDisabled("%s", TextGet(Text::NoSlot));
        } else {
            std::uint32_t& renaming = gUiState.toolRenaming[window];
            std::uint32_t removed = 0;
            for (const std::uint32_t id : slot->tools) {
                ToolState* tool = ToolFind(tools, id);
                if (!tool) {
                    continue;
                }
                ImGui::PushID(static_cast<int>(id));
                if (renaming == id) {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::IsWindowAppearing() || !ImGui::IsAnyItemActive()) {
                        ImGui::SetKeyboardFocusHere();
                    }
                    const bool entered =
                        ImGui::InputText("##rename", tool->name.data(), tool->name.size(),
                                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                    if (entered || ImGui::IsItemDeactivated()) {
                        renaming = 0;
                    }
                } else {
                    if (ImGui::Selectable(tool->name.data(), id == tools.current,
                                          ImGuiSelectableFlags_AllowDoubleClick)) {
                        ActionRequestPush(window, ActionId::ToolSelect, id);
                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                            renaming = id;
                        }
                    }
                    if (ImGui::BeginPopupContextItem("tool")) {
                        if (ImGui::MenuItem(TextLabel(Text::ToolRename))) {
                            renaming = id;
                        }
                        MenuAction(window, Text::ToolDuplicate, ActionId::ToolDuplicate, false, id);
                        if (ImGui::MenuItem(TextLabel(Text::ToolRemove), nullptr, false, slot->tools.size() > 1)) {
                            removed = id;
                        }
                        MenuAction(window, Text::ToolDelete, ActionId::ToolDelete, false, id);
                        ImGui::EndPopup();
                    }
                }
                ImGui::PopID();
            }
            if (removed != 0) {
                std::erase(slot->tools, removed);
                if (slot->last == removed) {
                    slot->last = slot->tools.front();
                }
            }
            if (ImGui::BeginPopupContextWindow("tools",
                                               ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
                if (ImGui::BeginMenu(TextLabel(Text::ToolAdd))) {
                    for (const ToolState& tool : tools.library) {
                        if (std::ranges::contains(slot->tools, tool.id)) {
                            continue;
                        }
                        ImGui::PushID(static_cast<int>(tool.id));
                        if (ImGui::MenuItem(tool.name.data())) {
                            slot->tools.push_back(tool.id);
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndMenu();
                }
                ImGui::EndPopup();
            }
        }
    }
    ImGui::End();
}

// Canvas to view DIPs as in the renderer, then to client pixels around the floored center.
[[nodiscard]] ImVec2 CanvasToScreen(const Viewport& viewport, const WindowState& windowState, double x,
                                    double y) noexcept {
    const float c = std::cos(viewport.rot);
    const float s = std::sin(viewport.rot);
    const float centerX = std::floor(static_cast<float>(windowState.width) * 0.5f);
    const float centerY = std::floor(static_cast<float>(windowState.height) * 0.5f);
    const float scale = windowState.dpiScale;
    const auto dx = static_cast<float>(x - viewport.posX);
    const auto dy = static_cast<float>(y - viewport.posY);
    return ImVec2(centerX + (((dx * c) - (dy * s)) * viewport.zoomX * scale),
                  centerY + (((dx * s) + (dy * c)) * viewport.zoomY * scale));
}

// Outlines every tile of the window's canvas view on screen, labeled with its atlas cell and grid position. Drawn
// behind the UI windows, over the canvas.
void TileCellsOverlay(Window window, const WindowState& windowState) noexcept {
    ZoneScopedN("TileCellsOverlay");
    const auto view = std::ranges::find(gAppState.canvasViews, window, &CanvasViewState::window);
    if (view == gAppState.canvasViews.end() || view->id == kCanvasViewInvalid) {
        return;
    }
    const Viewport& viewport = view->viewport;
    const auto toScreen = [&](double x, double y) {
        return CanvasToScreen(viewport, windowState, x, y);
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

// The box of the selection's transform and its handles over its view, behind the UI windows.
void TransformOverlay(Window window, const WindowState& windowState) noexcept {
    const auto box = AppTransformBox(window);
    if (!box) {
        return;
    }
    ZoneScopedN("TransformOverlay");
    const Viewport& viewport = gAppState.canvasViews[box->view].viewport;
    const SelectionBounds& source = box->source;
    const auto at = [&](double handleX, double handleY) {
        const double x = ((source.minX + source.maxX) * 0.5) + (handleX * (source.maxX - source.minX) * 0.5);
        const double y = ((source.minY + source.maxY) * 0.5) + (handleY * (source.maxY - source.minY) * 0.5);
        const StrokeTransformPoint to = StrokeTransformApply(box->transform, x, y);
        return CanvasToScreen(viewport, windowState, to.x, to.y);
    };
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    // An accent rather than white or black, so it shows over light and dark paint alike.
    const ImU32 accent = ImGui::GetColorU32(ImGuiCol_CheckMark);
    drawList->AddQuad(at(-1.0, -1.0), at(1.0, -1.0), at(1.0, 1.0), at(-1.0, 1.0), accent, 1.0f);
    const float half = std::round(4.0f * windowState.dpiScale);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0) {
                continue;
            }
            const ImVec2 handle = at(x, y);
            const ImVec2 min(handle.x - half, handle.y - half);
            const ImVec2 max(handle.x + half, handle.y + half);
            drawList->AddRectFilled(min, max, IM_COL32_WHITE);
            drawList->AddRect(min, max, accent, 0.0f, 1.0f);
        }
    }
}

// In the bottom right corner, from the bottom line up: the view's position, zoom and rotation, the active layer's name,
// blend mode and opacity with how many more are selected, then the current tool with its blend mode or selection
// operation. Faint so it never draws the eye, behind the UI windows and shown with them hidden too. Without a canvas,
// only the tool.
void StateOverlay(const WindowState& windowState, const Viewport* viewport, const CanvasState* canvas,
                  const ToolsState& tools) noexcept {
    ZoneScopedN("StateOverlay");
    std::array<std::array<char, 128>, 3> lines{};
    if (viewport) {
        const char* flip = "";
        if (viewport->zoomX < 0.0f && viewport->zoomY < 0.0f) {
            flip = " flip XY";
        } else if (viewport->zoomX < 0.0f) {
            flip = " flip X";
        } else if (viewport->zoomY < 0.0f) {
            flip = " flip Y";
        }
        // The texel under the view center.
        std::format_to_n(lines[0].data(), lines[0].size() - 1, "{}, {} {:.1f}% {:.1f}\u00B0{}",
                         static_cast<std::int64_t>(std::floor(viewport->posX)),
                         static_cast<std::int64_t>(std::floor(viewport->posY)), std::abs(viewport->zoomX) * 100.0f,
                         viewport->rot * 180.0f / std::numbers::pi_v<float>, flip);
    }
    if (const Layer active = canvas ? canvas->activeLayer : kLayerInvalid; active != kLayerInvalid) {
        const LayerState& layer = canvas->layers[active];
        const std::size_t selected = LayersSelected(*canvas).size();
        const char* name = canvas->layerNames[active].data();
        const char* blendMode = TextGet(kBlendModeTexts[std::to_underlying(layer.blendMode)]);
        const float opacity = layer.opacity * 100.0f;
        if (selected > 1) {
            std::format_to_n(lines[1].data(), lines[1].size() - 1, "{} (+{}) {} {:.0f}%", name, selected - 1,
                             blendMode, opacity);
        } else {
            std::format_to_n(lines[1].data(), lines[1].size() - 1, "{} {} {:.0f}%", name, blendMode, opacity);
        }
    }
    const ToolState& tool = ToolCurrent(tools);
    const char* mode = tool.target == ToolTarget::Layer
                         ? TextGet(kBlendModeTexts[std::to_underlying(tool.blendMode)])
                         : TextGet(kSelectionOpTexts[std::to_underlying(tool.selectionOp)]);
    std::format_to_n(lines[2].data(), lines[2].size() - 1, "{} {}", tool.name.data(), mode);

    ImFont* font = ImGui::GetFont();
    const float size = ImGui::GetFontSize() * 1.15f;
    const float margin = 10.0f * windowState.dpiScale;
    const float shadow = std::max(1.0f, std::round(windowState.dpiScale));
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    float bottom = static_cast<float>(windowState.height) - margin;
    for (const std::array<char, 128>& line : lines) {
        if (line[0] == '\0') {
            continue;
        }
        const ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0.0f, line.data());
        const ImVec2 pos(static_cast<float>(windowState.width) - extent.x - margin, bottom - extent.y);
        drawList->AddText(font, size, ImVec2(pos.x + shadow, pos.y + shadow), IM_COL32(0, 0, 0, 100), line.data());
        drawList->AddText(font, size, pos, IM_COL32(255, 255, 255, 170), line.data());
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
    const BrushState& brush = ToolCurrent(tools).brush;
    const float scale = windowState.dpiScale;
    const ImVec2 center(adjust.centerX, adjust.centerY);
    constexpr ImU32 kStrong = IM_COL32(40, 130, 230, 255);
    constexpr ImU32 kFaint = IM_COL32(40, 130, 230, 120);
    ImDrawList* drawList = ImGui::GetForegroundDrawList();

    const int param = BrushAdjustParam(ToolSource(tools, ToolCurrent(tools)).params, adjust.mode);
    float value = brush.params[static_cast<std::size_t>(std::max(param, 0))].value[0];
    if (adjust.mode == BrushAdjustMode::Radius) {
        const float radius = value * std::abs(gAppState.canvasViews[adjust.view].viewport.zoomX) * scale;
        drawList->AddCircle(center, radius, kStrong, 0, 2.0f * scale);
    } else {
        const float range = kBrushAdjustRangeDips * scale;
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
    constexpr std::array kNames = {Text::AdjustRadius, Text::AdjustRadius, Text::AdjustOpacity, Text::AdjustHardness,
                                   Text::AdjustLayerOpacity};
    std::array<char, 16> text{};
    // A radius is whole from 1 up, see BrushAdjustApply.
    int decimals = 3;
    if (adjust.mode == BrushAdjustMode::Radius) {
        decimals = value >= 1.0f ? 0 : 1;
    }
    std::format_to_n(text.data(), text.size() - 1, "{:.{}f}", value, decimals);
    // The value at the center, the property it belongs to right above.
    const float lineHeight = ImGui::GetTextLineHeight();
    label(TextGet(kNames[std::to_underlying(adjust.mode)]), center.x, center.y - (lineHeight * 1.5f));
    label(text.data(), center.x, center.y - (lineHeight * 0.5f));
}

// What the user never sees: memory, jobs, transfers, the atlas, history and each canvas's tiles and queues. Plain
// English, as a tool for whoever works on the app.
void StatisticsWindow(Window window) noexcept {
    ZoneScopedN("StatisticsWindow");
    ImGui::SetNextWindowSize(ImVec2(420.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(TextLabel(Text::Statistics), &gUiState.statisticsShown[window])) {
        ImGui::End();
        return;
    }
    const AppState& app = gAppState;
    constexpr double kMiB = 1024.0 * 1024.0;
    const auto mib = [](std::uint64_t bytes) {
        return static_cast<double>(bytes) / kMiB;
    };
    // A label and its value, in two columns.
    const auto row = [](const char* label, const char* format, auto... args) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(label);
        ImGui::TableNextColumn();
        ImGui::Text(format, args...);
    };
    const auto table = [](const char* id) {
        return ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp);
    };

    if (ImGui::CollapsingHeader("Memory", ImGuiTreeNodeFlags_DefaultOpen) && table("memory")) {
        const PlatformMemory process = PlatformMemoryGet();
        const GraphicsMemory gpu = GraphicsMemoryGet();
        row("Process private", "%.1f MiB", mib(process.privateBytes));
        row("Working set", "%.1f MiB (peak %.1f)", mib(process.workingSet), mib(process.peakWorkingSet));
        row("GPU local", "%.1f of %.1f MiB", mib(gpu.localUsage), mib(gpu.localBudget));
        row("GPU non-local", "%.1f of %.1f MiB", mib(gpu.nonLocalUsage), mib(gpu.nonLocalBudget));
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Frames", ImGuiTreeNodeFlags_DefaultOpen) && table("frames")) {
        const ImGuiIO& io = ImGui::GetIO();
        row("Frame", "%u", app.frame);
        row("This window", "%.1f ms, %.0f fps", io.DeltaTime * 1000.0f, io.Framerate);
        row("Busy", "%s", app.busy ? "yes: every window presents" : "no: presents after input");
        row("UI wants frames", "%s", UiWantsFrame(window) ? "yes" : "no");
        row("UI vertices", "%d in %d windows", io.MetricsRenderVertices, io.MetricsRenderWindows);
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Jobs and transfers", ImGuiTreeNodeFlags_DefaultOpen) && table("jobs")) {
        const JobsCount jobs = JobsCountGet();
        row("Jobs pending", "%u on %u threads", jobs.pending, jobs.threads);
        row("Queued", "%u reads, %u writes, %u finished", jobs.reads, jobs.writes, jobs.finished);
        constexpr std::array kStates = {"free", "job", "ready", "record", "gpu"};
        for (const auto& [name, slots] : {std::pair{"Upload slots", &app.transfers.uploads},
                                          std::pair{"Download slots", &app.transfers.downloads}}) {
            std::array<int, kStates.size()> counts{};
            for (const TileSlot& slot : *slots) {
                ++counts[std::to_underlying(slot.state)];
            }
            row(name, "%d %s, %d %s, %d %s, %d %s, %d %s", counts[0], kStates[0], counts[1], kStates[1], counts[2],
                kStates[2], counts[3], kStates[3], counts[4], kStates[4]);
        }
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Atlas and history", ImGuiTreeNodeFlags_DefaultOpen) && table("atlas")) {
        const TileAtlasState& atlas = app.atlas;
        std::array<int, kTileMipMax> cells{};
        int blocks = 0;
        for (std::size_t page = 0; page < std::size_t{atlas.count} * kTileAtlasPages; ++page) {
            blocks += std::popcount(atlas.used[page]);
            for (std::uint8_t b = 0; b < kTileAtlasBlocks;) {
                if (((atlas.used[page] >> b) & 1) == 0) {
                    ++b;
                    continue;
                }
                const std::uint8_t level = atlas.owners[page][b].level;
                ++cells[level];
                b = static_cast<std::uint8_t>(b + TileAtlasLevelBlocks(level));
            }
        }
        row("Atlases", "%d of %d", static_cast<int>(atlas.count), static_cast<int>(kTileAtlasMax));
        row("Blocks used", "%d of %d", blocks, static_cast<int>(atlas.count) * kTileAtlasPages * kTileAtlasBlocks);
        row("Cells by level", "%d, %d, %d, %d", cells[0], cells[1], cells[2], cells[3]);
        const HistoryPoolState& pool = app.historyPool;
        const auto slotsUsed = static_cast<std::uint32_t>(kHistorySlotsMax - pool.freeSlots.size());
        row("History slots", "%u of %u (%.1f MiB)", slotsUsed, kHistorySlotsMax,
            mib(std::uint64_t{slotsUsed} * kHistoryBlockBytes));
        row("History ops", "%d waiting, %llu records pushed", static_cast<int>(pool.ops.size()),
            static_cast<unsigned long long>(pool.sequence));
        ImGui::EndTable();
    }

    for (const CanvasState& canvas : app.canvases) {
        if (canvas.id == kCanvasInvalid) {
            continue;
        }
        std::array<char, 64> title{};
        std::format_to_n(title.data(), title.size() - 1, "Canvas {}{}###canvas{}", canvas.id,
                         canvas.modified ? " (modified)" : "", canvas.id);
        if (!ImGui::CollapsingHeader(title.data()) || !table("canvas")) {
            continue;
        }
        std::array<int, kTileMipMax> levels{};
        int loaded = 0;
        int modified = 0;
        int cachedDisk = 0;
        int saved = 0;
        int composite = 0;
        int dirty = 0;
        int moving = 0;
        int evictable = 0;
        for (const TileState& tile : canvas.tiles) {
            ++levels[TileKeyMip(tile.key)];
            loaded += HasAnyFlag(tile.flags, TileFlags::Loaded) ? 1 : 0;
            modified += HasAnyFlag(tile.flags, TileFlags::Modified) ? 1 : 0;
            cachedDisk += HasAnyFlag(tile.flags, TileFlags::CachedDisk) ? 1 : 0;
            saved += HasAnyFlag(tile.flags, TileFlags::Saved) ? 1 : 0;
            composite += HasAnyFlag(tile.flags, TileFlags::Composite) ? 1 : 0;
            dirty += HasAnyFlag(tile.flags, TileFlags::Dirty) ? 1 : 0;
            moving += tile.transfer != TileTransfer::None ? 1 : 0;
            evictable += TileEvictable(canvas.id, tile) ? 1 : 0;
        }
        row("Tiles", "%d (by level %d, %d, %d, %d)", static_cast<int>(canvas.tiles.size()), levels[0], levels[1],
            levels[2], levels[3]);
        row("Loaded", "%d, %d in transfer, %d evictable", loaded, moving, evictable);
        row("Modified", "%d, %d in the disk cache, %d saved", modified, cachedDisk, saved);
        row("Composites", "%d, %d dirty", composite, dirty);
        row("Queues", "%d creates, %d mip builds, %d composites", static_cast<int>(canvas.creates.size()),
            static_cast<int>(canvas.mipBuilds.size()), static_cast<int>(canvas.composites.size()));
        row("Wanted", "%d composites, %d deletes to save", static_cast<int>(canvas.compositeWants.size()),
            static_cast<int>(canvas.tileDeletes.size()));
        row("Layers", "%d in the order, %d ids free", static_cast<int>(canvas.layersOrder.size()),
            static_cast<int>(canvas.freeLayers.size()));
        const HistoryState& history = canvas.history;
        row("History", "%u of %d records, %d pending", history.current, static_cast<int>(history.records.size()),
            static_cast<int>(history.pending));
        row("History data", "%d blocks, %d tiles, %d layers", static_cast<int>(history.blocks.size()),
            static_cast<int>(history.tiles.size()), static_cast<int>(history.layers.size()));
        if (canvas.merge.result != kLayerInvalid) {
            row("Merge", "%d left, %d queued", static_cast<int>(canvas.merge.remaining.size()),
                static_cast<int>(canvas.merge.queued.size()));
        }
        for (const auto& [name, file] : {std::pair{"File", &canvas.file}, std::pair{"Disk cache", &canvas.cache}}) {
            if (file->env) {
                const CanvasFileSize size = CanvasFileSizeGet(*file);
                row(name, "%.1f MiB, %.1f used", mib(size.file), mib(size.used));
            } else {
                row(name, "none");
            }
        }
        ImGui::EndTable();
    }

    if (ImGui::CollapsingHeader("Stroke and input") && table("stroke")) {
        const StrokeState& stroke = app.stroke;
        if (stroke.canvas == kCanvasInvalid) {
            row("Stroke", "none");
        } else {
            constexpr std::array kShapes = {"brush", "rect", "lasso", "transform"};
            const auto pending = std::ranges::count_if(stroke.tiles, StrokeTilePending);
            row("Stroke", "%s on canvas %u%s", kShapes[std::to_underlying(stroke.shape)], stroke.canvas,
                stroke.ended ? ", ended" : "");
            row("Stroke tiles", "%d, %d pending", static_cast<int>(stroke.tiles.size()), static_cast<int>(pending));
            row("Stroke dabs", "%d", static_cast<int>(stroke.dabs.size()));
        }
        row("Transform", "%s", AppTransforming() ? (app.transform.held ? "held with Ctrl" : "open") : "none");
        const KeymapState& keymap = app.keymap;
        row("Keymap", "%d bindings, %d keys pending", static_cast<int>(keymap.bindings.size()),
            static_cast<int>(keymap.pendingLength));
        row("Held, repeating", "%d, %d", keymap.holding, keymap.repeating);
        row("Action requests", "%d", static_cast<int>(app.actionRequests.size()));
        ImGui::EndTable();
    }
    ImGui::End();
}

// The chosen atlas's cells at mip 0, kTileAtlasDebugColumns per row, sized to the window width. Premultiplied cells are
// drawn as straight alpha, so translucent paint looks darker than on the canvas.
void TileAtlasWindow(Window window) noexcept {
    ZoneScopedN("TileAtlasWindow");
    ImGui::SetNextWindowSize(ImVec2(400.0f, 450.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(TextLabel(Text::TileAtlas), &gUiState.tileAtlasShown[window])) {
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
    ImGui::SliderInt("Atlas###atlas", &atlas, 0, atlasCount - 1, "%d", ImGuiSliderFlags_AlwaysClamp);
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
    if (!ImGui::Begin(TextLabel(Text::Layers), &gUiState.layersShown[window])) {
        ImGui::End();
        return;
    }
    const auto view = std::ranges::find(gAppState.canvasViews, window, &CanvasViewState::window);
    if (view == gAppState.canvasViews.end() || view->id == kCanvasViewInvalid) {
        ImGui::TextUnformatted(TextGet(Text::NoCanvas));
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
        const auto blendModeNames = TextsGet(kBlendModeTexts);
        if (ImGui::Combo("##blend", &blendMode, blendModeNames.data(), static_cast<int>(blendModeNames.size()))) {
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
        ImGui::Text(TextGet(Text::Merging),
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

    if (ImGui::Button(TextLabel(Text::LayerNew))) {
        AppLayerNew(window, false);
    }
    ImGui::SameLine();
    if (ImGui::Button(TextLabel(Text::FolderNew))) {
        AppLayerNew(window, true);
    }
    ImGui::SameLine();
    if (ImGui::Button(TextLabel(Text::LayerDelete))) {
        AppLayerDelete(window);
    }
    ImGui::SameLine();
    if (ImGui::Button(TextLabel(Text::LayerUp))) {
        AppLayerMove(window, true);
    }
    ImGui::SameLine();
    if (ImGui::Button(TextLabel(Text::LayerDown))) {
        AppLayerMove(window, false);
    }
    ImGui::SameLine();
    if (ImGui::Button(TextLabel(Text::LayerMerge))) {
        AppLayerMerge(window);
    }
    ImGui::End();
}

// What applies to the whole app and is kept between runs, one section per topic.
void SettingsWindow(Window window) noexcept {
    ZoneScopedN("SettingsWindow");
    ImGui::SetNextWindowSize(ImVec2(360.0f, 200.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(TextLabel(Text::Settings), &gUiState.settingsShown[window])) {
        ImGui::End();
        return;
    }
    ImGui::SeparatorText(TextGet(Text::SettingsDisplay));
    const char* shown = TextGet(Text::LanguageSystem);
    for (const LanguageInfo& language : kLanguages) {
        if (gAppState.language == language.code) {
            shown = language.name;
        }
    }
    if (ImGui::BeginCombo(TextLabel(Text::Language), shown)) {
        if (ImGui::Selectable(TextGet(Text::LanguageSystem), gAppState.language.empty())) {
            ActionRequestPush(window, ActionId::SettingsLanguage, 0);
        }
        for (std::size_t i = 0; i < kLanguages.size(); ++i) {
            if (ImGui::Selectable(kLanguages[i].name, gAppState.language == kLanguages[i].code)) {
                ActionRequestPush(window, ActionId::SettingsLanguage, static_cast<std::uint32_t>(i + 1));
            }
        }
        ImGui::EndCombo();
    }
    const auto filterNames = TextsGet(kFilterTexts);
    int filter = std::to_underlying(gAppState.canvasFilter);
    if (ImGui::Combo(TextLabel(Text::CanvasFilter), &filter, filterNames.data(),
                     static_cast<int>(filterNames.size()))) {
        ActionRequestPush(window, ActionId::SettingsCanvasFilter, static_cast<std::uint32_t>(filter));
    }
    ImGui::SetItemTooltip("%s", TextGet(Text::CanvasFilterTip));
    // In percent, as the view panel shows the zoom.
    constexpr ImGuiSliderFlags kZoomFlags = ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp;
    float zoomMin = gAppState.zoomMin * 100.0f;
    if (ImGui::SliderFloat(TextLabel(Text::ZoomMin), &zoomMin, kViewportZoomMin * 100.0f, kViewportZoomMax * 100.0f,
                           "%.1f%%", kZoomFlags)) {
        gAppState.zoomMin = std::min(zoomMin / 100.0f, gAppState.zoomMax);
    }
    ImGui::SetItemTooltip(TextGet(Text::ZoomMinTip), AppZoomSupported(window) * 100.0f);
    float zoomMax = gAppState.zoomMax * 100.0f;
    if (ImGui::SliderFloat(TextLabel(Text::ZoomMax), &zoomMax, kViewportZoomMin * 100.0f, kViewportZoomMax * 100.0f,
                           "%.1f%%", kZoomFlags)) {
        gAppState.zoomMax = std::max(zoomMax / 100.0f, gAppState.zoomMin);
    }
    ImGui::End();
}

struct HelpEntry {
    Text keys;
    Text action;
};

constexpr std::array kHelpPaint = {
    HelpEntry{.keys = Text::GestureLeftDrag, .action = Text::GesturePaint},
    HelpEntry{.keys = Text::GestureCtrlHold, .action = Text::GestureCtrlTransform},
};
constexpr std::array kHelpErase = {
    HelpEntry{.keys = Text::GestureLeftDrag, .action = Text::GestureErase},
    HelpEntry{.keys = Text::GestureCtrlHold, .action = Text::GestureCtrlTransform},
};
constexpr std::array kHelpSelect = {
    HelpEntry{.keys = Text::GestureLeftDrag, .action = Text::GestureSelect},
    HelpEntry{.keys = Text::GestureCtrlHold, .action = Text::GestureCtrlTransform},
};
constexpr std::array kHelpLasso = {
    HelpEntry{.keys = Text::GestureLeftDrag, .action = Text::GestureLasso},
    HelpEntry{.keys = Text::GestureCtrlHold, .action = Text::GestureCtrlTransform},
};
constexpr std::array kHelpTransform = {
    HelpEntry{.keys = Text::GestureDragInside, .action = Text::GestureTransformMove},
    HelpEntry{.keys = Text::GestureDragHandle, .action = Text::GestureTransformScale},
    HelpEntry{.keys = Text::GestureShiftDragCorner, .action = Text::GestureTransformRatio},
    HelpEntry{.keys = Text::GestureAltDragHandle, .action = Text::GestureTransformDistort},
    HelpEntry{.keys = Text::GestureDragOutside, .action = Text::GestureTransformRotate},
    HelpEntry{.keys = Text::GestureShiftDragOutside, .action = Text::GestureTransformSnap},
};
constexpr std::array kHelpStroke = {HelpEntry{.keys = Text::GestureRelease, .action = Text::GestureFinishStroke}};
constexpr std::array kHelpNavigate = {
    HelpEntry{.keys = Text::GestureLeftDrag, .action = Text::GesturePan},
    HelpEntry{.keys = Text::GestureCtrlDrag, .action = Text::GestureZoom},
    HelpEntry{.keys = Text::GestureShiftDrag, .action = Text::GestureRotate},
    HelpEntry{.keys = Text::GestureAlt, .action = Text::GestureSnapView},
};
constexpr std::array kHelpAdjust = {
    HelpEntry{.keys = Text::GestureMove, .action = Text::GestureAdjustMove},
    HelpEntry{.keys = Text::GestureShift, .action = Text::GestureSlower},
    HelpEntry{.keys = Text::GestureAlt, .action = Text::GestureSnapValue},
    HelpEntry{.keys = Text::GestureKeep, .action = Text::GestureKeepValue},
    HelpEntry{.keys = Text::GestureCancel, .action = Text::GestureRestore},
};

enum struct HelpBindings : std::uint8_t {
    None,
    All,
    Layers,    // layer.*
    NotLayers, // neither layer.* nor transform.*
    Transform, // transform.*
};

// The gestures, then the bindings of `context` that `which` picks, as the keymap has them.
void HelpTable(const char* id, std::span<const HelpEntry> gestures, ActionContext context,
               HelpBindings which) noexcept {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        return;
    }
    const auto row = [](const char* keys, const char* action) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(keys);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(action);
    };
    for (const HelpEntry& entry : gestures) {
        row(TextGet(entry.keys), TextGet(entry.action));
    }
    const KeymapState& keymap = gAppState.keymap;
    for (std::size_t i = 0; i < keymap.bindings.size() && which != HelpBindings::None; ++i) {
        const KeyBinding& binding = keymap.bindings[i];
        const ActionInfo& info = kActions[std::to_underlying(binding.action)];
        const bool layer = std::string_view(info.name).starts_with("layer.");
        const bool transform = std::string_view(info.name).starts_with("transform.");
        if (binding.context != context || (which == HelpBindings::Layers && !layer)
            || (which == HelpBindings::NotLayers && (layer || transform))
            || (which == HelpBindings::Transform && !transform)) {
            continue;
        }
        if (info.argName) {
            std::array<char, 256> action{};
            std::format_to_n(action.data(), action.size() - 1, "{} {}", TextAction(binding.action), binding.arg);
            row(keymap.keys[i].c_str(), action.data());
        } else {
            row(keymap.keys[i].c_str(), TextAction(binding.action));
        }
    }
    ImGui::EndTable();
}

// Tips for what the user is doing right now: adjusting the brush, navigating, stroking, or else the current tool. The
// bindings that work everywhere and the layer ones follow, folded.
void HelpWindow(Window window, const ToolsState& tools) noexcept {
    ZoneScopedN("HelpWindow");
    ImGui::SetNextWindowSize(ImVec2(420.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(TextLabel(Text::Help), &gUiState.helpShown[window])) {
        ImGui::End();
        return;
    }
    const AppState& app = gAppState;
    constexpr std::array kAdjustTexts = {Text::HelpAdjustRadius, Text::HelpAdjustRadius, Text::HelpAdjustOpacity,
                                         Text::HelpAdjustHardness, Text::HelpAdjustLayerOpacity};
    if (app.brushAdjust.mode != BrushAdjustMode::None) {
        ImGui::TextUnformatted(TextGet(kAdjustTexts[std::to_underlying(app.brushAdjust.mode)]));
        HelpTable("adjust", kHelpAdjust, ActionContext::Global, HelpBindings::None);
    } else if (app.spaceDown || app.navigate.mode != NavigateMode::None) {
        ImGui::TextUnformatted(TextGet(Text::HelpNavigating));
        HelpTable("navigate", kHelpNavigate, ActionContext::Navigate, HelpBindings::All);
    } else if (AppTransforming()) {
        ImGui::TextUnformatted(TextGet(Text::HelpTransforming));
        HelpTable("transform", kHelpTransform, ActionContext::Canvas, HelpBindings::Transform);
    } else if (app.stroke.canvas != kCanvasInvalid && !app.stroke.ended) {
        Text title = app.stroke.mode == StrokeMode::Erase ? Text::HelpErasing : Text::HelpPainting;
        if (app.stroke.shape == StrokeShape::Rect || app.stroke.shape == StrokeShape::Lasso) {
            title = Text::HelpSelecting;
        }
        ImGui::TextUnformatted(TextGet(title));
        HelpTable("stroke", kHelpStroke, ActionContext::Global, HelpBindings::None);
    } else {
        Text title = Text::HelpPaintBrush;
        std::span<const HelpEntry> gestures = kHelpPaint;
        if (ToolCurrent(tools).target == ToolTarget::Layer && ToolCurrent(tools).blendMode == BlendMode::Erase) {
            title = Text::HelpEraser;
            gestures = kHelpErase;
        } else if (ToolCurrent(tools).target == ToolTarget::Selection && ToolCurrent(tools).shape == ToolShape::Box) {
            title = Text::HelpSelect;
            gestures = kHelpSelect;
        } else if (ToolCurrent(tools).target == ToolTarget::Selection) {
            title = Text::HelpLasso;
            gestures = kHelpLasso;
        }
        ImGui::TextUnformatted(TextGet(title));
        HelpTable("tool", gestures, ActionContext::Canvas, HelpBindings::NotLayers);
    }
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(TextLabel(Text::HelpGeneral))) {
        HelpTable("general", {}, ActionContext::Global, HelpBindings::All);
    }
    if (ImGui::CollapsingHeader(TextLabel(Text::HelpLayers))) {
        HelpTable("layers", {}, ActionContext::Canvas, HelpBindings::Layers);
    }
    ImGui::End();
}

// Every action without an argument, the tools, the window's canvas layers and the recent files that match
// the query: those that would run first, then the best match, then the most recently run.
void SearchEntriesBuild(Window window) noexcept {
    ZoneScopedN("SearchEntriesBuild");
    const AppState& app = gAppState;
    const std::string_view query(gUiState.searchQuery[window].data());
    std::vector<SearchEntry>& entries = gUiState.searchEntries[window];
    entries.clear();
    const auto add = [&](ActionId action, std::uint32_t arg, std::string_view label) {
        const ActionInfo& info = kActions[std::to_underlying(action)];
        // English too, so a name read in a tutorial finds it in any language.
        std::array<char, 1024> haystack{};
        const auto end = std::format_to_n(haystack.data(), haystack.size() - 1, "{} {} {} {}", info.name, label,
                                          TextAction(action), info.description)
                             .out;
        const float score = FuzzyScoreTerms(query, std::string_view(haystack.data(), end));
        if (score != kFuzzyNone) {
            SearchEntry entry{.label = {},
                              .arg = arg,
                              .score = score,
                              .action = action,
                              .blocked = ActionBlocked(window, action, arg)};
            std::format_to_n(entry.label.data(), entry.label.size() - 1, "{}", label);
            entries.push_back(entry);
        }
    };
    for (std::size_t i = 0; i < kActions.size(); ++i) {
        if (!kActions[i].argName) {
            add(static_cast<ActionId>(i), 0, {});
        }
    }
    for (const ToolState& tool : app.tools.library) {
        add(ActionId::ToolSelect, tool.id, tool.name.data());
    }
    if (const auto view = std::ranges::find(app.canvasViews, window, &CanvasViewState::window);
        view != app.canvasViews.end() && view->id != kCanvasViewInvalid) {
        const CanvasState& canvas = app.canvases[view->canvas];
        for (const Layer layer : canvas.layersOrder) {
            if (!HasAnyFlag(canvas.layers[layer].flags, LayerFlags::Internal)) {
                add(ActionId::LayerActivate, layer, canvas.layerNames[layer].data());
            }
        }
    }
    for (std::size_t i = 0; i < app.recentFiles.size(); ++i) {
        add(ActionId::CanvasOpenRecent, static_cast<std::uint32_t>(i), PathUtf8(app.recentFiles[i].filename()));
    }
    add(ActionId::SettingsLanguage, 0, TextGet(Text::LanguageSystem));
    for (std::size_t i = 0; i < kLanguages.size(); ++i) {
        add(ActionId::SettingsLanguage, static_cast<std::uint32_t>(i + 1), kLanguages[i].name);
    }
    for (std::size_t i = 0; i < kSelectionOpTexts.size(); ++i) {
        add(ActionId::SelectionOp, static_cast<std::uint32_t>(i), TextGet(kSelectionOpTexts[i]));
    }
    for (std::size_t i = 0; i < kFilterTexts.size(); ++i) {
        add(ActionId::SettingsCanvasFilter, static_cast<std::uint32_t>(i), TextGet(kFilterTexts[i]));
    }
    const auto recency = [&](ActionId action) {
        return std::ranges::find(app.recentActions, action) - app.recentActions.begin();
    };
    std::ranges::stable_sort(entries, [&](const SearchEntry& a, const SearchEntry& b) {
        if ((a.blocked == Text::Count_) != (b.blocked == Text::Count_)) {
            return a.blocked == Text::Count_;
        }
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return recency(a.action) < recency(b.action);
    });
}

// F1: a query field over the matching entries, each shown as keybinds.json names it, what it does and its keys. Up and
// Down pick, Enter or a click runs, Escape or a click elsewhere closes. An auto-sized window is hidden its first frame,
// so the field is focused until it is active rather than only while the window appears.
void SearchWindow(Window window) noexcept {
    ZoneScopedN("SearchWindow");
    constexpr int kRowsShown = 12;
    constexpr int kEntriesMax = 100;
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.12f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(std::max(io.DisplaySize.x * 0.5f, 320.0f), 0.0f), ImGuiCond_Always);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                                      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                                      | ImGuiWindowFlags_AlwaysAutoResize;
    bool& focus = gUiState.searchFocus[window];
    if (focus) {
        ImGui::SetNextWindowFocus();
    }
    bool close = false;
    if (ImGui::Begin("###search", nullptr, kFlags)) {
        std::array<char, 128>& query = gUiState.searchQuery[window];
        int& selected = gUiState.searchSelected[window];
        if (focus) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##query", TextGet(Text::SearchHint), query.data(), query.size())) {
            selected = 0;
        }
        if (ImGui::IsItemActive()) {
            focus = false;
        }
        SearchEntriesBuild(window);
        const std::vector<SearchEntry>& entries = gUiState.searchEntries[window];
        const int count = std::min(static_cast<int>(entries.size()), kEntriesMax);
        const bool down = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
        const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
        selected = std::clamp(selected + (down ? 1 : 0) - (up ? 1 : 0), 0, std::max(count - 1, 0));
        int run = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ? selected : -1;

        const float height = ImGui::GetTextLineHeightWithSpacing() * static_cast<float>(std::min(count, kRowsShown));
        if (count > 0
            && ImGui::BeginTable("results", 3, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg, ImVec2(0.0f, height))) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("description", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("keys", ImGuiTableColumnFlags_WidthFixed);
            for (int i = 0; i < count; ++i) {
                const SearchEntry& entry = entries[static_cast<std::size_t>(i)];
                const ActionInfo& info = kActions[std::to_underlying(entry.action)];
                const bool blocked = entry.blocked != Text::Count_;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(blocked);
                ImGui::PushID(i);
                std::array<char, 128> name{};
                if (info.argName) {
                    std::format_to_n(name.data(), name.size() - 1, "{}({})", info.name, entry.arg);
                } else {
                    std::format_to_n(name.data(), name.size() - 1, "{}", info.name);
                }
                if (ImGui::Selectable(name.data(), i == selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    run = i;
                }
                if (i == selected && (down || up)) {
                    ImGui::SetScrollHereY();
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                const char* description = blocked ? TextGet(entry.blocked) : TextAction(entry.action);
                if (entry.label[0] == '\0') {
                    ImGui::TextUnformatted(description);
                } else {
                    ImGui::Text("%s  %s", entry.label.data(), description);
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", BindingKeys(entry.action, entry.arg));
                ImGui::EndDisabled();
            }
            ImGui::EndTable();
        }
        if (run >= 0 && run < count && entries[static_cast<std::size_t>(run)].blocked == Text::Count_) {
            const SearchEntry& entry = entries[static_cast<std::size_t>(run)];
            ActionRequestPush(window, entry.action, entry.arg);
            close = true;
        }
        // A click inside makes its item active, the scrollbar too, which would otherwise count as not hovered.
        close = close || ImGui::IsKeyPressed(ImGuiKey_Escape)
             || (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                 && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows
                                            | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem));
    }
    ImGui::End();
    if (close) {
        gUiState.searchShown[window] = false;
    }
}

// While a key sequence waits for its next chord, in the bottom left corner: the bindings it may still become.
void WhichKeyOverlay(const WindowState& windowState) noexcept {
    const KeymapState& keymap = gAppState.keymap;
    if (keymap.pendingLength == 0) {
        return;
    }
    const float margin = 8.0f * windowState.dpiScale;
    ImGui::SetNextWindowPos(ImVec2(margin, ImGui::GetIO().DisplaySize.y - margin), ImGuiCond_Always,
                            ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                                      | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
                                      | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("###whichKey", nullptr, kFlags) && ImGui::BeginTable("next", 2, ImGuiTableFlags_SizingFixedFit)) {
        for (std::size_t i = 0; i < keymap.bindings.size(); ++i) {
            const KeyBinding& binding = keymap.bindings[i];
            if (!KeymapContinues(keymap, binding)) {
                continue;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(keymap.keys[i].c_str());
            ImGui::TableNextColumn();
            if (kActions[std::to_underlying(binding.action)].argName) {
                ImGui::Text("%s %u", TextAction(binding.action), binding.arg);
            } else {
                ImGui::TextUnformatted(TextAction(binding.action));
            }
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

// ScaleAllSizes is not reversible, so a DPI change rebuilds the style from the defaults.
void ApplyDpiScale(float dpiScale) noexcept {
    ZoneScopedN("ApplyDpiScale");
    ImGuiStyle style{};
    ImGui::StyleColorsDark(&style);
    style.ScaleAllSizes(dpiScale);
    style.FontSizeBase = kFontSize;
    style.FontScaleDpi = dpiScale;
    ImGui::GetStyle() = style;
}

[[nodiscard]] std::vector<std::byte> FontRead(FontScript script) noexcept {
    const auto path = PlatformFontPath(script);
    if (!path) {
        return {};
    }
    std::ifstream file(*path, std::ios::binary | std::ios::ate);
    const std::streamoff size = file ? static_cast<std::streamoff>(file.tellg()) : -1;
    if (size <= 0) {
        MIDORI_LOG_WARN("font {} not read", PathUtf8(*path));
        return {};
    }
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) {
        MIDORI_LOG_WARN("font {} not read", PathUtf8(*path));
        return {};
    }
    return data;
}

void FontsAdd(ImFontAtlas& atlas) noexcept {
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    if (gUiState.fontLatin.empty()) {
        atlas.AddFontDefault();
    } else {
        atlas.AddFontFromMemoryTTF(gUiState.fontLatin.data(), static_cast<int>(gUiState.fontLatin.size()), 0.0f,
                                   &config);
    }
    if (!gUiState.fontJapanese.empty()) {
        config.MergeMode = true;
        atlas.AddFontFromMemoryTTF(gUiState.fontJapanese.data(), static_cast<int>(gUiState.fontJapanese.size()), 0.0f,
                                   &config);
    }
}

} // namespace

void UiFontsLoad() noexcept {
    ZoneScopedN("UiFontsLoad");
    gUiState.fontLatin = FontRead(FontScript::Latin);
    gUiState.fontJapanese = FontRead(FontScript::Japanese);
    if (gUiState.fontLatin.empty()) {
        MIDORI_LOG_WARN("no UI font installed, Dear ImGui's used");
    }
    if (gUiState.fontJapanese.empty()) {
        MIDORI_LOG_WARN("no Japanese font installed, kana and kanji not shown");
    }
}

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
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // Its windows open where the preferences keep them instead, docked or not.
    ImGui::LoadIniSettingsFromMemory(gUiState.preferences.imguiSettings.data(),
                                     gUiState.preferences.imguiSettings.size());
    io.BackendPlatformName = "midori";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;
    // No keyboard navigation: Space would press the focused widget instead of navigating the canvas.
    // imgui.cpp's default Win32 IME handler places the candidate window over this handle.
    ImGui::GetMainViewport()->PlatformHandleRaw = *hwnd;
    FontsAdd(*io.Fonts);
    ApplyDpiScale(state->dpiScale);

    gUiState.contexts[window] = context;
    gUiState.frameTimes[window] = 0;
    gUiState.tileAtlasShown[window] = false;
    gUiState.tileAtlasIndex[window] = 0;
    gUiState.tileCellsShown[window] = false;
    gUiState.statisticsShown[window] = false;
    gUiState.imguiMetricsShown[window] = false;
    gUiState.hidden[window] = false;
    gUiState.helpShown[window] = gUiState.preferences.helpShown;
    gUiState.menuBarShown[window] = gUiState.preferences.menuBarShown;
    gUiState.viewShown[window] = gUiState.preferences.viewShown;
    gUiState.toolShown[window] = gUiState.preferences.toolShown;
    gUiState.toolbarShown[window] = gUiState.preferences.toolbarShown;
    gUiState.toolsShown[window] = gUiState.preferences.toolsShown;
    gUiState.toolRenaming[window] = 0;
    gUiState.layersShown[window] = gUiState.preferences.layersShown;
    gUiState.settingsShown[window] = false;
    gUiState.layerFoldersClosed[window].clear();
    gUiState.layerRenaming[window] = -1;
    return context;
}

bool UiWantsFrame(Window window) noexcept {
    if (window >= kWindowMax || !gUiState.contexts[window]) {
        return false;
    }
    ImGui::SetCurrentContext(gUiState.contexts[window]);
    return ImGui::GetIO().WantTextInput
        || std::chrono::steady_clock::now().time_since_epoch().count() < gUiState.settleUntil[window];
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
    gUiState.settleUntil[window] =
        (std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(kUiSettle))
            .time_since_epoch()
            .count();
    GraphicsWindowRequestFrame(window);
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

void UiWindowFrame(Window window, Viewport* viewport, Color::Srgb* backgroundColor, ToolsState& tools) noexcept {
    ZoneScopedN("UiWindowFrame");
    MIDORI_ASSERT(window < kWindowMax && gUiState.contexts[window], "UiWindowFrame: no context for window {}", window);
    const auto state = WindowGetState(window);
    if (!state) {
        return;
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
    const auto view = std::ranges::find(gAppState.canvasViews, window, &CanvasViewState::window);
    const bool hasCanvas = viewport && view != gAppState.canvasViews.end() && view->id != kCanvasViewInvalid;
    StateOverlay(*state, hasCanvas ? viewport : nullptr, hasCanvas ? &gAppState.canvases[view->canvas] : nullptr,
                 tools);
    TransformOverlay(window, *state);
    WhichKeyOverlay(*state);
    if (gUiState.searchShown[window]) {
        SearchWindow(window);
    }
    if (gUiState.hidden[window]) {
        BrushAdjustOverlay(window, *state, tools);
        ImGui::Render();
        return;
    }
    if (gUiState.menuBarShown[window] && ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(TextLabel(Text::File))) {
            MenuAction(window, Text::New, ActionId::CanvasNew);
            MenuAction(window, Text::Open, ActionId::CanvasOpen);
            ImGui::Separator();
            MenuAction(window, Text::Save, ActionId::CanvasSave);
            MenuAction(window, Text::SaveAs, ActionId::CanvasSaveAs);
            // The file name, its folder on the right.
            const std::vector<std::filesystem::path>& recent = gAppState.recentFiles;
            if (ImGui::BeginMenu(TextLabel(Text::OpenRecent), !recent.empty())) {
                for (std::size_t i = 0; i < recent.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    if (ImGui::MenuItem(PathUtf8(recent[i].filename()).c_str(),
                                        PathUtf8(recent[i].parent_path()).c_str())) {
                        ActionRequestPush(window, ActionId::CanvasOpenRecent, static_cast<std::uint32_t>(i));
                    }
                    ImGui::PopID();
                }
                ImGui::Separator();
                MenuAction(window, Text::RecentClear, ActionId::CanvasRecentClear);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            MenuAction(window, Text::Exit, ActionId::AppQuit);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(TextLabel(Text::Edit))) {
            MenuAction(window, Text::Undo, ActionId::HistoryUndo);
            MenuAction(window, Text::Redo, ActionId::HistoryRedo);
            ImGui::Separator();
            MenuAction(window, Text::Cut, ActionId::EditCut);
            MenuAction(window, Text::Copy, ActionId::EditCopy);
            MenuAction(window, Text::Paste, ActionId::EditPaste);
            ImGui::Separator();
            MenuAction(window, Text::SettingsMenu, ActionId::PanelSettings, gUiState.settingsShown[window]);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(TextLabel(Text::WindowMenu))) {
            MenuAction(window, Text::Fullscreen, ActionId::WindowFullscreen,
                       state->displayMode == WindowDisplayMode::Fullscreened);
            MenuAction(window, Text::HideUi, ActionId::UiToggle, gUiState.hidden[window]);
            ImGui::Separator();
            MenuAction(window, Text::View, ActionId::PanelView, gUiState.viewShown[window]);
            MenuAction(window, Text::Toolbar, ActionId::PanelToolbar, gUiState.toolbarShown[window]);
            MenuAction(window, Text::Tools, ActionId::PanelTools, gUiState.toolsShown[window]);
            MenuAction(window, Text::Tool, ActionId::PanelTool, gUiState.toolShown[window]);
            MenuAction(window, Text::Layers, ActionId::PanelLayers, gUiState.layersShown[window]);
            MenuAction(window, Text::Help, ActionId::PanelHelp, gUiState.helpShown[window]);
            MenuAction(window, Text::MenuBar, ActionId::PanelMenuBar, gUiState.menuBarShown[window]);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(TextLabel(Text::Debug))) {
            MenuAction(window, Text::TileAtlas, ActionId::DebugTileAtlas, gUiState.tileAtlasShown[window]);
            MenuAction(window, Text::TileCells, ActionId::DebugTileCells, gUiState.tileCellsShown[window]);
            MenuAction(window, Text::Statistics, ActionId::DebugStatistics, gUiState.statisticsShown[window]);
            MenuAction(window, Text::ImGuiMetrics, ActionId::DebugImGuiMetrics, gUiState.imguiMetricsShown[window]);
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }
    // Panels dock around the canvas, which shows and takes input through the empty middle.
    ImGui::DockSpaceOverViewport(0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);
    if (viewport && gUiState.viewShown[window]) {
        if (ImGui::Begin(TextLabel(Text::View), &gUiState.viewShown[window])) {
            ImGui::Text(TextGet(Text::ViewZoom), std::abs(viewport->zoomX) * 100.0f,
                        viewport->rot * 180.0f / std::numbers::pi_v<float>);
            // The pointer in canvas units, as painting places it.
            const float centerX = std::floor(static_cast<float>(state->width) * 0.5f);
            const float centerY = std::floor(static_cast<float>(state->height) * 0.5f);
            const Viewport under = ViewportMove(*viewport, -(gAppState.pointerX - centerX) / state->dpiScale,
                                                -(gAppState.pointerY - centerY) / state->dpiScale);
            ImGui::Text(TextGet(Text::ViewCenter), viewport->posX, viewport->posY);
            ImGui::Text(TextGet(Text::ViewPointer), under.posX, under.posY);
            if (ImGui::Button(ShortcutLabel(Text::FlipX, ActionId::ViewFlipHorizontal).data())) {
                *viewport = ViewportZoom(*viewport, -1.0f, 1.0f, kViewportZoomMin, kViewportZoomMax);
            }
            ImGui::SameLine();
            if (ImGui::Button(ShortcutLabel(Text::FlipY, ActionId::ViewFlipVertical).data())) {
                *viewport = ViewportZoom(*viewport, 1.0f, -1.0f, kViewportZoomMin, kViewportZoomMax);
            }
            ImGui::SameLine();
            if (ImGui::Button(ShortcutLabel(Text::Reset, ActionId::ViewReset).data())) {
                *viewport = ViewportSnap(ViewportResetZoomRotation(*viewport), state->dpiScale);
            }
            ImGui::ColorEdit4(TextLabel(Text::Background), &backgroundColor->r);
        }
        ImGui::End();
    }
    if (gUiState.toolbarShown[window]) {
        ToolbarWindow(window, tools);
    }
    if (gUiState.toolsShown[window]) {
        ToolsWindow(window, tools);
    }
    if (gUiState.toolShown[window]) {
        ToolWindow(window, tools);
    }
    if (gUiState.tileAtlasShown[window]) {
        TileAtlasWindow(window);
    }
    if (gUiState.statisticsShown[window]) {
        StatisticsWindow(window);
    }
    if (gUiState.imguiMetricsShown[window]) {
        ImGui::ShowMetricsWindow(&gUiState.imguiMetricsShown[window]);
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
    preferences.toolbarShown = gUiState.toolbarShown[window];
    preferences.toolsShown = gUiState.toolsShown[window];
    preferences.layersShown = gUiState.layersShown[window];
    preferences.helpShown = gUiState.helpShown[window];
    preferences.menuBarShown = gUiState.menuBarShown[window];
    // With no ini file, ImGui only flags that its windows moved or resized.
    if (io.WantSaveIniSettings) {
        preferences.imguiSettings = ImGui::SaveIniSettingsToMemory();
        io.WantSaveIniSettings = false;
    }
}

} // namespace Midori
