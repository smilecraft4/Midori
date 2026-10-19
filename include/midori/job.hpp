#pragma once

#include "midori/base.hpp"
#include "midori/tile.hpp"
#include <cstdint>
#include <limits>

namespace Midori {

using Worker = std::uint8_t;
constexpr Worker kWorkerInvalid = std::numeric_limits<Worker>::max();

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

using JobExecuteFn = Result<void>(Worker, Job);

struct JobsInitInfo {
    Worker workerCount{kWorkerInvalid}; // by default uses `std::ceil(2, std::thread::hardware_concurrency() - 2)`
    JobExecuteFn executeFn;
};

Result<void> JobsInit(JobsInitInfo initInfo) noexcept;
Result<void> JobsUpdate() noexcept;
void JobsTerminate() noexcept;
void JobsWait() noexcept;
bool JobsBusy() noexcept;

Result<void> JobSubmit(Job job) noexcept;

} // namespace Midori
