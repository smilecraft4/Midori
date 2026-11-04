#include "midori/layer.hpp"

#include "midori/app.hpp"
#include "midori/history.hpp"
#include "midori/log.hpp"
#include "midori/tile.hpp"

#include <algorithm>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include <tracy/Tracy.hpp>

namespace Midori {

namespace {

[[nodiscard]] bool LayerIsFolder(const CanvasState& canvas, Layer layer) noexcept {
    return HasAnyFlag(canvas.layers[layer].flags, LayerFlags::Folder);
}

// In the order and not the root.
[[nodiscard]] bool LayerEditable(const CanvasState& canvas, Layer layer) noexcept {
    return layer < kLayerMax && canvas.layerHeights[layer] != kLayerInvalid
        && !HasAnyFlag(canvas.layers[layer].flags, LayerFlags::Internal);
}

// The folder a layer lands in at `height`, see LayerCreateInfo.
[[nodiscard]] Layer LayerParentAt(const CanvasState& canvas, Layer height) noexcept {
    const Layer at = canvas.layersOrder[height];
    return LayerIsFolder(canvas, at) ? at : canvas.layerParents[at];
}

// The smallest free id, else the next one; `wanted` when given, which must be free.
[[nodiscard]] Layer LayerIdTake(CanvasState& canvas, Layer wanted) noexcept {
    if (wanted == kLayerInvalid) {
        if (!canvas.freeLayers.empty()) {
            const Layer id = canvas.freeLayers.back();
            canvas.freeLayers.pop_back();
            return id;
        }
        return canvas.largestLayer == kLayerMax ? kLayerInvalid : canvas.largestLayer++;
    }
    MIDORI_ASSERT(wanted < kLayerMax, "layer id {}", wanted);
    if (wanted >= canvas.largestLayer) {
        // Ascending at the front keeps the list largest first.
        for (Layer id = canvas.largestLayer; id < wanted; ++id) {
            canvas.freeLayers.insert(canvas.freeLayers.begin(), id);
        }
        canvas.largestLayer = static_cast<Layer>(wanted + 1);
        return wanted;
    }
    const auto found = std::ranges::find(canvas.freeLayers, wanted);
    MIDORI_ASSERT(found != canvas.freeLayers.end(), "layer {} in use", wanted);
    canvas.freeLayers.erase(found);
    return wanted;
}

// Before the op changes it, once per layer.
void LayerRemember(const CanvasState& canvas, std::vector<HistoryLayer>& changed, Layer layer) noexcept {
    if (std::ranges::contains(changed, layer, &HistoryLayer::id)) {
        return;
    }
    changed.push_back(HistoryLayer{
        .before = canvas.layers[layer],
        .after = {},
        .beforeName = canvas.layerNames[layer],
        .afterName = {},
        .id = layer,
    });
}

// `folder` and every folder around it hold `count` more layers.
void LayerDescendantsAdd(CanvasState& canvas, std::vector<HistoryLayer>& changed, Layer folder, int count) noexcept {
    for (Layer f = folder; f != kLayerInvalid; f = canvas.layerParents[f]) {
        LayerRemember(canvas, changed, f);
        LayerState& state = canvas.layers[f];
        state.descendants = static_cast<Layer>(state.descendants + count);
        state.flags |= LayerFlags::Dirty;
    }
}

void LayerPropertiesApply(CanvasState& canvas, Layer layer, const LayerProperties& properties) noexcept {
    LayerState& state = canvas.layers[layer];
    state.opacity = properties.opacity;
    state.blendMode = properties.blendMode;
    state.flags = properties.hidden ? state.flags | LayerFlags::Hidden : state.flags & ~LayerFlags::Hidden;
    state.flags = properties.locked ? state.flags | LayerFlags::Locked : state.flags & ~LayerFlags::Locked;
    state.flags |= LayerFlags::Dirty;
    canvas.layerNames[layer] = properties.name;
    canvas.layerNames[layer].back() = '\0';
    LayerCompositesDirty(canvas, canvas.layerParents[layer]);
    canvas.modified = true;
}

} // namespace

std::array<char, kLayerNameLenMax> LayerName(std::string_view text) noexcept {
    std::array<char, kLayerNameLenMax> name{};
    std::ranges::copy(text.substr(0, kLayerNameLenMax - 1), name.begin());
    return name;
}

Layer LayerSelectionTake(CanvasState& canvas) noexcept {
    if (canvas.selection != kLayerInvalid) {
        return canvas.selection;
    }
    const Layer id = LayerIdTake(canvas, kLayerInvalid);
    if (id == kLayerInvalid) {
        return id;
    }
    canvas.layers[id] = LayerState{.flags = LayerFlags::Mask};
    canvas.layerNames[id] = LayerName("Selection");
    canvas.selection = id;
    return id;
}

void LayersIndex(CanvasState& canvas) noexcept {
    ZoneScopedN("LayersIndex");
    canvas.layerHeights.fill(kLayerInvalid);
    canvas.layerParents.fill(kLayerInvalid);
    struct Open {
        Layer folder;
        Layer bottom;       // its lowest height
    };
    std::vector<Open> open; // the folders around the height, innermost last
    for (std::size_t h = canvas.layersOrder.size(); h-- > 0;) {
        const Layer id = canvas.layersOrder[h];
        const auto height = static_cast<Layer>(h);
        while (!open.empty() && open.back().bottom > height) {
            open.pop_back();
        }
        canvas.layerHeights[id] = height;
        canvas.layerParents[id] = open.empty() ? kLayerInvalid : open.back().folder;
        if (LayerIsFolder(canvas, id)) {
            open.push_back(Open{.folder = id, .bottom = static_cast<Layer>(height - canvas.layers[id].descendants)});
        }
    }
}

void LayersInit(CanvasState& canvas) noexcept {
    ZoneScopedN("LayersInit");
    constexpr Layer kFirst = 0;
    constexpr Layer kRoot = 1;
    canvas.layers[kFirst] = LayerState{.flags = LayerFlags::Dirty};
    canvas.layerNames[kFirst] = LayerName("Layer 1");
    canvas.layers[kRoot] = LayerState{
        .descendants = 1,
        .flags = LayerFlags::Folder | LayerFlags::Internal | LayerFlags::Dirty,
    };
    canvas.layerNames[kRoot] = LayerName("Canvas");
    canvas.layersOrder = {kFirst, kRoot};
    canvas.freeLayers.clear();
    canvas.largestLayer = 2;
    LayersIndex(canvas);
    canvas.activeLayer = kFirst;
}

void LayerCompositesDirty(CanvasState& canvas, Layer folder) noexcept {
    ZoneScopedN("LayerCompositesDirty");
    std::array<bool, kLayerMax> folders{};
    if (folder == kLayerInvalid) {
        folders.fill(true);
    }
    for (Layer f = folder; f != kLayerInvalid; f = canvas.layerParents[f]) {
        folders[f] = true;
    }
    for (TileState& tile : canvas.tiles) {
        if (HasAnyFlag(tile.flags, TileFlags::Composite) && folders[TileKeyLayer(tile.key)]) {
            tile.flags |= TileFlags::Dirty;
        }
    }
}

void LayerCompositesTouch(CanvasState& canvas, TileKey key) noexcept {
    const TileKey gridPos = TileGridPosKey(TileKeyWithMip(key, 0));
    for (Layer f = canvas.layerParents[TileKeyLayer(key)]; f != kLayerInvalid; f = canvas.layerParents[f]) {
        if (const auto found = canvas.tileIndices.find(TileGridPosLayerKey(gridPos, f));
            found != canvas.tileIndices.end()) {
            canvas.tiles[found->second].flags |= TileFlags::Dirty;
        }
    }
}

std::vector<Layer> LayersSelected(const CanvasState& canvas) noexcept {
    std::vector<Layer> selected;
    for (const Layer layer : canvas.selectedLayers) {
        if (LayerEditable(canvas, layer) && !std::ranges::contains(selected, layer)) {
            selected.push_back(layer);
        }
    }
    if (selected.empty() && LayerEditable(canvas, canvas.activeLayer)) {
        selected.push_back(canvas.activeLayer);
    }
    std::ranges::sort(selected, std::greater{}, [&canvas](Layer layer) { return canvas.layerHeights[layer]; });
    return selected;
}

bool LayerOpsBlocked(const CanvasState& canvas) noexcept {
    return canvas.merge.result != kLayerInvalid || gAppState.stroke.canvas == canvas.id || canvas.history.pending != 0;
}

bool LayerPaintable(const CanvasState& canvas, Layer layer) noexcept {
    if (!LayerEditable(canvas, layer)
        || HasAnyFlag(canvas.layers[layer].flags, LayerFlags::Folder | LayerFlags::Hidden | LayerFlags::Locked)) {
        return false;
    }
    for (Layer parent = canvas.layerParents[layer]; parent != kLayerInvalid; parent = canvas.layerParents[parent]) {
        if (HasAnyFlag(canvas.layers[parent].flags, LayerFlags::Locked)) {
            return false;
        }
    }
    return true;
}

Result<Layer> LayerNew(Canvas canvasId, const LayerCreateInfo& createInfo) noexcept {
    ZoneScopedN("LayerNew");
    CanvasState& canvas = gAppState.canvases[canvasId];
    if (LayerOpsBlocked(canvas)) {
        return std::unexpected(Error::CanvasBusy);
    }
    if (createInfo.height >= canvas.layersOrder.size()
        || HasAnyFlag(createInfo.flags, LayerFlags::Internal | LayerFlags::Deleted | LayerFlags::Purged)) {
        return std::unexpected(Error::LayerOpInvalid);
    }
    const Layer id = LayerIdTake(canvas, createInfo.id);
    if (id == kLayerInvalid) {
        MIDORI_LOG_WARN("LayerNew: all {} layer ids are in use", kLayerMax);
        return std::unexpected(Error::LayerOpInvalid);
    }
    const std::vector<Layer> orderBefore = canvas.layersOrder;
    std::vector<HistoryLayer> changed;
    // Deleted before it was made, so undoing deletes it again.
    canvas.layers[id] = LayerState{.flags = LayerFlags::Deleted};
    canvas.layerNames[id] = createInfo.name;
    canvas.layerNames[id].back() = '\0';
    LayerRemember(canvas, changed, id);
    LayerDescendantsAdd(canvas, changed, LayerParentAt(canvas, createInfo.height), 1);
    canvas.layersOrder.insert(canvas.layersOrder.begin() + createInfo.height, id);
    canvas.layers[id] = LayerState{
        .opacity = createInfo.opacity,
        .blendMode = createInfo.blendMode,
        .flags = createInfo.flags | LayerFlags::Dirty,
    };
    LayersIndex(canvas);
    canvas.modified = true;
    HistoryPushLayers(canvasId, orderBefore, changed);
    return id;
}

Result<void> LayerDelete(Canvas canvasId, std::span<const Layer> layers) noexcept {
    ZoneScopedN("LayerDelete");
    CanvasState& canvas = gAppState.canvases[canvasId];
    if (LayerOpsBlocked(canvas)) {
        return std::unexpected(Error::CanvasBusy);
    }
    // A layer inside a folder deleted goes with it.
    std::vector<Layer> tops;
    for (const Layer layer : layers) {
        if (!LayerEditable(canvas, layer)) {
            return std::unexpected(Error::LayerOpInvalid);
        }
        bool inside = false;
        for (Layer p = canvas.layerParents[layer]; p != kLayerInvalid; p = canvas.layerParents[p]) {
            inside = inside || std::ranges::contains(layers, p);
        }
        if (!inside && !std::ranges::contains(tops, layer)) {
            tops.push_back(layer);
        }
    }
    if (tops.empty()) {
        return {};
    }
    // Highest first: deleting one leaves the heights below it as they were.
    std::ranges::sort(tops, std::greater{}, [&canvas](Layer layer) { return canvas.layerHeights[layer]; });
    const std::vector<Layer> orderBefore = canvas.layersOrder;
    std::vector<HistoryLayer> changed;
    std::vector<Layer> parents;
    Layer bottom = 0;
    for (const Layer layer : tops) {
        const Layer top = canvas.layerHeights[layer];
        const auto count = static_cast<Layer>(canvas.layers[layer].descendants + 1);
        bottom = static_cast<Layer>(top + 1 - count);
        parents.push_back(canvas.layerParents[layer]);
        LayerDescendantsAdd(canvas, changed, canvas.layerParents[layer], -count);
        for (Layer h = bottom; h <= top; ++h) {
            const Layer id = canvas.layersOrder[h];
            LayerRemember(canvas, changed, id);
            canvas.layers[id].flags |= LayerFlags::Deleted | LayerFlags::Dirty;
        }
        canvas.layersOrder.erase(canvas.layersOrder.begin() + bottom, canvas.layersOrder.begin() + top + 1);
        LayersIndex(canvas);
    }
    // Derived from what is gone; composited again if the folder comes back.
    std::vector<TileKey> composites;
    for (const TileState& tile : canvas.tiles) {
        if (HasAnyFlag(tile.flags, TileFlags::Composite)
            && HasAnyFlag(canvas.layers[TileKeyLayer(tile.key)].flags, LayerFlags::Deleted)) {
            composites.push_back(tile.key);
        }
    }
    for (const TileKey key : composites) {
        const Tile tile{
            .canvas = canvasId, .mip = TileKeyMip(key), .layer = TileKeyLayer(key), .gridPos = TileKeyGridPos(key)};
        if (!TileDelete(tile)) {
            MIDORI_LOG_WARN("LayerDelete: composite tile {:#018x} not deleted", key);
        }
    }
    for (const Layer parent : parents) {
        LayerCompositesDirty(canvas, parent);
    }
    if (canvas.activeLayer != kLayerInvalid && canvas.layerHeights[canvas.activeLayer] == kLayerInvalid) {
        // What was right above the lowest deleted layer is now at `bottom`; the root alone is never active.
        canvas.activeLayer =
            canvas.layersOrder.size() == 1 ? kLayerInvalid : canvas.layersOrder[bottom > 0 ? bottom - 1 : 0];
    }
    canvas.selectedLayers.assign(1, canvas.activeLayer);
    canvas.modified = true;
    HistoryPushLayers(canvasId, orderBefore, changed);
    return {};
}

Result<void> LayerMove(Canvas canvasId, Layer layer, Layer height) noexcept {
    ZoneScopedN("LayerMove");
    CanvasState& canvas = gAppState.canvases[canvasId];
    if (LayerOpsBlocked(canvas)) {
        return std::unexpected(Error::CanvasBusy);
    }
    if (!LayerEditable(canvas, layer)) {
        return std::unexpected(Error::LayerOpInvalid);
    }
    const Layer top = canvas.layerHeights[layer];
    const auto count = static_cast<Layer>(canvas.layers[layer].descendants + 1);
    const auto bottom = static_cast<Layer>(top + 1 - count);
    if (height >= canvas.layersOrder.size() - count) {
        return std::unexpected(Error::LayerOpInvalid);
    }
    const Layer oldParent = canvas.layerParents[layer];
    const std::vector<Layer> orderBefore = canvas.layersOrder;
    std::vector<HistoryLayer> changed;
    LayerDescendantsAdd(canvas, changed, oldParent, -count);
    const std::vector<Layer> moved(canvas.layersOrder.begin() + bottom, canvas.layersOrder.begin() + top + 1);
    canvas.layersOrder.erase(canvas.layersOrder.begin() + bottom, canvas.layersOrder.begin() + top + 1);
    LayersIndex(canvas);
    const Layer newParent = LayerParentAt(canvas, height);
    LayerDescendantsAdd(canvas, changed, newParent, count);
    canvas.layersOrder.insert(canvas.layersOrder.begin() + height, moved.begin(), moved.end());
    LayersIndex(canvas);
    // Back where it was: the same folders hold it, with the same counts.
    if (canvas.layersOrder == orderBefore) {
        return {};
    }
    LayerCompositesDirty(canvas, oldParent);
    LayerCompositesDirty(canvas, newParent);
    canvas.modified = true;
    HistoryPushLayers(canvasId, orderBefore, changed);
    return {};
}

Result<void> LayerMerge(Canvas canvasId, Layer layer) noexcept {
    ZoneScopedN("LayerMerge");
    CanvasState& canvas = gAppState.canvases[canvasId];
    if (LayerOpsBlocked(canvas)) {
        return std::unexpected(Error::CanvasBusy);
    }
    if (!LayerEditable(canvas, layer) || HasAnyFlag(canvas.layers[layer].flags, LayerFlags::Hidden)) {
        return std::unexpected(Error::LayerOpInvalid);
    }
    const Layer top = canvas.layerHeights[layer];
    const auto bottom = static_cast<Layer>(top - canvas.layers[layer].descendants);
    Layer lower = kLayerInvalid;
    if (!LayerIsFolder(canvas, layer)) {
        lower = bottom > 0 ? canvas.layersOrder[bottom - 1] : kLayerInvalid;
        if (lower == kLayerInvalid || canvas.layerParents[lower] != canvas.layerParents[layer]
            || LayerIsFolder(canvas, lower) || HasAnyFlag(canvas.layers[lower].flags, LayerFlags::Hidden)) {
            return std::unexpected(Error::LayerOpInvalid);
        }
    }
    const Layer result = LayerIdTake(canvas, kLayerInvalid);
    if (result == kLayerInvalid) {
        MIDORI_LOG_WARN("LayerMerge: all {} layer ids are in use", kLayerMax);
        return std::unexpected(Error::LayerOpInvalid);
    }
    // Out of the order until done, so nothing composites it.
    const Layer source = lower == kLayerInvalid ? layer : lower;
    canvas.layers[result] = LayerState{
        .opacity = canvas.layers[source].opacity,
        .blendMode = canvas.layers[source].blendMode,
        .flags = canvas.layers[source].flags & (LayerFlags::Locked | LayerFlags::Hidden),
    };
    canvas.layerNames[result] = canvas.layerNames[source];

    // Wherever the lower layer or anything in the upper one has a tile.
    std::array<bool, kLayerMax> merged{};
    for (Layer h = bottom; h <= top; ++h) {
        merged[canvas.layersOrder[h]] = true;
    }
    if (lower != kLayerInvalid) {
        merged[lower] = true;
    }
    LayerMergeState& merge = canvas.merge;
    merge = LayerMergeState{.result = result, .upper = layer, .lower = lower};
    for (const TileState& tile : canvas.tiles) {
        if (TileKeyMip(tile.key) == 0 && !HasAnyFlag(tile.flags, TileFlags::Composite)
            && merged[TileKeyLayer(tile.key)]) {
            merge.remaining.push_back(TileGridPosKey(tile.key));
        }
    }
    std::ranges::sort(merge.remaining);
    const auto [first, last] = std::ranges::unique(merge.remaining);
    merge.remaining.erase(first, last);
    canvas.modified = true;
    return {};
}

void LayerMergeFinish(CanvasState& canvas) noexcept {
    ZoneScopedN("LayerMergeFinish");
    const LayerMergeState merge = std::exchange(canvas.merge, LayerMergeState{});
    const Layer top = canvas.layerHeights[merge.upper];
    const Layer bottom = merge.lower == kLayerInvalid ? static_cast<Layer>(top - canvas.layers[merge.upper].descendants)
                                                      : canvas.layerHeights[merge.lower];
    const Layer parent = canvas.layerParents[merge.upper];
    const std::vector<Layer> orderBefore = canvas.layersOrder;
    std::vector<HistoryLayer> changed;
    // The result was out of the order: undoing deletes it again.
    const LayerState result = canvas.layers[merge.result];
    canvas.layers[merge.result].flags |= LayerFlags::Deleted;
    LayerRemember(canvas, changed, merge.result);
    LayerDescendantsAdd(canvas, changed, parent, -(top - bottom));
    for (Layer h = bottom; h <= top; ++h) {
        const Layer id = canvas.layersOrder[h];
        LayerRemember(canvas, changed, id);
        canvas.layers[id].flags |= LayerFlags::Deleted | LayerFlags::Dirty;
    }
    std::vector<TileKey> composites;
    for (const TileState& tile : canvas.tiles) {
        if (HasAnyFlag(tile.flags, TileFlags::Composite)
            && HasAnyFlag(canvas.layers[TileKeyLayer(tile.key)].flags, LayerFlags::Deleted)) {
            composites.push_back(tile.key);
        }
    }
    for (const TileKey key : composites) {
        const Tile tile{
            .canvas = canvas.id, .mip = TileKeyMip(key), .layer = TileKeyLayer(key), .gridPos = TileKeyGridPos(key)};
        if (!TileDelete(tile)) {
            MIDORI_LOG_WARN("LayerMergeFinish: composite tile {:#018x} not deleted", key);
        }
    }
    canvas.layersOrder.erase(canvas.layersOrder.begin() + bottom, canvas.layersOrder.begin() + top + 1);
    canvas.layersOrder.insert(canvas.layersOrder.begin() + bottom, merge.result);
    canvas.layers[merge.result] = result;
    canvas.layers[merge.result].flags |= LayerFlags::Dirty;
    LayersIndex(canvas);
    LayerCompositesDirty(canvas, parent);
    canvas.activeLayer = merge.result;
    canvas.selectedLayers.assign(1, merge.result);
    canvas.modified = true;
    HistoryPushLayers(canvas.id, orderBefore, changed);
}

LayerProperties LayerPropertiesGet(const CanvasState& canvas, Layer layer) noexcept {
    const LayerState& state = canvas.layers[layer];
    return LayerProperties{
        .opacity = state.opacity,
        .name = canvas.layerNames[layer],
        .blendMode = state.blendMode,
        .hidden = HasAnyFlag(state.flags, LayerFlags::Hidden),
        .locked = HasAnyFlag(state.flags, LayerFlags::Locked),
    };
}

Result<void> LayerPropertiesSet(Canvas canvasId, Layer layer, const LayerProperties& properties) noexcept {
    ZoneScopedN("LayerPropertiesSet");
    CanvasState& canvas = gAppState.canvases[canvasId];
    if (canvas.merge.result != kLayerInvalid) {
        return std::unexpected(Error::CanvasBusy);
    }
    if (!LayerEditable(canvas, layer)) {
        return std::unexpected(Error::LayerOpInvalid);
    }
    LayerPropertiesApply(canvas, layer, properties);
    return {};
}

void LayerPropertiesCommit(Canvas canvasId, std::span<const Layer> layers,
                           std::span<const LayerProperties> before) noexcept {
    ZoneScopedN("LayerPropertiesCommit");
    MIDORI_ASSERT(layers.size() == before.size(), "{} layers, {} properties", layers.size(), before.size());
    const CanvasState& canvas = gAppState.canvases[canvasId];
    std::vector<HistoryLayer> changed;
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (before[i] == LayerPropertiesGet(canvas, layers[i])) {
            continue;
        }
        HistoryLayer& entry = changed.emplace_back(HistoryLayer{
            .before = canvas.layers[layers[i]],
            .after = {},
            .beforeName = before[i].name,
            .afterName = {},
            .id = layers[i],
        });
        entry.before.opacity = before[i].opacity;
        entry.before.blendMode = before[i].blendMode;
        entry.before.flags =
            before[i].hidden ? entry.before.flags | LayerFlags::Hidden : entry.before.flags & ~LayerFlags::Hidden;
        entry.before.flags =
            before[i].locked ? entry.before.flags | LayerFlags::Locked : entry.before.flags & ~LayerFlags::Locked;
    }
    if (!changed.empty()) {
        HistoryPushLayers(canvasId, {}, changed);
    }
}

} // namespace Midori
