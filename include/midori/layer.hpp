#pragma once

#include "midori/base.hpp"
#include "midori/canvas.hpp"
#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace Midori {

// What a layer shows as, apart from its content and place.
struct LayerProperties {
    float opacity;
    std::array<char, kLayerNameLenMax> name;
    BlendMode blendMode;
    bool hidden;

    [[nodiscard]] bool operator==(const LayerProperties&) const = default;
};

// Truncated to fit, always zero terminated.
[[nodiscard]] std::array<char, kLayerNameLenMax> LayerName(std::string_view text) noexcept;
// Rebuilds layerHeights and layerParents from layersOrder.
void LayersIndex(CanvasState& canvas) noexcept;
// A new canvas's layers: the root folder holding one layer, made active. No history.
void LayersInit(CanvasState& canvas) noexcept;
// `folder` and every folder around it are composited again at every grid position; every folder with kLayerInvalid.
void LayerCompositesDirty(CanvasState& canvas, Layer folder) noexcept;
// The folders around the layer of the tile `key` are composited again at its grid position.
void LayerCompositesTouch(CanvasState& canvas, TileKey key) noexcept;
// The layers ops act on together: the selected ones still in the order, top first, else the active one, else none.
[[nodiscard]] std::vector<Layer> LayersSelected(const CanvasState& canvas) noexcept;
// Layer ops wait for the canvas's merge, its stroke and its pending undo or redo steps.
[[nodiscard]] bool LayerOpsBlocked(const CanvasState& canvas) noexcept;
// In the order, not a folder, neither hidden nor locked.
[[nodiscard]] bool LayerPaintable(const CanvasState& canvas, Layer layer) noexcept;

// Each pushes one history record. Error::CanvasBusy while LayerOpsBlocked, Error::LayerOpInvalid for the root, a
// layer out of the order or a height out of range.
[[nodiscard]] Result<Layer> LayerNew(Canvas canvas, const LayerCreateInfo& createInfo) noexcept;
// A folder goes with everything inside it. They are Deleted: out of the order, kept while history can bring them back.
// The active layer moves to the one below the lowest deleted, else above, and is the only one selected.
[[nodiscard]] Result<void> LayerDelete(Canvas canvas, std::span<const Layer> layers) noexcept;
// A folder goes with everything inside it, to `height` of the order without them, as LayerCreateInfo places it.
[[nodiscard]] Result<void> LayerMove(Canvas canvas, Layer layer, Layer height) noexcept;
// Starts merging a folder into one layer that shows what it shows, or a layer down into the one right below it in the
// same folder, which must be a shown layer, not a folder. The result is a new layer, made tile by tile as the sources
// stream in, with the properties of the folder or of the lower layer; the canvas takes no edit until it is done. Then
// LayerMergeFinish puts it in place of the sources, as one history record.
[[nodiscard]] Result<void> LayerMerge(Canvas canvas, Layer layer) noexcept;
// Once every result tile of the canvas's merge is composited.
void LayerMergeFinish(CanvasState& canvas) noexcept;

[[nodiscard]] LayerProperties LayerPropertiesGet(const CanvasState& canvas, Layer layer) noexcept;
// Live and without history, as while a slider is dragged; LayerPropertiesCommit then records the change. Not while the
// canvas merges.
[[nodiscard]] Result<void> LayerPropertiesSet(Canvas canvas, Layer layer, const LayerProperties& properties) noexcept;
// One history record from each layer's `before` to what it has now, none when nothing changed.
void LayerPropertiesCommit(Canvas canvas, std::span<const Layer> layers,
                           std::span<const LayerProperties> before) noexcept;

} // namespace Midori
