#include "midori/job.hpp"

#include "midori/log.hpp"

#include <algorithm>
#include <stop_token>

#include <tracy/Tracy.hpp>

namespace Midori {

JobsState gJobsState{};

namespace {

constexpr std::uint32_t kReaderCountMax = 8;

void JobsRun(const std::stop_token& stop, JobQueue& queue) noexcept {
    JobsState& state = gJobsState;
    for (;;) {
        Job job{};
        {
            std::unique_lock lock(queue.mutex);
            if (!queue.ready.wait(lock, stop, [&queue] { return !queue.jobs.empty(); })) {
                return;
            }
            job = queue.jobs.front();
            queue.jobs.pop_front();
        }
        {
            ZoneScopedN("Job");
            job.failed = !state.execute(job);
        }
        {
            const std::scoped_lock lock(state.finishedMutex);
            state.finished.push_back(job);
            --state.pending;
        }
        state.idle.notify_all();
    }
}

} // namespace

void JobsInit(JobsInitInfo initInfo) noexcept {
    JobsState& state = gJobsState;
    MIDORI_ASSERT(state.threads.empty(), "JobsInit called twice");
    state.execute = initInfo.execute;
    std::uint32_t readers = initInfo.readerCount;
    if (readers == 0) {
        const std::uint32_t hardware = std::thread::hardware_concurrency();
        readers = std::clamp<std::uint32_t>(hardware > 2 ? hardware - 2 : 1, 1, kReaderCountMax);
    }
    state.threads.reserve(readers + 1);
    for (std::uint32_t i = 0; i < readers; ++i) {
        state.threads.emplace_back([](const std::stop_token& stop) { JobsRun(stop, gJobsState.reads); });
    }
    state.threads.emplace_back([](const std::stop_token& stop) { JobsRun(stop, gJobsState.writes); });
    MIDORI_LOG_INFO("jobs: {} reader threads and 1 writer thread", readers);
}

void JobsTerminate() noexcept {
    JobsState& state = gJobsState;
    for (std::jthread& thread : state.threads) {
        thread.request_stop();
    }
    state.threads.clear();
    state.reads.jobs.clear();
    state.writes.jobs.clear();
    state.finished.clear();
    state.pending = 0;
}

void JobSubmit(const Job& job) noexcept {
    JobsState& state = gJobsState;
    {
        const std::scoped_lock lock(state.finishedMutex);
        ++state.pending;
    }
    JobQueue& queue = job.type == JobType::TileWrite ? state.writes : state.reads;
    {
        const std::scoped_lock lock(queue.mutex);
        queue.jobs.push_back(job);
    }
    queue.ready.notify_one();
}

void JobsTakeFinished(std::vector<Job>& jobs) noexcept {
    JobsState& state = gJobsState;
    const std::scoped_lock lock(state.finishedMutex);
    jobs.insert(jobs.end(), state.finished.begin(), state.finished.end());
    state.finished.clear();
}

void JobsWaitIdle() noexcept {
    ZoneScopedN("JobsWaitIdle");
    JobsState& state = gJobsState;
    std::unique_lock lock(state.finishedMutex);
    state.idle.wait(lock, [] { return gJobsState.pending == 0; });
}

} // namespace Midori
