#pragma once

#include "midori/base.hpp"
#include "midori/tile.hpp"
#include <cstdint>

namespace Midori {

enum struct JobType : std::uint8_t {
    TileLoad,
    TileCache,
    TileCommit,
    TileSave,
    LayerSave,
    MetadataSave,
};

enum struct JobStatus : std::uint8_t {
    Pending,
    Busy,
    Finished,
    Error,
};

struct Job {
    Tile tile;
    JobType type;
    JobStatus status;
    TileTextureTransferSlot slot;
    Error error;
};

[[nodiscard]] Result<void> JobSubmit(Job job) noexcept;

} // namespace Midori
