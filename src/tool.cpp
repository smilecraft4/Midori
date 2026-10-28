#include "midori/tool.hpp"

#include "midori/platform.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace Midori {

namespace {

std::uint32_t ToolIdNext(const ToolsState& tools) noexcept {
    std::uint32_t id = 0;
    for (const ToolState& tool : tools.library) {
        id = std::max(id, tool.id);
    }
    return id + 1;
}

} // namespace

ToolState* ToolFind(ToolsState& tools, std::uint32_t id) noexcept {
    const auto found = std::ranges::find(tools.library, id, &ToolState::id);
    return found == tools.library.end() ? nullptr : &*found;
}

const ToolState* ToolFind(const ToolsState& tools, std::uint32_t id) noexcept {
    const auto found = std::ranges::find(tools.library, id, &ToolState::id);
    return found == tools.library.end() ? nullptr : &*found;
}

ToolState& ToolCurrent(ToolsState& tools) noexcept {
    ToolState* tool = ToolFind(tools, tools.current);
    return tool ? *tool : tools.library.front();
}

const ToolState& ToolCurrent(const ToolsState& tools) noexcept {
    const ToolState* tool = ToolFind(tools, tools.current);
    return tool ? *tool : tools.library.front();
}

ToolbarSlot* ToolbarSlotFind(ToolsState& tools, std::uint32_t id) noexcept {
    const auto found = std::ranges::find(tools.slots, id, &ToolbarSlot::id);
    return found == tools.slots.end() ? nullptr : &*found;
}

void ToolPick(ToolsState& tools, std::uint32_t id) noexcept {
    if (!ToolFind(tools, id)) {
        return;
    }
    tools.current = id;
    ToolbarSlot* slot = ToolbarSlotFind(tools, tools.slot);
    if (!slot || !std::ranges::contains(slot->tools, id)) {
        const auto holding =
            std::ranges::find_if(tools.slots, [&](const ToolbarSlot& s) { return std::ranges::contains(s.tools, id); });
        if (holding == tools.slots.end()) {
            return;
        }
        slot = &*holding;
        tools.slot = slot->id;
    }
    slot->last = id;
}

void ToolbarSlotPress(ToolsState& tools, std::uint32_t id) noexcept {
    const ToolbarSlot* slot = ToolbarSlotFind(tools, id);
    if (!slot) {
        return;
    }
    std::uint32_t pick = std::ranges::contains(slot->tools, slot->last) ? slot->last : slot->tools.front();
    if (const auto at = std::ranges::find(slot->tools, tools.current); tools.slot == id && at != slot->tools.end()) {
        pick = at + 1 == slot->tools.end() ? slot->tools.front() : *(at + 1);
    }
    tools.slot = id;
    ToolPick(tools, pick);
}

std::uint32_t ToolbarSlotAdd(ToolsState& tools, std::uint32_t tool) noexcept {
    std::uint32_t id = 0;
    for (const ToolbarSlot& slot : tools.slots) {
        id = std::max(id, slot.id);
    }
    tools.slots.push_back(ToolbarSlot{.tools = {tool}, .id = id + 1, .last = tool});
    return id + 1;
}

std::uint32_t ToolDuplicate(ToolsState& tools, std::uint32_t id) noexcept {
    const auto source = std::ranges::find(tools.library, id, &ToolState::id);
    if (source == tools.library.end()) {
        return 0;
    }
    ToolState copy = *source;
    copy.id = ToolIdNext(tools);
    tools.library.insert(source + 1, copy);
    if (ToolbarSlot* slot = ToolbarSlotFind(tools, tools.slot)) {
        if (const auto at = std::ranges::find(slot->tools, id); at != slot->tools.end()) {
            slot->tools.insert(at + 1, copy.id);
        }
    }
    return copy.id;
}

void ToolDelete(ToolsState& tools, std::uint32_t id) noexcept {
    if (tools.library.size() <= 1) {
        return;
    }
    std::erase_if(tools.library, [&](const ToolState& tool) { return tool.id == id; });
    for (ToolbarSlot& slot : tools.slots) {
        std::erase(slot.tools, id);
    }
    ToolsFix(tools);
}

void ToolsFix(ToolsState& tools) noexcept {
    for (ToolbarSlot& slot : tools.slots) {
        std::erase_if(slot.tools, [&](std::uint32_t id) { return !ToolFind(tools, id); });
        if (!slot.tools.empty() && !std::ranges::contains(slot.tools, slot.last)) {
            slot.last = slot.tools.front();
        }
    }
    std::erase_if(tools.slots, [](const ToolbarSlot& slot) { return slot.tools.empty(); });
    const ToolbarSlot* slot = ToolbarSlotFind(tools, tools.slot);
    if (!slot) {
        tools.slot = 0;
    }
    if (!ToolFind(tools, tools.current)) {
        tools.current = slot ? slot->last : tools.library.front().id;
    }
}

const MdslBrush& ToolSource(const ToolsState& tools, const ToolState& tool) noexcept {
    return tools.shaders[tool.shader].brush;
}

std::optional<MdslBrush> ToolShaderRead(const std::filesystem::path& path, std::string& error) noexcept {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = std::format("{}: cannot be read", PathUtf8(path.filename()));
        return std::nullopt;
    }
    std::stringstream text;
    text << file.rdbuf();
    auto brush = MdslTranslate(text.str());
    if (!brush) {
        error = std::format("{}:{}:{}: {}", PathUtf8(path.filename()), brush.error().line, brush.error().column,
                            brush.error().message);
        return std::nullopt;
    }
    error.clear();
    return std::move(*brush);
}

std::uint32_t ToolShaderAdd(ToolsState& tools, const std::filesystem::path& path) noexcept {
    if (path.empty()) {
        return 0;
    }
    if (const auto found = std::ranges::find(tools.shaders, path, &ToolShader::path); found != tools.shaders.end()) {
        return static_cast<std::uint32_t>(found - tools.shaders.begin());
    }
    if (tools.shaders.size() >= kToolShadersMax) {
        return kToolShadersMax;
    }
    ToolShader shader{.path = path};
    std::error_code ec;
    shader.writeTime = std::filesystem::last_write_time(path, ec);
    auto brush = ToolShaderRead(path, shader.error);
    shader.brush = brush ? std::move(*brush) : tools.shaders.front().brush;
    tools.shaders.push_back(std::move(shader));
    return static_cast<std::uint32_t>(tools.shaders.size() - 1);
}

} // namespace Midori
