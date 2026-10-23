#pragma once

#include "midori/base.hpp"
#include "midori/canvas.hpp"
#include "midori/tile.hpp"
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace Midori {

enum struct JobType : std::uint8_t {
    TileRead,  // reader threads: `pixels` from `file`
    TileWrite, // the writer thread, the only one writing to the libmdbx envs from jobs: `pixels` into `file`
};

// Plain data handed to a thread and back. The canvas outlives its jobs: CanvasDestroy waits for them.
struct Job {
    CanvasFile file;
    TileKey key;
    std::byte* pixels; // kTileBytes of a transfer slot's mapped memory
    TileTransferSlot slot;
    Canvas canvas;
    JobType type;
    bool failed;
    bool empty; // TileWrite: the tile was transparent, so nothing was written
};

using JobExecuteFn = Result<void>(Job& job) noexcept;

struct JobQueue {
    std::mutex mutex;
    std::condition_variable_any ready;
    std::deque<Job> jobs;
};

// Queues and results are guarded by their mutex; `execute` and the threads are set by JobsInit and fixed until
// JobsTerminate, both on the main thread. `pending` counts the jobs submitted and not yet in `finished`.
struct JobsState {
    JobQueue reads;
    JobQueue writes;
    std::mutex finishedMutex;
    std::condition_variable idle;
    std::vector<Job> finished;
    std::uint32_t pending{};
    std::vector<std::jthread> threads;
    JobExecuteFn* execute{};
};

extern JobsState gJobsState;

struct JobsInitInfo {
    JobExecuteFn* execute;
    std::uint32_t readerCount; // 0: the hardware threads left after the main and writer threads, at least 1
};

void JobsInit(JobsInitInfo initInfo) noexcept;
// Drops the jobs not started and joins the threads.
void JobsTerminate() noexcept;
void JobSubmit(const Job& job) noexcept;
// Moves the finished jobs to the end of `jobs`.
void JobsTakeFinished(std::vector<Job>& jobs) noexcept;
void JobsWaitIdle() noexcept;

} // namespace Midori
