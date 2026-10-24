#pragma once

#include "midori/input.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Midori {

enum struct ActionId : std::uint8_t {
    AppQuit,
    AppSearch,
    CanvasNew,
    CanvasOpen,
    CanvasSave,
    CanvasSaveAs,
    HistoryUndo,
    HistoryRedo,
    WindowFullscreen,
    UiToggle,
    ToolPaint,
    ToolErase,
    BrushPreset,
    BrushRadius,
    BrushOpacity,
    BrushHardness,
    LayerNew,
    LayerNewFolder,
    LayerDelete,
    LayerMerge,
    LayerSelectAbove,
    LayerSelectBelow,
    LayerMoveUp,
    LayerMoveDown,
    LayerToggleHidden,
    LayerBlendNext,
    LayerBlendPrevious,
    LayerOpacity,
    ViewNavigate,
    ViewReset,
    ViewFlipHorizontal,
    ViewFlipVertical,
    ViewBack,
    ViewForward,
    Count_,
};

enum struct ActionKind : std::uint8_t {
    Trigger,
    Repeat, // runs again while the last key of its sequence repeats
    Hold,   // lasts while the last key of its sequence is down
};

// Searched by name and description, shown as `name(argName)` description (keys), the name as keybinds.json writes it.
struct ActionInfo {
    const char* name;      // stable, the one keybinds.json uses
    const char* description;
    const char* argName{}; // nullptr when it takes none
    ActionKind kind{};
};

constexpr std::array<ActionInfo, std::to_underlying(ActionId::Count_)> kActions = {{
    {.name = "app.quit", .description = "Close every window, asking to save"},
    {.name = "app.search", .description = "Search every action"},
    {.name = "canvas.new", .description = "New canvas in this window"},
    {.name = "canvas.open", .description = "Open a canvas in this window"},
    {.name = "canvas.save", .description = "Save the canvas"},
    {.name = "canvas.saveAs", .description = "Save the canvas to a new file"},
    {.name = "history.undo", .description = "Undo the last step", .kind = ActionKind::Repeat},
    {.name = "history.redo", .description = "Redo the last undone step", .kind = ActionKind::Repeat},
    {.name = "window.fullscreen", .description = "Toggle fullscreen"},
    {.name = "ui.toggle", .description = "Hide or show the UI"},
    {.name = "tool.paint", .description = "Switch to the paint brush"},
    {.name = "tool.erase", .description = "Switch to the eraser"},
    {.name = "brush.preset", .description = "Give the current tool a brush preset", .argName = "number"},
    {.name = "brush.radius", .description = "Change the brush radius"},
    {.name = "brush.opacity", .description = "Change the brush opacity"},
    {.name = "brush.hardness", .description = "Change the brush hardness"},
    {.name = "layer.new", .description = "New layer above the active one"},
    {.name = "layer.newFolder", .description = "New folder above the active layer"},
    {.name = "layer.delete", .description = "Delete the selected layers, a folder with its layers"},
    {.name = "layer.merge", .description = "Merge the active layer down, or a folder into one layer"},
    {.name = "layer.selectAbove", .description = "Select the layer above"},
    {.name = "layer.selectBelow", .description = "Select the layer below"},
    {.name = "layer.moveUp", .description = "Move the active layer up"},
    {.name = "layer.moveDown", .description = "Move the active layer down"},
    {.name = "layer.toggleHidden", .description = "Hide or show the selected layers"},
    {.name = "layer.blendNext", .description = "Next blend mode of the selected layers"},
    {.name = "layer.blendPrevious", .description = "Previous blend mode of the selected layers"},
    {.name = "layer.opacity", .description = "Change the active layer's opacity"},
    {.name = "view.navigate",
     .description = "Drag to pan, with Ctrl zoom, with Shift rotate",
     .kind = ActionKind::Hold},
    {.name = "view.reset", .description = "Reset zoom and rotation"},
    {.name = "view.flipHorizontal", .description = "Flip the view horizontally"},
    {.name = "view.flipVertical", .description = "Flip the view vertically"},
    {.name = "view.back", .description = "Back to the previous view", .kind = ActionKind::Repeat},
    {.name = "view.forward", .description = "Forward to the next view", .kind = ActionKind::Repeat},
}};

// Where a binding applies; the most specific active context wins.
enum struct ActionContext : std::uint8_t {
    Global,
    Canvas,   // a canvas view has the keyboard
    Navigate, // view.navigate is held
};

struct KeyChord {
    Key key;
    KeyMod mods; // Shift, Ctrl, Alt, Super: either side matches
};

