#pragma once

#include "midori/action.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace Midori {

enum struct Text : std::uint8_t {
    Untitled,
    AskSave,
    AskSaveContent,
    AskSaveYes,
    AskSaveNo,
    ErrorReason,
    ErrorFileDialog,
    ErrorSave,
    ErrorOpen,
    ErrorCreate,
    Tool,
    Paint,
    Erase,
    Color,
    Blend,
    Removes,
    Radius,
    Hardness,
    Flow,
    Opacity,
    Spacing,
    PressureRadius,
    PressureHardness,
    PressureFlow,
    PressureOpacity,
    PressureMin,
    PressureMax,
    AdjustRadius,
    AdjustOpacity,
    AdjustHardness,
    AdjustLayerOpacity,
    BlendNormal,
    BlendMultiply,
    BlendAdd,
    BlendScreen,
    BlendNormalOkLab,
    Layers,
    NoCanvas,
    Merging,
    LayerNew,
    FolderNew,
    LayerDelete,
    LayerUp,
    LayerDown,
    LayerMerge,
    Settings,
    SettingsDisplay,
    Language,
    LanguageSystem,
    CanvasFilter,
    CanvasFilterTip,
    FilterNearest,
    FilterBilinear,
    FilterBicubic,
    FilterMitchell,
    FilterLanczos2,
    FilterLanczos3,
    Help,
    HelpGeneral,
    HelpLayers,
    HelpAdjustRadius,
    HelpAdjustOpacity,
    HelpAdjustHardness,
    HelpAdjustLayerOpacity,
    HelpNavigating,
    HelpErasing,
    HelpPainting,
    HelpEraser,
    HelpPaintBrush,
    File,
    New,
    Open,
    Save,
    SaveAs,
    OpenRecent,
    RecentClear,
    Exit,
    Edit,
    Undo,
    Redo,
    SettingsMenu,
    WindowMenu,
    Fullscreen,
    HideUi,
    View,
    Debug,
    TileAtlas,
    TileCells,
    ViewZoom,
    ViewCenter,
    ViewPointer,
    FlipX,
    FlipY,
    Reset,
    Background,
    GestureLeftDrag,
    GesturePaint,
    GestureErase,
    GestureRelease,
    GestureFinishStroke,
    GesturePan,
    GestureCtrlDrag,
    GestureZoom,
    GestureShiftDrag,
    GestureRotate,
    GestureAlt,
    GestureSnapView,
    GestureMove,
    GestureAdjustMove,
    GestureShift,
    GestureSlower,
    GestureSnapValue,
    GestureKeep,
    GestureKeepValue,
    GestureCancel,
    GestureRestore,
    SearchHint,
    ReasonNoCanvas,
    ReasonNoLayer,
    ReasonOneLayer,
    ReasonMerging,
    ReasonPainting,
    ReasonNoUndo,
    ReasonNoRedo,
    ReasonNoPreset,
    ReasonNoRecent,
    ReasonNoViewBack,
    ReasonNoViewForward,
    ReasonHold,
    Select,
    HelpSelect,
    HelpSelecting,
    GestureSelect,
    GestureShiftSelect,
    GestureAltSelect,
    GestureShiftDragOnly,
    GestureAltDrag,
    ReasonNoSelection,
    SelectionOpLabel,
    SelectionReplace,
    SelectionAdd,
    SelectionSubtract,
    SelectionIntersect,
    SelectionExclude,
    GestureShiftAltDrag,
    GestureShiftAltSelect,
    Count_,
};

// The key names the text in a language file and is its Dear ImGui id. Format specifiers are printf's; a translation
// must keep the same ones in the same order.
struct TextInfo {
    const char* key;
    const char* english;
};

