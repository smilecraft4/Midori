#include "midori/ui.hpp"
#include "midori/log.hpp"
#include "midori/tool.hpp"

#include <array>
#include <chrono>
#include <cmath>
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
    ImGui::SliderFloat("Radius", &brush.radius, 0.5f, 500.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Hardness", &brush.hardness, 0.0f, 1.0f);
    ImGui::SliderFloat("Flow", &brush.flow, 0.0f, 1.0f);
    ImGui::SliderFloat("Opacity", &brush.opacity, 0.0f, 1.0f);
    ImGui::SliderFloat("Spacing", &brush.spacing, 0.01f, 1.0f);
    ImGui::Checkbox("Pressure radius", &brush.pressureRadius);
    ImGui::Checkbox("Pressure opacity", &brush.pressureOpacity);
}

// ScaleAllSizes is not reversible, so a DPI change rebuilds the style from the defaults.
void ApplyDpiScale(float dpiScale) noexcept {
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

UiCommand UiWindowFrame(Window window, Viewport* viewport, float& backgroundOpacity, ToolsState& tools) noexcept {
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
        ImGui::EndMainMenuBar();
    }
    if (viewport) {
        if (ImGui::Begin("View")) {
            ImGui::Text("Zoom %.1f%%  Rotation %.1f deg", std::abs(viewport->zoomX) * 100.0f,
                        viewport->rot * 180.0f / std::numbers::pi_v<float>);
            if (ImGui::Button("Flip X")) {
                *viewport = ViewportZoom(*viewport, -1.0f, 1.0f);
            }
            ImGui::SameLine();
            if (ImGui::Button("Flip Y")) {
                *viewport = ViewportZoom(*viewport, 1.0f, -1.0f);
            }
            ImGui::SliderFloat("Background opacity", &backgroundOpacity, 0.0f, 1.0f);
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
    ImGui::Render();
    return command;
}

} // namespace Midori
