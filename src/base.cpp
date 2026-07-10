#include "base.hpp"
#include <format>
#include <tracy/Tracy.hpp>

// void ArenaAllocator::Reserve(size_t newCapacity, size_t alignment) {
//     Assert(head == data, "ArenaAllocator still in use");
//     if (head != data) {
//         throw std::bad_alloc();
//     }
//     if (newCapacity == capacity) {
//         return;
//     }
//     if (newCapacity == 0 && data != nullptr) {
//         Reset();
//     }
//
//     data = static_cast<std::byte*>(Alloc(newCapacity, alignment));
//     head = data;
//     capacity = newCapacity;
// }

// void ArenaAllocator::Clear() {
//     Free(data);
//     capacity = 0;
//     head = nullptr;
//     data = nullptr;
// }

void* Malloc(size_t size) {
    void* mem = std::malloc(size);
    TracyAlloc(mem, size);
    return mem;
}

void* Calloc(size_t count, size_t size) {
    void* ptr = std::calloc(count, size);
    TracyAlloc(ptr, size);
    return ptr;
}

void* Realloc(void* ptr, size_t size) {
    TracyFree(ptr);
    void* newMem = std::realloc(ptr, size);
    TracyAlloc(newMem, size);
    return newMem;
}

void Free(void* ptr) {
    TracyFree(ptr);
    std::free(ptr);
}

void* operator new(std::size_t count) {
    if (count == 0) {
        count = 1;
    }
    auto* ptr = Malloc(count);
    return ptr;
}

void operator delete(void* ptr) noexcept {
    Free(ptr);
}

ThreadPool::ThreadPool(size_t size, const std::string& name) {
    threads.reserve(size);
    for (size_t i = 0; i < size; i++) {
        threads.emplace_back([this, name, i] {
            const auto n = std::format("{} {}", name, i);
            tracy::SetThreadName(n.c_str());

            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock lock(tasksMtx);
                    cv.wait(lock, [this] { return !tasks.empty() || wait || stop; });
                    if (stop || (wait && tasks.empty())) {
                        return;
                    }

                    task = std::move(tasks.front());
                    tasks.pop();
                }
                task();
            }
        });
    }
}

ThreadPool::~ThreadPool() {
    Stop();
}

void ThreadPool::Wait() {
    if (wait) {
        return;
    }

    {
        std::unique_lock lock(tasksMtx);
        wait = true;
    }
    cv.notify_all();
    for (auto& thread : threads) {
        thread.join();
    }
}

void ThreadPool::Stop() {
    if (stop) {
        return;
    }

    {
        std::unique_lock lock(tasksMtx);
        stop = true;
    }
    cv.notify_all();
    for (auto& thread : threads) {
        thread.join();
    }
}

void ThreadPool::Emplace(std::function<void()> task) {
    {
        std::unique_lock lock(tasksMtx);
        tasks.emplace(std::move(task));
    }
    cv.notify_one();
}
