#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>

#include "app.h"
#include "command.h"
#include "tiles.h"
#include "types.h"

namespace Midori {
// class Tiles {};
/*
class Layers {
   public:
    enum class Error {
        Unknown,
    };

    template <typename T>
    using Result = std::expected<T, Error>;

    using ID = std::uint32_t;
    using Depth = std::uint16_t;
    using Opacity = std::uint8_t;

    struct CreateInfo {
        Depth depth;
        Opacity opacity;
        std::string name;
        bool internal;  // true if the layer is created by the app not the user
        bool hidden;
        bool locked;
    };

    std::expected<ID, Error> New(CreateInfo& createInfo);
    std::expected<ID, Error> Duplicate(ID srcLayer);
    std::expected<ID, Error> Merge(ID overLayer, ID underLayer);

    std::expected<ID, Error> Delete(ID layer);

    std::expected<ID, Error> GetLayer(const std::string& name) const;
    std::expected<ID, Error> GetLayer(Depth depth) const;

    std::optional<Error> SetDepth(ID layer, Depth depth);
    std::optional<Error> SetOpacity(ID layer, Opacity opacity);
    std::optional<Error> SetName(ID layer, const std::string& name);
    std::optional<Error> SetHidden(ID layer, bool hidden);
    std::optional<Error> SetLocked(ID layer, bool locked);

   private:
};
*/

class LayerCreateCommand : public Command {
   private:
    App &app_;
    LayerInfo info_;

   public:
    explicit LayerCreateCommand(App &app, LayerInfo info);

    virtual std::string name() const { return "Layer Create"; }
    virtual void execute();
    virtual void revert();
};

class LayerDeleteCommand : public Command {
   private:
    App &app_;
    Layer layer_;

   public:
    explicit LayerDeleteCommand(App &app, Layer layer);

    virtual std::string name() const { return "Layer Delete"; }
    virtual void execute();
    virtual void revert();
};

class LayerDepthCommand : public Command {
   private:
    App &app_;
    Layer layer_;
    int previousDepth_;
    int newDepth_;

   public:
    explicit LayerDepthCommand(App &app, Layer layer, int previousDepth, int newDepth);

    virtual std::string name() const { return "Layer Depth"; }
    virtual void execute();
    virtual void revert();
};
}  // namespace Midori