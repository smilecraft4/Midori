#include "layers.h"

namespace Midori {

LayerCreateCommand::LayerCreateCommand(App& app, LayerInfo info) : Command(Type::Unknown), app_(app), info_(info) {}

void LayerCreateCommand::execute() {
    const auto layer = app_.canvas.CreateLayer(info_.name, info_.depth);
    app_.canvas.layer_infos[layer] = info_;
}

void LayerCreateCommand::revert() {}

LayerDeleteCommand::LayerDeleteCommand(App& app, Layer layer) : Command(Type::Unknown), app_(app), layer_(layer) {}

void LayerDeleteCommand::execute() { app_.canvas.DeleteLayer(layer_); }

void LayerDeleteCommand::revert() {}

LayerDepthCommand::LayerDepthCommand(App& app, Layer layer, int previousDepth, int newDepth)
    : Command(Type::Unknown), app_(app), layer_(layer), previousDepth_(previousDepth), newDepth_(newDepth) {}

void LayerDepthCommand::execute() { app_.canvas.SetLayerDepth(layer_, newDepth_); }

void LayerDepthCommand::revert() { app_.canvas.SetLayerDepth(layer_, previousDepth_); }
}  // namespace Midori