constexpr std::array<TextInfo, std::to_underlying(Text::Count_)> kTexts = {{
    {.key = "untitled", .english = "Untitled"},
    {.key = "askSave", .english = "Do you want to save changes to %s?"},
    {.key = "askSaveContent", .english = "Your changes will be lost if you don't save them."},
    {.key = "askSaveYes", .english = "Save"},
    {.key = "askSaveNo", .english = "Don't Save"},
    {.key = "errorReason", .english = "Reason: %s."},
    {.key = "errorFileDialog", .english = "Could not show the file dialog"},
    {.key = "errorSave", .english = "Could not save %s"},
    {.key = "errorOpen", .english = "Could not open %s"},
    {.key = "errorCreate", .english = "Could not create a canvas"},
    {.key = "tool", .english = "Tool"},
    {.key = "paint", .english = "Paint"},
    {.key = "erase", .english = "Erase"},
    {.key = "color", .english = "Color"},
    {.key = "blend", .english = "Blend"},
    {.key = "removes", .english = "Removes"},
    {.key = "radius", .english = "Radius"},
    {.key = "hardness", .english = "Hardness"},
    {.key = "flow", .english = "Flow"},
    {.key = "opacity", .english = "Opacity"},
    {.key = "spacing", .english = "Spacing"},
    {.key = "pressureRadius", .english = "Pressure radius"},
    {.key = "pressureHardness", .english = "Pressure hardness"},
    {.key = "pressureFlow", .english = "Pressure flow"},
    {.key = "pressureOpacity", .english = "Pressure opacity"},
    {.key = "pressureMin", .english = "Min %.2f"},
    {.key = "pressureMax", .english = "Max %.2f"},
    {.key = "adjustRadius", .english = "Radius"},
    {.key = "adjustOpacity", .english = "Opacity"},
    {.key = "adjustHardness", .english = "Hardness"},
    {.key = "adjustLayerOpacity", .english = "Layer opacity"},
    {.key = "blendNormal", .english = "Normal"},
    {.key = "blendMultiply", .english = "Multiply"},
    {.key = "blendAdd", .english = "Add"},
    {.key = "blendScreen", .english = "Screen"},
    {.key = "blendNormalOkLab", .english = "Normal (OkLab)"},
    {.key = "layers", .english = "Layers"},
    {.key = "noCanvas", .english = "No canvas"},
    {.key = "merging", .english = "Merging, %d tiles left"},
    {.key = "layerNew", .english = "+ Layer"},
    {.key = "folderNew", .english = "+ Folder"},
    {.key = "layerDelete", .english = "Delete"},
    {.key = "layerUp", .english = "Up"},
    {.key = "layerDown", .english = "Down"},
    {.key = "layerMerge", .english = "Merge"},
    {.key = "settings", .english = "Settings"},
    {.key = "settingsDisplay", .english = "Display"},
    {.key = "language", .english = "Language"},
    {.key = "languageSystem", .english = "System"},
    {.key = "canvasFilter", .english = "Canvas filter"},
    {.key = "canvasFilterTip",
     .english = "How the canvas is drawn between its pixels: wider filters stay sharp when rotated or zoomed between "
                "steps, Nearest keeps hard pixels."},
    {.key = "filterNearest", .english = "Nearest"},
    {.key = "filterBilinear", .english = "Bilinear"},
    {.key = "filterBicubic", .english = "Bicubic (Catmull-Rom)"},
    {.key = "filterMitchell", .english = "Mitchell"},
    {.key = "filterLanczos2", .english = "Lanczos 2"},
    {.key = "filterLanczos3", .english = "Lanczos 3"},
    {.key = "help", .english = "Help"},
    {.key = "helpGeneral", .english = "Everywhere"},
    {.key = "helpLayers", .english = "Layers"},
    {.key = "helpAdjustRadius", .english = "Adjusting the brush radius"},
    {.key = "helpAdjustOpacity", .english = "Adjusting the brush opacity"},
    {.key = "helpAdjustHardness", .english = "Adjusting the brush hardness"},
    {.key = "helpAdjustLayerOpacity", .english = "Adjusting the layer opacity"},
    {.key = "helpNavigating", .english = "Moving the view"},
    {.key = "helpErasing", .english = "Erasing"},
    {.key = "helpPainting", .english = "Painting"},
    {.key = "helpEraser", .english = "Eraser"},
    {.key = "helpPaintBrush", .english = "Paint brush"},
    {.key = "file", .english = "File"},
    {.key = "new", .english = "New"},
    {.key = "open", .english = "Open..."},
    {.key = "save", .english = "Save"},
    {.key = "saveAs", .english = "Save As..."},
    {.key = "openRecent", .english = "Open Recent"},
    {.key = "recentClear", .english = "Clear"},
    {.key = "exit", .english = "Exit"},
    {.key = "edit", .english = "Edit"},
    {.key = "undo", .english = "Undo"},
    {.key = "redo", .english = "Redo"},
    {.key = "settingsMenu", .english = "Settings..."},
    {.key = "window", .english = "Window"},
    {.key = "fullscreen", .english = "Fullscreen"},
    {.key = "hideUi", .english = "Hide UI"},
    {.key = "view", .english = "View"},
    {.key = "debug", .english = "Debug"},
    {.key = "tileAtlas", .english = "Tile atlas"},
    {.key = "tileCells", .english = "Tile cells on canvas"},
    {.key = "viewZoom", .english = "Zoom %.1f%%  Rotation %.1f deg"},
    {.key = "viewCenter", .english = "Center %.1f, %.1f"},
    {.key = "viewPointer", .english = "Pointer %.1f, %.1f"},
    {.key = "flipX", .english = "Flip X"},
    {.key = "flipY", .english = "Flip Y"},
    {.key = "reset", .english = "Reset"},
    {.key = "background", .english = "Background"},
    {.key = "gestureLeftDrag", .english = "Left drag"},
    {.key = "gesturePaint", .english = "Paint with the brush"},
    {.key = "gestureErase", .english = "Erase with the brush; its color picks the channels removed"},
    {.key = "gestureRelease", .english = "Release"},
    {.key = "gestureFinishStroke", .english = "Finish the stroke"},
    {.key = "gesturePan", .english = "Pan"},
    {.key = "gestureCtrlDrag", .english = "Ctrl + left drag"},
    {.key = "gestureZoom", .english = "Zoom, right or up zooms in"},
    {.key = "gestureShiftDrag", .english = "Shift + left drag"},
    {.key = "gestureRotate", .english = "Rotate around the center"},
    {.key = "gestureAlt", .english = "Alt"},
    {.key = "gestureSnapView", .english = "Snap the zoom to texel-friendly steps, the rotation to 15 degrees"},
    {.key = "gestureMove", .english = "Move"},
    {.key = "gestureAdjustMove", .english = "Away from the center raises the value, toward it lowers it"},
    {.key = "gestureShift", .english = "Shift"},
    {.key = "gestureSlower", .english = "Change it 10 times slower"},
    {.key = "gestureSnapValue", .english = "Snap it to powers of two, a layer opacity to steps of 5 percent"},
    {.key = "gestureKeep", .english = "Left click, Enter"},
    {.key = "gestureKeepValue", .english = "Keep the value"},
    {.key = "gestureCancel", .english = "Right click, Escape"},
    {.key = "gestureRestore", .english = "Restore the previous value"},
    {.key = "searchHint", .english = "Search actions, layers, files..."},
    {.key = "reasonNoCanvas", .english = "Needs a canvas in this window"},
    {.key = "reasonNoLayer", .english = "Needs an active layer"},
    {.key = "reasonOneLayer", .english = "Needs another layer"},
    {.key = "reasonMerging", .english = "Waits for the layer merge to finish"},
    {.key = "reasonPainting", .english = "Not while painting"},
    {.key = "reasonNoUndo", .english = "Nothing to undo"},
    {.key = "reasonNoRedo", .english = "Nothing to redo"},
    {.key = "reasonNoPreset", .english = "No such brush preset"},
    {.key = "reasonNoRecent", .english = "No recent files"},
    {.key = "reasonNoViewBack", .english = "No previous view"},
    {.key = "reasonNoViewForward", .english = "No next view"},
    {.key = "reasonHold", .english = "Only while its keys are held"},
    {.key = "select", .english = "Select"},
    {.key = "helpSelect", .english = "Rectangle selection"},
    {.key = "helpSelecting", .english = "Selecting"},
    {.key = "gestureSelect", .english = "Select a rectangle by the operation"},
    {.key = "gestureShiftSelect", .english = "Add a rectangle to the selection"},
    {.key = "gestureAltSelect", .english = "Remove a rectangle from the selection"},
    {.key = "gestureShiftDragOnly", .english = "Shift + left drag"},
    {.key = "gestureAltDrag", .english = "Alt + left drag"},
    {.key = "reasonNoSelection", .english = "Nothing is selected"},
    {.key = "selectionOp", .english = "Operation"},
    {.key = "selectionReplace", .english = "Replace"},
    {.key = "selectionAdd", .english = "Add"},
    {.key = "selectionSubtract", .english = "Subtract"},
    {.key = "selectionIntersect", .english = "Intersect"},
    {.key = "selectionExclude", .english = "Exclude"},
    {.key = "gestureShiftAltDrag", .english = "Shift + Alt + left drag"},
    {.key = "gestureShiftAltSelect", .english = "Keep only what a rectangle selects of the selection"},
}};

