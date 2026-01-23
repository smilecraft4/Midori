#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "app.h"
#include "command.h"
#include "tiles.h"
#include "types.h"

namespace Midori {
// class Tiles {};
/*
enum class BlendMode : std::uint8_t {
    Alpha,
    Add,
    Multiply,
    Screen,
    Overlay,
};

class Layers {
   public:
    enum class Result {
        None,
        Unknown,
    };

    using ID = std::uint16_t;
    using Depth = std::uint16_t;
    using Opacity = std::uint8_t;

    enum class Type : std::uint8_t {
        RasterLayer,
        RasterMask,
        Folder,
    };

    struct CreateInfo {
        Depth depth;
        std::string_view name;
        Type type;
        Opacity opacity;
        BlendMode blendMode;
        bool hidden;
        bool locked;
        bool internal;
    };

    std::expected<ID, Result> New(CreateInfo& createInfo);
    std::expected<ID, Result> Duplicate(ID srcLayer);
    std::expected<ID, Result> Merge(ID over, ID under);
    std::expected<ID, Result> Delete(ID layer);

    std::expected<ID, Result> GetLayer(const std::string& name) const;
    std::expected<ID, Result> GetLayer(Depth depth) const;

    Result SetDepth(ID layer, Depth depth);
    Result SetOpacity(ID layer, Opacity opacity);
    Result SetName(ID layer, const std::string& name);
    Result SetHidden(ID layer, bool hidden);
    Result SetLocked(ID layer, bool locked);

   private:
    ID lastID = 0;
    std::vector<ID> freeIDs;

    std::vector<bool> saved;
    std::vector<bool> modified;
    std::vector<bool> hidden;
    std::vector<bool> locked;
    std::vector<bool> internal;
    std::vector<Type> types;
    std::vector<Depth> dephts;
    std::vector<Opacity> opacities;
    std::vector<std::string> names;
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
    uint8_t previousDepth_;
    uint8_t newDepth_;

   public:
    explicit LayerDepthCommand(App &app, Layer layer, uint8_t previousDepth, uint8_t newDepth);

    virtual std::string name() const { return "Layer Depth"; }
    virtual void execute();
    virtual void revert();
};
}  // namespace Midori