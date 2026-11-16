#include "test.hpp"

#include "midori/action.hpp"

namespace Midori {

namespace {

// Parsed as keybinds.json reads it; an invalid one leaves `action` on a binding no key reaches.
[[nodiscard]] KeyBinding Bind(std::string_view keys, ActionId action, ActionContext context) noexcept {
    KeyBinding binding = KeyBindingParse(keys, kActions[std::to_underlying(action)].kind).value_or(KeyBinding{});
    binding.action = action;
    binding.context = context;
    return binding;
}

} // namespace

void TestKeymap(TestContext& t) noexcept {
    const auto sequence = KeyBindingParse("Ctrl+K, N", ActionKind::Press);
    if (MIDORI_CHECK(t, sequence.has_value())) {
        MIDORI_CHECK(t, sequence->length == 2 && sequence->trigger == ActionKind::Press);
        MIDORI_CHECK(t, sequence->chords[0].key == Key::K && sequence->chords[0].mods == KeyMod::Ctrl);
        MIDORI_CHECK(t, sequence->chords[1].key == Key::N && sequence->chords[1].mods == KeyMod::None);
    }
    // Modifiers alone make a Hold binding; names ignore case.
    const auto hold = KeyBindingParse("Shift(Hold)", ActionKind::Press);
    if (MIDORI_CHECK(t, hold.has_value())) {
        MIDORI_CHECK(t, hold->trigger == ActionKind::Hold && hold->length == 1);
        MIDORI_CHECK(t, hold->chords[0].key == Key::Unknown && hold->chords[0].mods == KeyMod::Shift);
    }
    const auto mixed = KeyBindingParse("shift+alt+x", ActionKind::Press);
    MIDORI_CHECK(t, mixed && mixed->chords[0].key == Key::X && mixed->chords[0].mods == (KeyMod::Shift | KeyMod::Alt));
    for (const std::string_view invalid :
         {"", "Ctrl+", "Bogus", "Ctrl+K, ", "Ctrl", "K(Sometimes)", "A, B, C, D, E", "Shift, K(Hold)"}) {
        MIDORI_CHECK(t, !KeyBindingParse(invalid, ActionKind::Press));
    }
    // Written back as keybinds.json writes it, whatever the layout.
    MIDORI_CHECK(t,
                 KeyBindingKeys(Bind("ctrl+shift+s", ActionId::CanvasSaveAs, ActionContext::Global)) == "Ctrl+Shift+S");

    KeymapState keymap{};
    keymap.bindings = {
        Bind("Ctrl+K, N", ActionId::WindowNew, ActionContext::Global),
        Bind("Ctrl+Z", ActionId::HistoryUndo, ActionContext::Global),
        Bind("B", ActionId::ToolbarSlot, ActionContext::Canvas),
    };
    // A sequence waits for its next chord, which either side's modifier starts.
    MIDORI_CHECK(t, KeymapPress(keymap, {.key = Key::K, .mods = KeyMod::LCtrl}, ActionContext::Canvas) == -1);
    MIDORI_CHECK(t, keymap.pendingLength == 1);
    MIDORI_CHECK(t, KeymapContinues(keymap, keymap.bindings[0]) && !KeymapContinues(keymap, keymap.bindings[1]));
    MIDORI_CHECK(t, KeymapPress(keymap, {.key = Key::N, .mods = KeyMod::None}, ActionContext::Canvas) == 0);
    MIDORI_CHECK(t, keymap.pendingLength == 0);
    MIDORI_CHECK(t, KeymapPress(keymap, {.key = Key::Z, .mods = KeyMod::RCtrl}, ActionContext::Global) == 1);
    // A Canvas binding only works with a canvas.
    MIDORI_CHECK(t, KeymapPress(keymap, {.key = Key::B, .mods = KeyMod::None}, ActionContext::Global) == -1);
    MIDORI_CHECK(t, keymap.pendingLength == 0);
    MIDORI_CHECK(t, KeymapPress(keymap, {.key = Key::B, .mods = KeyMod::None}, ActionContext::Canvas) == 2);

    KeymapState held{};
    held.bindings = {
        Bind("Alt", ActionId::ToolEyedropper, ActionContext::Canvas),
        Bind("Shift(Hold)", ActionId::SelectionOp, ActionContext::Canvas),
        Bind("Shift+Alt(Hold)", ActionId::SelectionOp, ActionContext::Canvas),
    };
    held.bindings[1].arg = 1;
    held.bindings[2].arg = 3;
    MIDORI_CHECK(t, KeymapHeld(held, ActionId::ToolEyedropper, ActionContext::Canvas, KeyMod::LAlt) == 0);
    MIDORI_CHECK(t, KeymapHeld(held, ActionId::ToolEyedropper, ActionContext::Canvas, KeyMod::None) == -1);
    MIDORI_CHECK(t, KeymapHeld(held, ActionId::SelectionOp, ActionContext::Canvas, KeyMod::LShift) == 1);
    MIDORI_CHECK(t,
                 KeymapHeld(held, ActionId::SelectionOp, ActionContext::Canvas, KeyMod::LShift | KeyMod::RAlt) == 2);
}

} // namespace Midori