struct LanguageInfo {
    const char* code; // lang/<code>.json beside the executable; none for English
    const char* name; // in the language itself
};

constexpr std::array kLanguages = {
    LanguageInfo{.code = "en", .name = "English"},
    LanguageInfo{.code = "fr", .name = "Français"},
    LanguageInfo{.code = "ja", .name = "日本語"},
};

// Each text, then each action description, as `text` and as the Dear ImGui label `text###key`, null terminated in one
// arena. Owned by the main thread; pointers into it last until the next TextLoad.
struct TextState {
    std::string arena;
    std::array<std::uint32_t, std::to_underlying(Text::Count_)> texts;
    std::array<std::uint32_t, std::to_underlying(Text::Count_)> labels;
    std::array<std::uint32_t, std::to_underlying(ActionId::Count_)> actions;
    std::string language; // the code loaded
};

extern TextState gTextState;

// A text the language file misses, or translates with other format specifiers, stays English and is logged. A code
// with no file loads English.
void TextLoad(std::string_view language) noexcept;

[[nodiscard]] inline const char* TextGet(Text text) noexcept {
    return gTextState.arena.data() + gTextState.texts[std::to_underlying(text)];
}

[[nodiscard]] inline const char* TextLabel(Text text) noexcept {
    return gTextState.arena.data() + gTextState.labels[std::to_underlying(text)];
}

[[nodiscard]] inline const char* TextAction(ActionId action) noexcept {
    return gTextState.arena.data() + gTextState.actions[std::to_underlying(action)];
}

// TextLoad checked the specifiers against the English ones, which the arguments match.
template<class... Args>
[[nodiscard]] std::string TextFormat(Text text, Args... args) noexcept {
    const char* format = TextGet(text);
    const int size = std::snprintf(nullptr, 0, format, args...);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    std::snprintf(result.data(), result.size() + 1, format, args...);
    return result;
}

} // namespace Midori
