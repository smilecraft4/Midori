#pragma once

#include "midori/base.hpp"
#include "midori/input.hpp"
#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
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
    EditCopy,
    EditCut,
    EditPaste,
    WindowFullscreen,
    UiToggle,
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
    LayerActivate,
    CanvasOpenRecent,
    CanvasRecentClear,
    PanelSettings,
    PanelView,
    PanelTool,
    PanelLayers,
    PanelHelp,
    PanelMenuBar,
    DebugTileAtlas,
    DebugTileCells,
    DebugStatistics,
    DebugImGuiMetrics,
    SettingsLanguage,
    SettingsCanvasFilter,
    ToolSelect,
    SelectionClear,
    SelectionDelete,
    SelectionOp,
    SelectionTransform,
    TransformApply,
    TransformCancel,
    BrushOpen,
    BrushBuiltIn,
    BrushTexture,
    BrushExample,
    ToolbarSlot,
    ToolDuplicate,
    ToolDelete,
    PanelToolbar,
    PanelTools,
    BrushReload,
    Count_,
};

// How a binding fires, written after its keys: "Shift(Hold)". Without one, its action's kind.
enum struct ActionKind : std::uint8_t {
    Press,
    Repeat, // runs again while the last key of its sequence repeats
    Hold,   // one step only, lasts while its keys are down; one key alone also matches with more modifiers held
};

// Searched by name and description, shown as `name(argName)` description (keys), the name as keybinds.json writes it.
struct ActionInfo {
    const char* name;      // stable, the one keybinds.json uses
    const char* description;
    const char* argName{}; // nullptr when it takes none
    ActionKind kind{};
    bool holdable{};       // also bound with Hold, read while held rather than run: see KeymapHeld
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
    {.name = "edit.copy", .description = "Copy the active layer's pixels in the selection, for this app or another"},
    {.name = "edit.cut", .description = "Copy the active layer's pixels in the selection, then delete them"},
    {.name = "edit.paste",
     .description = "Paste the clipboard's image, a screenshot or one copied from a web page, into a new layer to "
                    "place it"},
    {.name = "window.fullscreen", .description = "Toggle fullscreen"},
    {.name = "ui.toggle", .description = "Hide or show the UI"},
    {.name = "brush.radius", .description = "Change the brush size"},
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
    {.name = "layer.activate", .description = "Make a layer the active one", .argName = "id"},
    {.name = "canvas.openRecent", .description = "Open a recent file", .argName = "index"},
    {.name = "canvas.recentClear", .description = "Forget the recent files"},
    {.name = "panel.settings", .description = "Show or hide the settings"},
    {.name = "panel.view", .description = "Show or hide the view panel"},
    {.name = "panel.tool", .description = "Show or hide the tool panel"},
    {.name = "panel.layers", .description = "Show or hide the layers panel"},
    {.name = "panel.help", .description = "Show or hide the help"},
    {.name = "panel.menuBar", .description = "Show or hide the menu bar"},
    {.name = "debug.tileAtlas", .description = "Show or hide the tile atlas"},
    {.name = "debug.tileCells", .description = "Show or hide the tile cells on the canvas"},
    {.name = "debug.statistics", .description = "Show or hide the memory, jobs, caches and queues behind the canvas"},
    {.name = "debug.imguiMetrics", .description = "Show or hide Dear ImGui's own metrics"},
    {.name = "settings.language", .description = "Change the language, 0 follows the system", .argName = "index"},
    {.name = "settings.canvasFilter",
     .description = "Change how the canvas is drawn between its pixels",
     .argName = "filter"},
    {.name = "tool.select", .description = "Pick a tool", .argName = "id"},
    {.name = "selection.clear", .description = "Select nothing"},
    {.name = "selection.delete", .description = "Delete the active layer's pixels in the selection"},
    {.name = "selection.op",
     .description = "Change how a new selection combines with the last one, or only while held",
     .argName = "operation",
     .holdable = true},
    {.name = "selection.transform",
     .description = "Move, scale, turn or distort the selection and the active layer's pixels in it"},
    {.name = "transform.apply", .description = "Apply the transform"},
    {.name = "transform.cancel", .description = "Put the selection and its pixels back as they were"},
    {.name = "brush.open", .description = "Paint with a brush file, read again whenever it changes"},
    {.name = "brush.builtIn", .description = "Paint with the built-in round brush"},
    {.name = "brush.texture", .description = "Pick the image one of the brush's textures reads", .argName = "index"},
    {.name = "brush.example",
     .description = "Paint with one of the example brushes next to Midori",
     .argName = "index"},
    {.name = "toolbar.slot",
     .description = "Pick a toolbar slot's tool, or the next one of its drawer when it is the active slot",
     .argName = "id"},
    {.name = "tool.duplicate",
     .description = "Copy a tool into the library, next to it in the active drawer",
     .argName = "id"},
    {.name = "tool.delete", .description = "Delete a tool from the library and the toolbar", .argName = "id"},
    {.name = "panel.toolbar", .description = "Show or hide the toolbar"},
    {.name = "panel.tools", .description = "Show or hide the tools of the active toolbar slot"},
    {.name = "brush.reload", .description = "Read the current tool's brush file and its textures again"},
}};