constexpr std::size_t kKeySequenceMax = 4;

// Written "Ctrl+K, Shift+1" in keybinds.json. No binding is a prefix of another, so a sequence never waits on a
// timeout.
struct KeyBinding {
    std::array<KeyChord, kKeySequenceMax> chords;
    std::uint32_t arg{};
    ActionId action;
    ActionContext context{};
    std::uint8_t length{1};
};

// Parsed like keybinds.json, which replaces them.
struct KeyBindingText {
    const char* keys;
    ActionId action;
    std::uint32_t arg{};
    ActionContext context{};
};

constexpr std::array kDefaultBindings = {
    KeyBindingText{.keys = "Ctrl+Q", .action = ActionId::AppQuit},
    KeyBindingText{.keys = "F1", .action = ActionId::AppSearch},
    KeyBindingText{.keys = "Ctrl+N", .action = ActionId::CanvasNew},
    KeyBindingText{.keys = "Ctrl+O", .action = ActionId::CanvasOpen},
    KeyBindingText{.keys = "Ctrl+S", .action = ActionId::CanvasSave},
    KeyBindingText{.keys = "Ctrl+Shift+S", .action = ActionId::CanvasSaveAs},
    KeyBindingText{.keys = "Ctrl+Z", .action = ActionId::HistoryUndo},
    KeyBindingText{.keys = "Ctrl+Shift+Z", .action = ActionId::HistoryRedo},
    KeyBindingText{.keys = "Ctrl+Y", .action = ActionId::HistoryRedo},
    KeyBindingText{.keys = "F11", .action = ActionId::WindowFullscreen},
    KeyBindingText{.keys = "Tab", .action = ActionId::UiToggle},
    KeyBindingText{.keys = "B", .action = ActionId::ToolPaint, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "E", .action = ActionId::ToolErase, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Shift+B, 1", .action = ActionId::BrushPreset, .arg = 1, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Shift+B, 2", .action = ActionId::BrushPreset, .arg = 2, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Shift+B, 3", .action = ActionId::BrushPreset, .arg = 3, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Shift+B, 4", .action = ActionId::BrushPreset, .arg = 4, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Shift+B, 5", .action = ActionId::BrushPreset, .arg = 5, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Shift+B, 6", .action = ActionId::BrushPreset, .arg = 6, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Shift+B, 7", .action = ActionId::BrushPreset, .arg = 7, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Shift+B, 8", .action = ActionId::BrushPreset, .arg = 8, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Shift+B, 9", .action = ActionId::BrushPreset, .arg = 9, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "F", .action = ActionId::BrushRadius, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Shift+F", .action = ActionId::BrushOpacity, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Ctrl+F", .action = ActionId::BrushHardness, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Ctrl+Shift+N", .action = ActionId::LayerNew, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Ctrl+G", .action = ActionId::LayerNewFolder, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Ctrl+E", .action = ActionId::LayerMerge, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "PageUp", .action = ActionId::LayerSelectAbove, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "PageDown", .action = ActionId::LayerSelectBelow, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Ctrl+PageUp", .action = ActionId::LayerMoveUp, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Ctrl+PageDown", .action = ActionId::LayerMoveDown, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Ctrl+Comma", .action = ActionId::LayerToggleHidden, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Alt+PageUp", .action = ActionId::LayerBlendNext, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Alt+PageDown", .action = ActionId::LayerBlendPrevious, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "O", .action = ActionId::LayerOpacity, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Space", .action = ActionId::ViewNavigate, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "R", .action = ActionId::ViewReset, .context = ActionContext::Navigate},
    KeyBindingText{.keys = "F", .action = ActionId::ViewFlipHorizontal, .context = ActionContext::Navigate},
    KeyBindingText{.keys = "Shift+F", .action = ActionId::ViewFlipVertical, .context = ActionContext::Navigate},
    KeyBindingText{.keys = "Z", .action = ActionId::ViewBack, .context = ActionContext::Navigate},
    KeyBindingText{.keys = "Shift+Z", .action = ActionId::ViewForward, .context = ActionContext::Navigate},
};

struct KeymapState {
    std::vector<KeyBinding> bindings;
    std::array<KeyChord, kKeySequenceMax> pending;
    std::uint32_t pendingFrame; // when the last chord came, for the which-key popup delay
    KeyMod pendingHeld;         // held since the previous chord: ignored when matching the next
    std::uint8_t pendingLength;
};

struct SearchEntry {
    std::string label; // dynamic entries only: the layer, region or file name
    std::uint32_t arg;
    int score;
    ActionId action;
};

} // namespace Midori
