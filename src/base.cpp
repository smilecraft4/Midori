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

thread_pool::thread_pool(size_t size, const std::string& name) {
    threads_.reserve(size);
    for (size_t i = 0; i < size; i++) {
        threads_.emplace_back([this, name, i] {
            const auto n = std::format("{} {}", name, i);
            tracy::SetThreadName(n.c_str());

            while (true) {
                std::function<void()> task;
                {
                    std::unique_lock lock(mtx_);
                    cv_.wait(lock, [this] { return !tasks_.empty() || wait_ || stop_; });
                    if (stop_ || (wait_ && tasks_.empty())) {
                        return;
                    }

                    task = std::move(tasks_.front());
                    tasks_.pop();
                }
                task();
            }
        });
    }
}

thread_pool::~thread_pool() {
    stop();
}

void thread_pool::wait() {
    if (wait_) {
        return;
    }

    {
        std::unique_lock lock(mtx_);
        wait_ = true;
    }
    cv_.notify_all();
    for (auto& thread : threads_) {
        thread.join();
    }
}

void thread_pool::stop() {
    if (stop_) {
        return;
    }

    {
        std::unique_lock lock(mtx_);
        stop_ = true;
    }
    cv_.notify_all();
    for (auto& thread : threads_) {
        thread.join();
    }
}

void thread_pool::emplace(std::function<void()> task) {
    {
        std::unique_lock lock(mtx_);
        tasks_.emplace(std::move(task));
    }
    cv_.notify_one();
}