// Where a binding applies. Each lies within the one before, so Navigate is active with Canvas and Global around it, and
// the innermost binding wins. Written in keybinds.json as kContextNames.
enum struct ActionContext : std::uint8_t {
    Global,
    Canvas,   // a canvas view has the keyboard
    Navigate, // view.navigate is held
    Count_,
};

constexpr std::array<std::string_view, std::to_underlying(ActionContext::Count_)> kContextNames = {
    "global",
    "canvas",
    "navigate",
};

struct KeyChord {
    Key key;     // Unknown: the modifiers alone, for a Hold binding
    KeyMod mods; // Shift, Ctrl, Alt, Super: either side matches
};

constexpr std::size_t kKeySequenceMax = 4;

// Written "Ctrl+K, Shift+1" or "Shift(Hold)" in keybinds.json. No binding is a prefix of another, so a sequence never
// waits on a timeout.
struct KeyBinding {
    std::array<KeyChord, kKeySequenceMax> chords;
    std::uint32_t arg{};
    ActionId action;
    ActionContext context{};
    std::uint8_t length{1};
    ActionKind trigger{};
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
    KeyBindingText{.keys = "Ctrl+C", .action = ActionId::EditCopy, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Ctrl+X", .action = ActionId::EditCut, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Ctrl+V", .action = ActionId::EditPaste, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Ctrl+Shift+Z", .action = ActionId::HistoryRedo},
    KeyBindingText{.keys = "Ctrl+Y", .action = ActionId::HistoryRedo},
    KeyBindingText{.keys = "F11", .action = ActionId::WindowFullscreen},
    KeyBindingText{.keys = "Tab", .action = ActionId::UiToggle},
    KeyBindingText{.keys = "B", .action = ActionId::ToolbarSlot, .arg = 1, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "E", .action = ActionId::ToolbarSlot, .arg = 2, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "M", .action = ActionId::ToolbarSlot, .arg = 3, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Shift(Hold)", .action = ActionId::SelectionOp, .arg = 1, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Alt(Hold)", .action = ActionId::SelectionOp, .arg = 2, .context = ActionContext::Canvas},
    KeyBindingText{
        .keys = "Shift+Alt(Hold)", .action = ActionId::SelectionOp, .arg = 3, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Ctrl+D", .action = ActionId::SelectionClear, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Delete", .action = ActionId::SelectionDelete, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Ctrl+T", .action = ActionId::SelectionTransform, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Enter", .action = ActionId::TransformApply, .context = ActionContext::Canvas},
    KeyBindingText{.keys = "Escape", .action = ActionId::TransformCancel, .context = ActionContext::Canvas},
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
    std::vector<std::string> keys; // each binding as the keyboard layout names its keys: "Maj+B, &"
    std::array<KeyChord, kKeySequenceMax> pending;
    std::bitset<std::to_underlying(Key::Count_)> down;
    std::int32_t holding{-1};      // the Hold binding whose last key is down
    std::int32_t repeating{-1};    // the Repeat binding its last key repeats while down
    std::uint8_t pendingLength{};
};

// Picked in a UI frame, which runs inside the renderer's submit where no dialog can open: the next AppUpdate runs it.
struct ActionRequest {
    std::uint32_t arg;
    ActionId action;
    Window window;
};

constexpr std::size_t kRecentActionsMax = 32;

// Chords split by commas, each its modifiers (Ctrl, Shift, Alt, Super) then its key, joined by +; names as kKeyNames
// writes them, case ignored. Then maybe (Press), (Repeat) or (Hold), else `trigger`. A Hold binding is one chord, which
// may be modifiers alone. Only the chords, length and trigger are set.
[[nodiscard]] Result<KeyBinding> KeyBindingParse(std::string_view keys, ActionKind trigger) noexcept;
// kDefaultBindings, then keybinds.json in the settings folder over them, an empty array written there when missing.
// Each entry replaces the bindings it conflicts with (the same chords, or a prefix of them, in the same context);
// "action": "none" only unbinds them. Entries not understood are logged and skipped. Each binding's keys are shown as
// the current keyboard layout names them.
void KeymapLoad(KeymapState& keymap) noexcept;
// Binds `action` with `arg` to `keys` in keybinds.json, read as KeyBindingParse reads them, in place of every binding it
// had; empty `keys` only unbinds it. The keymap is loaded again.
Result<void> KeymapBind(KeymapState& keymap, std::string_view keys, ActionId action, std::uint32_t arg,
                        ActionContext context) noexcept;
// As keybinds.json writes it, whatever the keyboard layout: "Ctrl+K, Shift+1", "Shift(Hold)".
std::string KeyBindingKeys(const KeyBinding& binding) noexcept;
// The keys of the first binding of `action`, "" when it has none.
[[nodiscard]] const char* KeymapKeys(const KeymapState& keymap, ActionId action) noexcept;
// Adds a pressed chord to the sequence in progress, in `context` and those around it. Returns the binding it completes,
// else -1, with `pendingLength` above 0 while a longer binding may still follow. Modifiers held since the previous
// chord match whether a binding repeats them or not.
[[nodiscard]] std::int32_t KeymapPress(KeymapState& keymap, KeyChord chord, ActionContext context) noexcept;
// The binding goes on from the chords pressed so far, matched as KeymapPress matches them.
[[nodiscard]] bool KeymapContinues(const KeymapState& keymap, const KeyBinding& binding) noexcept;
// The Hold binding of `action` whose keys are all down with `mods`, in `context` or one around it, else -1. Of those, the
// one with the most keys wins: Shift+Alt's over Shift's.
std::int32_t KeymapHeld(const KeymapState& keymap, ActionId action, ActionContext context, KeyMod mods) noexcept;

constexpr float kFuzzyNone = -1e30f;
constexpr std::size_t kFuzzyLengthMax = 256;

// How well `needle` matches `haystack` in order, case ignored, kFuzzyNone when it does not: fzy's scoring
// (github.com/jhawthorn/fzy, MIT), which favors consecutive letters and the starts of words, camelCase humps and dotted
// names. Only the first kFuzzyLengthMax bytes of `haystack` count.
[[nodiscard]] float FuzzyScore(std::string_view needle, std::string_view haystack) noexcept;
// Every space-separated term of `query` must match; their scores add up.
[[nodiscard]] float FuzzyScoreTerms(std::string_view query, std::string_view haystack) noexcept;

} // namespace Midori
