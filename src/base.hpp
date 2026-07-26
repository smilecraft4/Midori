#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <queue>
#include <ranges>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <limits>
#include <numbers>
#include <utility>

namespace Midori {

constexpr size_t operator""_KiB(size_t b) {
    return b * 1024llu;
}
constexpr size_t operator""_KB(size_t b) {
    return b * 1000llu;
}
constexpr size_t operator""_MiB(size_t b) {
    return b * 1024llu * 1024llu;
}
constexpr size_t operator""_MB(size_t b) {
    return b * 1000llu * 1000llu;
}
constexpr size_t operator""_GiB(size_t b) {
    return b * 1024llu * 1024llu * 1024llu;
}
constexpr size_t operator""_GB(size_t b) {
    return b * 1000llu * 1000llu * 1000llu;
}

template<typename T>
concept Numeric = std::is_arithmetic_v<T>;

template<Numeric T>
constexpr T Abs(T value) {
    return value < T(0) ? -value : value;
}

template<Numeric T>
constexpr T Mod(T value, T step = T(1)) {
    const auto i = static_cast<long long>(value / step);
    return value - (static_cast<T>(i) * step);
}

template<Numeric T>
constexpr T Floor(T value, T step = T(1)) {
    if (step == T(0)) {
        return value;
    }

    if constexpr (std::is_integral_v<T>) {
        T q = value / step;
        T r = value % step;
        if (r != T(0) && ((r < T(0)) != (step < T(0)))) {
            q -= T(1);
        }
        return q * step;
    } else {
        T q = value / step;
        const auto i = static_cast<long long>(q);
        T t = static_cast<T>(i);
        if (t > q) {
            t -= T(1);
        }
        return t * step;
    }
}

template<Numeric T>
constexpr T Ceil(T value, T step = T(1)) {
    if (step == T(0)) {
        return value;
    }

    if constexpr (std::is_integral_v<T>) {
        T q = value / step;
        T r = value % step;
        if (r != T(0) && ((r < T(0)) == (step < T(0)))) {
            q += T(1);
        }
        return q * step;
    } else {
        T q = value / step;
        const auto i = static_cast<long long>(q);
        T t = static_cast<T>(i);
        if (t < q) {
            t += T(1);
        }
        return t * step;
    }
}

template<Numeric T>
constexpr T Round(T value, T step = T(1)) {
    if (step == T(0)) {
        return value;
    }

    if constexpr (std::is_integral_v<T>) {
        T absStep = Abs(step);
        T flo = Floor(value, step);
        T diff = value - flo;
        if (diff * T(2) >= absStep) {
            return flo + (step < T(0) ? -absStep : absStep);
        }
        return flo;
    } else {
        T q = value / step;
        T sign = q < T(0) ? T(-1) : T(1);
        T rounded = Floor(Abs(q) + T(0.5));
        return sign * rounded * step;
    }
}

template<Numeric T>
constexpr T Wrap(T value, T min, T max) {
    T range = max - min;
    if (range == T(0)) {
        return min;
    }

    if constexpr (std::is_integral_v<T>) {
        T result = (value - min) % range;
        if (result < T(0)) {
            result += Abs(range);
        }
        return result + min;
    } else {
        T step = Abs(range);
        T result = Mod(value - min, step);
        if (result < T(0)) {
            result += step;
        }
        return result + min;
    }
}

template<Numeric T>
constexpr T Ln(T x) {
    using F = std::conditional_t<std::is_floating_point_v<T>, T, double>;
    F fx = static_cast<F>(x);

    if (fx <= F(0)) {
        return static_cast<T>(-std::numeric_limits<F>::infinity());
    }

    int k = 0;
    while (fx >= F(2)) {
        fx /= F(2);
        ++k;
    }
    while (fx < F(1)) {
        fx *= F(2);
        --k;
    }

    F u = (fx - F(1)) / (fx + F(1));
    F u2 = u * u;
    F term = u;
    F sum = u;
    for (int n = 1; n < 20; ++n) {
        term *= u2;
        sum += term / F((2 * n) + 1);
    }
    return static_cast<T>((F(2) * sum) + (F(k) * std::numbers::ln2_v<long double>));
}

template<Numeric T>
constexpr T Exp(T x) {
    using F = std::conditional_t<std::is_floating_point_v<T>, T, double>;
    F fx = static_cast<F>(x);

    bool neg = fx < F(0);
    if (neg) {
        fx = -fx;
    }

    int k = 0;
    while (fx > F(0.5)) {
        fx /= F(2);
        ++k;
    }

    F term = F(1);
    F sum = F(1);
    for (int n = 1; n < 20; ++n) {
        term *= fx / F(n);
        sum += term;
    }
    for (int i = 0; i < k; ++i) {
        sum *= sum;
    }

    return static_cast<T>(neg ? F(1) / sum : sum);
}

template<Numeric T>
constexpr T Pow(T power, T exponent) {
    if constexpr (std::is_floating_point_v<T>) {
        if (exponent == static_cast<T>(static_cast<long long>(exponent))) {
            auto e = static_cast<long long>(exponent);
            bool neg = e < 0;
            if (neg) {
                e = -e;
            }
            T r(1);
            for (long long i = 0; i < e; ++i) {
                r *= power;
            }
            return neg ? T(1) / r : r;
        }
        return Exp(exponent * Ln(power));
    } else {
        T r(1);
        T e = Abs(exponent);
        for (T i = 0; i < e; ++i) {
            r *= power;
        }
        return exponent < T(0) ? T(1) / r : r;
    }
}

template<Numeric T>
constexpr T Remap(T value, T low, T high, T dstLow, T dstHigh) {
    // TODO

    return value;
}

void Assert(bool cond, const char* msg, const std::source_location& loc = std::source_location::current());

void* Malloc(size_t size);
void* Calloc(size_t value, size_t size);
void* Realloc(void* ptr, size_t size);
void Free(void* ptr);

// NOLINTBEGIN(readability-identifier-naming)

template<class Alloc = std::allocator<std::byte>>
struct arena {
    Alloc alloc_;
    std::byte* data_{};
    std::byte* head_{};
    size_t capacity_{};

    // TODO Add RAII stuff

    explicit arena(size_t capacity) : capacity_(capacity), alloc_(std::allocator<std::byte>()) {
        data_ = alloc_.allocate(capacity);
        head_ = data_;
    }

    explicit arena(size_t capacity, Alloc allocator) : capacity_(capacity), alloc_(allocator) {
        data_ = alloc_.allocate(capacity);
        head_ = data_;
    }

    ~arena() {
        alloc_.deallocate(data_, capacity_);
        capacity_ = 0;
        head_ = nullptr;
        data_ = nullptr;
    }

    arena(const arena& other)
        : alloc_(std::allocator_traits<Alloc>::select_on_container_copy_construction(other.alloc_)),
          capacity_(other.capacity_) {
        if (capacity_) {
            const size_t used = other.size();
            data_ = alloc_.allocate(capacity_);
            std::memcpy(data_, other.data_, used);
            head_ = data_ + used;
        }
    }

    arena(arena&& other) noexcept
        : alloc_(std::move(other.alloc_)), data_(other.data_), head_(other.head_), capacity_(other.capacity_) {
        other.data_ = nullptr;
        other.head_ = nullptr;
        other.capacity_ = 0;
    }

    arena& operator=(const arena& other) {
        if (this == &other) {
            return *this;
        }

        // Release the current buffer with the current allocator first.
        if (capacity_) {
            alloc_.deallocate(data_, capacity_);
        }
        data_ = nullptr;
        head_ = nullptr;
        capacity_ = 0;

        if constexpr (std::allocator_traits<Alloc>::propagate_on_container_copy_assignment::value
                      && std::is_copy_assignable_v<Alloc>) {
            alloc_ = other.alloc_;
        }

        if (other.capacity_) {
            const size_t used = other.size();
            data_ = alloc_.allocate(other.capacity_);
            std::memcpy(data_, other.data_, used);
            head_ = data_ + used;
            capacity_ = other.capacity_;
        }

        return *this;
    }

    arena& operator=(arena&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        if (capacity_) {
            alloc_.deallocate(data_, capacity_);
        }

        if constexpr (std::allocator_traits<Alloc>::propagate_on_container_move_assignment::value
                      && std::is_move_assignable_v<Alloc>) {
            alloc_ = std::move(other.alloc_);
        }

        data_ = other.data_;
        head_ = other.head_;
        capacity_ = other.capacity_;

        other.data_ = nullptr;
        other.head_ = nullptr;
        other.capacity_ = 0;

        return *this;
    }

    [[nodiscard]] size_t size() const { return head_ - data_; }

    [[nodiscard]] size_t capacity() const { return capacity_; }

    void* allocate(size_t bytes, size_t alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
        void* ptr = head_;
        // std::align takes `space` by reference and decrements it by the alignment
        // padding, so it must be a local holding the *remaining* space, not the
        // member `capacity` (passing the member would permanently shrink it).
        size_t space = capacity_ - size();
        if (std::align(alignment, bytes, ptr, space) == nullptr) {
            return nullptr;
        }
        head_ = static_cast<std::byte*>(ptr) + bytes;
        return ptr;
    }

    void deallocate(void* ptr, size_t size = 0) {}

    void reset() { head_ = data_; }
};

template<typename T, class Alloc = std::allocator<std::byte>>
struct arena_allocator {
    arena<Alloc>* arena_;

    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using reference = T&;
    using const_reference = const T&;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::true_type;

    arena_allocator(arena<Alloc>& arena) : arena_(&arena) {}

    arena_allocator(const arena_allocator& other) = default;

    template<typename U>
    arena_allocator(const arena_allocator<U>& other) : arena_(other.arena_) {}

    T* allocate(std::size_t n) {
        if (n == 0) {
            return nullptr;
        }

        void* ptr = arena_->allocate(sizeof(T) * n, alignof(T));
        if (ptr == nullptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(ptr);
    }

    void deallocate(T* ptr, std::size_t n) noexcept {
        (void)ptr;
        (void)n;
    }

    template<typename U, typename... Args>
    void construct(U* ptr, Args&&... args) {
        std::construct_at(ptr, std::forward<Args>(args)...);
    }

    template<typename U>
    void destroy(U* ptr) {
        ptr->~U();
    }

    bool operator==(const arena_allocator<T>& other) const noexcept { return arena_ == other.arena_; }

    bool operator!=(const arena_allocator<T>& other) const noexcept { return !(*this == other); }
};

template<typename T, class Alloc = std::allocator<T>>
struct pool {
    // TODO make multithreaded

    union Chunk {
        Chunk* next;
        T data;
    };

    using chunk_alloc = typename std::allocator_traits<Alloc>::template rebind_alloc<Chunk>;

    chunk_alloc alloc_;
    Chunk* free_{};
    std::mutex mtx_;
    std::atomic<size_t> count_;

    // TODO Add RAII stuff

    explicit pool(size_t count) : alloc_(std::allocator<Chunk>()), count_(count) {
        Chunk* prev{};
        for (size_t c = 0; c < count; c++) {
            free_ = alloc_.allocate(1);
            free_->next = prev;
            prev = free_;
        }
    }

    explicit pool(size_t count, Alloc allocator) : alloc_(chunk_alloc(allocator)), count_(count) {
        Chunk* prev{};
        for (size_t c = 0; c < count; c++) {
            free_ = alloc_.allocate(1);
            free_->next = prev;
            prev = free_;
        }
    }

    ~pool() { clear(); }

    // A pool only tracks its free list, not the chunks currently handed out, so a
    // meaningful deep copy is not possible; copying is disabled. Moving transfers
    // ownership of the free list (the mutex is not movable, so a fresh one is used).
    pool(const pool&) = delete;
    pool& operator=(const pool&) = delete;

    pool(pool&& other) noexcept : alloc_(std::move(other.alloc_)), free_(other.free_), count_(other.count_.load()) {
        other.free_ = nullptr;
        other.count_ = 0;
    }

    pool& operator=(pool&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        clear();

        if constexpr (std::allocator_traits<chunk_alloc>::propagate_on_container_move_assignment::value
                      && std::is_move_assignable_v<chunk_alloc>) {
            alloc_ = std::move(other.alloc_);
        }

        free_ = other.free_;
        count_ = other.count_.load();

        other.free_ = nullptr;
        other.count_ = 0;

        return *this;
    }

    [[nodiscard]] size_t count() const { return count_; }

    template<typename... Args>
    T* allocate(size_t n, Args&&... args) {
        if (!count_) {
            throw std::bad_alloc();
        }

        Chunk* chunk{};
        {
            std::unique_lock lock(mtx_);
            chunk = free_;
            free_ = free_->next;
        }
        count_--;
        return ::new (&chunk->data) T(std::forward<Args>(args)...);
    }

    void deallocate(T* obj, size_t n) {
        obj->~T();

        auto* chunk = reinterpret_cast<Chunk*>(obj);
        {
            std::unique_lock lock(mtx_);
            chunk->next = free_;
            free_ = chunk;
        }
        count_++;
    }

    void clear() {
        {
            std::unique_lock lock(mtx_);
            while (free_ != nullptr) {
                const auto prev = free_;
                free_ = free_->next;
                alloc_.deallocate(prev, 1);
                count_--;
            }
        }
        // Assert(count == 0, "Did not cleared all owned chunks");
    }
};

struct thread_pool {
    thread_pool(const thread_pool& pool) = delete;
    thread_pool(thread_pool&& pool) noexcept = delete;
    thread_pool& operator=(const thread_pool& pool) = delete;
    thread_pool& operator=(thread_pool&& pool) noexcept = delete;

    thread_pool(size_t size, const std::string& name = "Worker");
    ~thread_pool();

    void emplace(std::function<void()> task);

    void wait();
    void stop();

    std::condition_variable cv_{};
    std::queue<std::function<void()>> tasks_;
    std::vector<std::thread> threads_;
    std::mutex mtx_;
    bool wait_{};
    bool stop_{};
};

// template<typename Vec, typename T>
// bool PushUnique(Vec& vec, const T& value) {
//     for (const auto& elem : vec) {
//         if (elem == value) {
//             return false;
//         }
//     }
//     vec.push_back(value);
//     return true;
// }
//

template<typename T>
concept Hashable = requires(const T& a) {
    { std::hash<T>{}(a) } -> std::convertible_to<std::size_t>;
};

template<typename T>
concept Keyable = Hashable<T> && std::equality_comparable<T> && std::is_trivially_copyable_v<T>;

constexpr size_t Splitmix64(size_t hash) {
    hash ^= hash >> 30;
    hash *= 0xbf58476d1ce4e5b9ull;
    hash ^= hash >> 27;
    hash *= 0x94d049bb133111ebull;
    hash ^= hash >> 31;
    return hash;
}

template<Keyable Key, class Alloc = std::allocator<Key>>
struct hash_set {
    // static constexpr int8_t FULL = 0b01111111;     // low 7 bits used for hash
    static constexpr int8_t EMPTY = static_cast<int8_t>(0b10000000);   // free to use
    static constexpr int8_t DELETED = static_cast<int8_t>(0b11111110); // tombstone ??
    // static constexpr int8_t SENTINEL = static_cast<int8_t>(0b11111111); // end of table for iteration
    static constexpr size_t GROUP_WIDTH = 16;

    using slab_alloc = typename std::allocator_traits<Alloc>::template rebind_alloc<Key>;
    using slab_allocator_traits = std::allocator_traits<slab_alloc>;

    slab_alloc alloc_;
    int8_t* metas_{};
    Key* keys_{};
    size_t size_{};
    size_t capacity_{};

    hash_set(const hash_set& other)
        : alloc_(slab_allocator_traits::select_on_container_copy_construction(other.alloc_)),
          size_(other.size_),
          capacity_(other.capacity_) {
        if (capacity_) {
            const size_t metaCap = (((capacity_ * sizeof(int8_t)) + GROUP_WIDTH - 1) + sizeof(Key) - 1) / sizeof(Key);

            auto* ptr = slab_allocator_traits::allocate(alloc_, capacity_ + metaCap);
            std::memcpy(ptr, other.metas_, (capacity_ + metaCap) * sizeof(Key));

            metas_ = reinterpret_cast<int8_t*>(ptr);
            keys_ = ptr + metaCap;
        }
    }
    template<typename OtherAlloc>
    hash_set(const hash_set<Key, OtherAlloc>& other, Alloc alloc)
        : alloc_(alloc), size_(other.size_), capacity_(other.capacity_) {
        if (capacity_) {
            const size_t metaCap = (((capacity_ * sizeof(int8_t)) + GROUP_WIDTH - 1) + sizeof(Key) - 1) / sizeof(Key);

            auto* ptr = slab_allocator_traits::allocate(alloc_, capacity_ + metaCap);
            std::memcpy(ptr, other.metas_, (capacity_ + metaCap) * sizeof(Key));

            metas_ = reinterpret_cast<int8_t*>(ptr);
            keys_ = ptr + metaCap;
        }
    }

    hash_set(hash_set&& other) noexcept
        : alloc_(std::move(other.alloc_)),
          metas_(other.metas_),
          keys_(other.keys_),
          size_(other.size_),
          capacity_(other.capacity_) {
        other.reset();
    }

    hash_set& operator=(const hash_set& other) {
        if (this == &other) {
            return *this;
        }

        // Free existing storage with the current allocator before (optionally)
        // adopting the source allocator.
        release();

        if constexpr (slab_allocator_traits::propagate_on_container_copy_assignment::value
                      && std::is_copy_assignable_v<slab_alloc>) {
            alloc_ = other.alloc_;
        }

        size_ = other.size_;
        capacity_ = other.capacity_;

        if (capacity_) {
            const size_t metaCap = (((capacity_ * sizeof(int8_t)) + GROUP_WIDTH - 1) + sizeof(Key) - 1) / sizeof(Key);

            auto* ptr = slab_allocator_traits::allocate(alloc_, capacity_ + metaCap);
            std::memcpy(ptr, other.metas_, (capacity_ + metaCap) * sizeof(Key));

            metas_ = reinterpret_cast<int8_t*>(ptr);
            keys_ = ptr + metaCap;
        } else {
            metas_ = nullptr;
            keys_ = nullptr;
        }

        return *this;
    }

    hash_set& operator=(hash_set&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        release();

        if constexpr (slab_allocator_traits::propagate_on_container_move_assignment::value
                      && std::is_move_assignable_v<slab_alloc>) {
            alloc_ = std::move(other.alloc_);
        }
        metas_ = other.metas_;
        keys_ = other.keys_;
        size_ = other.size_;
        capacity_ = other.capacity_;

        other.reset();

        return *this;
    }

    hash_set(Alloc alloc) : alloc_(alloc) {}

    hash_set(size_t capacity = 0, Alloc alloc = std::allocator<Key>()) : capacity_(capacity), alloc_(alloc) {
        if (capacity) {
            const size_t metaCap = (((capacity * sizeof(int8_t)) + GROUP_WIDTH - 1) + sizeof(Key) - 1) / sizeof(Key);

            auto* ptr = slab_allocator_traits::allocate(alloc, capacity + metaCap);

            metas_ = reinterpret_cast<int8_t*>(ptr);
            keys_ = ptr + metaCap;

            std::memset(metas_, EMPTY, capacity);
            // metas[capacity] = SENTINEL;
        }
    }

    ~hash_set() { release(); }

    [[nodiscard]] size_t size() const { return size_; }
    [[nodiscard]] size_t capacity() const { return capacity_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] bool full() const { return size_ == capacity_; }

    void clear() {
        if (size_) {
            std::memset(metas_, EMPTY, capacity_);
            size_ = 0;
        }
    }

    void release() noexcept {
        if (capacity_) {
            const size_t metaCap = (((capacity_ * sizeof(int8_t)) + GROUP_WIDTH - 1) + sizeof(Key) - 1) / sizeof(Key);

            slab_allocator_traits::deallocate(alloc_, keys_ - metaCap, capacity_);

            keys_ = nullptr;
            metas_ = nullptr;
        }
    }

    void reset() noexcept {
        if (capacity_) {
            metas_ = nullptr;
            keys_ = nullptr;
            capacity_ = 0;
            size_ = 0;
        }
    }

    bool reserve(size_t newCapacity) {
        if (size_ > newCapacity) {
            return false;
        }

        const size_t metaCap = (((newCapacity * sizeof(int8_t)) + GROUP_WIDTH - 1) + sizeof(Key) - 1) / sizeof(Key);

        auto* ptr = slab_allocator_traits::allocate(alloc_, newCapacity + metaCap);
        auto* newMetas = reinterpret_cast<int8_t*>(ptr);
        auto* newKeys = ptr + metaCap;

        std::memset(newMetas, EMPTY, newCapacity);
        // metas[capacity] = SENTINEL;

        size_t newSize{};
        if (size_ > 0) {
            // TODO: use iterators instead
            for (size_t pos = 0; pos < capacity_; pos++) {
                if (metas_[pos] >= 0) {
                    const auto key = keys_[pos];
                    auto hash = std::hash<Key>{}(key);
                    const int8_t h2 = hash & 0x7F;
                    size_t newPos = (hash >> 7) % newCapacity;

                    bool inserted = false;
                    for (size_t probed = 0; probed < newCapacity; ++probed) {
                        const int8_t m = newMetas[newPos];
                        if (m == EMPTY || m == DELETED) {
                            inserted = true;
                            newMetas[newPos] = h2;
                            newKeys[newPos] = key;
                            newSize++;
                            break;
                        }
                        newPos = (newPos + 1) % newCapacity;
                    }

                    if (newSize == size_) {
                        break;
                    }
                }
            }
        }

        if (newSize < size_) {
            if (newCapacity) {
                slab_allocator_traits::deallocate(alloc_, ptr, metaCap + newCapacity);
            }
            return false;
        } else {
            release();

            keys_ = newKeys;
            metas_ = newMetas;
            size_ = newSize;
            capacity_ = newCapacity;

            return true;
        }
    }

    [[nodiscard]] bool contains(const Key& key) const {
        if (size_ == 0) {
            return false;
        }

        const size_t hash = std::hash<Key>{}(key);
        const int8_t h2 = hash & 0x7F;        // low 7 bits, high bit clear
        size_t pos = (hash >> 7) % capacity_; // H1: start slot

        for (size_t probed = 0; probed < capacity_; ++probed) {
            const int8_t m = metas_[pos];
            if (m == h2) { // full + H2 match -> candidate
                if (keys_[pos] == key) {
                    return true;
                }
            } else if (m == EMPTY) { // empty -> not present, stop
                return false;
            }
            // Deleted tombstone, or full-but-different-H2 -> keep probing
            pos = (pos + 1) % capacity_;
        }

        return false;
    }

    // std::set-style: returns true if a new element was inserted, false if the
    // key was already present. Capacity exhaustion is a resource fault, so it
    // throws std::bad_alloc (like a fixed-capacity container that cannot grow).
    bool insert(const Key& key) {
        if (capacity_ == 0) {
            throw std::bad_alloc();
        }

        const size_t hash = std::hash<Key>{}(key);
        const int8_t h2 = hash & 0x7F;        // low 7 bits, high bit clear
        size_t pos = (hash >> 7) % capacity_; // H1: start slot
        size_t firstDeleted = capacity_;      // sentinel: no tombstone seen yet

        for (size_t probed = 0; probed < capacity_; ++probed) {
            const int8_t m = metas_[pos];
            if (m == h2) {           // full + H2 match -> candidate
                if (keys_[pos] == key) {
                    return false;    // already present -> no insertion took place
                }
            } else if (m == EMPTY) { // key definitely absent -> insert here (or in a prior tombstone)
                const size_t target = firstDeleted != capacity_ ? firstDeleted : pos;
                metas_[target] = h2;
                keys_[target] = key;
                size_++;
                return true;
            } else if (m == DELETED && firstDeleted == capacity_) {
                firstDeleted = pos; // remember the first tombstone, but keep probing for the key
            }
            // Tombstone, or full-but-different-H2 -> keep probing
            pos = (pos + 1) % capacity_;
        }

        // Probed every slot without hitting EMPTY (saturated with tombstones): reuse one.
        if (firstDeleted != capacity_) {
            metas_[firstDeleted] = h2;
            keys_[firstDeleted] = key;
            size_++;
            return true;
        }

        throw std::bad_alloc(); // table full, key absent: capacity exhausted
    }

    bool erase(const Key& key) {
        if (size_ == 0) {
            return false;
        }

        const size_t hash = std::hash<Key>{}(key);
        const int8_t h2 = hash & 0x7F;        // low 7 bits, high bit clear
        size_t pos = (hash >> 7) % capacity_; // H1: start slot

        for (size_t probed = 0; probed < capacity_; ++probed) {
            const int8_t m = metas_[pos];
            if (m == h2) { // full + H2 match -> candidate
                if (keys_[pos] == key) {
                    metas_[pos] = DELETED;
                    size_--;
                    return true;
                }
            } else if (m == EMPTY) { // empty -> not present, stop
                return false;
            }
            // Deleted tombstone, or full H2 -> keep probing
            pos = (pos + 1) % capacity_;
        }
        return false;
    }

    struct Iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = Key;
        // using pointer = Key*;
        using const_pointer = Key const*;
        // using reference = Key&;
        using const_reference = Key const&;
        using difference_type = std::ptrdiff_t;

        Iterator() = default;

        Iterator(hash_set* set, bool end) : set(set) {
            if (!set->capacity_) {
                return;
            }
            if (end) {
                pos = set->capacity_;
                return;
            }
            while (pos < set->capacity_ && set->metas_[pos] < 0) {
                pos++;
            }
        }

        const_reference& operator*() const { return set->keys_[pos]; }

        const_pointer operator->() const { return set->keys_ + pos; }

        Iterator& operator++() {
            if (!set->capacity_) {
                return *this;
            }

            do {
                pos++;
            } while (pos < set->capacity_ && set->metas_[pos] < 0);

            return *this;
        }

        Iterator operator++(int) {
            auto tmp = *this;
            ++*this;
            return tmp;
        }

        Iterator& operator--() {
            if (!set->capacity_) {
                return *this;
            }

            pos--;
            while (pos > 0 && set->metas_[pos] < 0) {
                pos--;
            }
            return *this;
        }

        Iterator operator--(int) {
            auto tmp = *this;
            --*this;
            return tmp;
        }

        bool operator==(const Iterator& other) const { return set == other.set && pos == other.pos; }

        // bool operator!=(const Iterator& other) const { return !(*this == other); }

        hash_set* set{};
        size_t pos{};
    };

    using const_iterator = Iterator;
    using const_reverse_iterator = std::reverse_iterator<Iterator>;

    const_iterator begin() noexcept { return const_iterator(this, false); }
    const_iterator end() noexcept { return const_iterator(this, true); }
    const_reverse_iterator rbegin() noexcept { return const_reverse_iterator{end()}; }
    const_reverse_iterator rend() noexcept { return const_reverse_iterator{begin()}; }
};
static_assert(std::bidirectional_iterator<hash_set<int>::Iterator>);
static_assert(std::ranges::bidirectional_range<hash_set<int>>);

template<Keyable Key, typename Elem, class Alloc = std::allocator<Key>>
struct hash_map {
    static constexpr int8_t EMPTY = static_cast<int8_t>(0b10000000);   // free to use
    static constexpr int8_t DELETED = static_cast<int8_t>(0b11111110); // tombstone
    static constexpr size_t GROUP_WIDTH = 16;
    static constexpr size_t ALIGN = std::max({alignof(Key), alignof(Elem), alignof(std::max_align_t)});

    struct alignas(ALIGN) Slab {
        std::byte raw[ALIGN];
    };

    using slab_alloc = typename std::allocator_traits<Alloc>::template rebind_alloc<Slab>;
    using elem_alloc = typename std::allocator_traits<Alloc>::template rebind_alloc<Elem>;
    using key_alloc = typename std::allocator_traits<Alloc>::template rebind_alloc<Key>;
    using slab_atr = std::allocator_traits<slab_alloc>;
    using elem_atr = std::allocator_traits<elem_alloc>;
    using key_atr = std::allocator_traits<key_alloc>;

    slab_alloc sa_;
    elem_alloc ea_{sa_};
    Slab* ptr_{};
    int8_t* metas_{};
    Key* keys_{};
    Elem* elems_{};
    size_t size_{};
    size_t capacity_{};

    hash_map(const hash_map& other)
        : sa_(slab_atr::select_on_container_copy_construction(other.sa_)),
          ea_(sa_),
          size_(other.size_),
          capacity_(other.capacity_) {
        if (capacity_) {
            const size_t metaCap = (((capacity_ * sizeof(int8_t)) + GROUP_WIDTH - 1) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t keyCap = ((capacity_ * sizeof(Key)) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t elemCap = ((capacity_ * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);

            ptr_ = slab_atr::allocate(sa_, metaCap + keyCap + elemCap);
            metas_ = reinterpret_cast<int8_t*>(ptr_);
            keys_ = reinterpret_cast<Key*>(ptr_ + metaCap);
            elems_ = reinterpret_cast<Elem*>(ptr_ + metaCap + keyCap);

            // metas/keys are trivially copyable and can be bulk-copied; Elem may be
            // non-trivial, so copy-construct only the live slots.
            std::memcpy(metas_, other.metas_, capacity_ * sizeof(int8_t));
            std::memcpy(keys_, other.keys_, capacity_ * sizeof(Key));
            for (size_t pos = 0; pos < capacity_; ++pos) {
                if (other.metas_[pos] >= 0) {
                    elem_atr::construct(ea_, elems_ + pos, std::as_const(other.elems_[pos]));
                }
            }
        }
    }

    hash_map(hash_map&& other) noexcept
        : sa_(std::move(other.sa_)),
          ea_(std::move(other.ea_)),
          ptr_(other.ptr_),
          metas_(other.metas_),
          keys_(other.keys_),
          elems_(other.elems_),
          size_(other.size_),
          capacity_(other.capacity_) {
        other.reset();
    }

    hash_map& operator=(const hash_map& other) {
        if (this == &other) {
            return *this;
        }

        // Destroy live elements and free storage with the current allocator before
        // (optionally) adopting the source allocator.
        release();

        if constexpr (slab_atr::propagate_on_container_copy_assignment::value
                      && std::is_copy_assignable_v<slab_alloc>) {
            sa_ = other.sa_;
            ea_ = elem_alloc(sa_);
        }

        size_ = other.size_;
        capacity_ = other.capacity_;

        if (capacity_) {
            const size_t metaCap = (((capacity_ * sizeof(int8_t)) + GROUP_WIDTH - 1) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t keyCap = ((capacity_ * sizeof(Key)) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t elemCap = ((capacity_ * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);

            ptr_ = slab_atr::allocate(sa_, metaCap + keyCap + elemCap);
            metas_ = reinterpret_cast<int8_t*>(ptr_);
            keys_ = reinterpret_cast<Key*>(ptr_ + metaCap);
            elems_ = reinterpret_cast<Elem*>(ptr_ + metaCap + keyCap);

            std::memcpy(metas_, other.metas_, capacity_ * sizeof(int8_t));
            std::memcpy(keys_, other.keys_, capacity_ * sizeof(Key));
            for (size_t pos = 0; pos < capacity_; ++pos) {
                if (other.metas_[pos] >= 0) {
                    elem_atr::construct(ea_, elems_ + pos, std::as_const(other.elems_[pos]));
                }
            }
        } else {
            ptr_ = nullptr;
            metas_ = nullptr;
            keys_ = nullptr;
            elems_ = nullptr;
        }

        return *this;
    }

    hash_map& operator=(hash_map&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        release();

        if constexpr (slab_atr::propagate_on_container_move_assignment::value
                      && std::is_move_assignable_v<slab_alloc>) {
            sa_ = std::move(other.sa_);
            ea_ = std::move(other.ea_);
        }
        ptr_ = other.ptr_;
        metas_ = other.metas_;
        keys_ = other.keys_;
        elems_ = other.elems_;
        size_ = other.size_;
        capacity_ = other.capacity_;

        other.reset();

        return *this;
    }

    hash_map(size_t capacity = 0, Alloc alloc = std::allocator<Key>())
        : capacity_(capacity), sa_(slab_alloc(alloc)), ea_(sa_) {
        if (capacity) {
            const size_t metaCap = (((capacity * sizeof(int8_t)) + GROUP_WIDTH - 1) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t keyCap = ((capacity * sizeof(Key)) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t elemCap = ((capacity * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);

            ptr_ = slab_atr::allocate(sa_, (metaCap + keyCap + elemCap));
            metas_ = reinterpret_cast<int8_t*>(ptr_);
            keys_ = reinterpret_cast<Key*>(ptr_ + metaCap);
            elems_ = reinterpret_cast<Elem*>(ptr_ + metaCap + keyCap);

            std::memset(metas_, EMPTY, capacity * sizeof(int8_t));
            // metas[capacity] = SENTINEL;
        }
    }

    ~hash_map() { release(); }

    [[nodiscard]] size_t size() const { return size_; }
    [[nodiscard]] size_t capacity() const { return capacity_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] bool full() const { return size_ == capacity_; }

    void clear() {
        if (size_) {
            size_t pos = 0;
            while (pos < capacity_) {
                if (metas_[pos] >= 0) {
                    elems_[pos].~Elem();
                    size_--;
                    if (size_ == 0) {
                        break;
                    }
                }
                pos++;
            }
            assert(size_ == 0);
            std::memset(metas_, EMPTY, capacity_);
        }
    }

    void release() noexcept {
        if (capacity_) {
            clear();

            const size_t metaCap = (((capacity_ * sizeof(int8_t)) + GROUP_WIDTH - 1) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t keyCap = ((capacity_ * sizeof(Key)) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t elemCap = ((capacity_ * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);

            slab_atr::deallocate(sa_, ptr_, metaCap + keyCap + elemCap);

            reset();
        }
    }

    void reset() noexcept {
        if (capacity_) {
            ptr_ = nullptr;
            metas_ = nullptr;
            keys_ = nullptr;
            elems_ = nullptr;
            capacity_ = 0;
            size_ = 0;
        }
    }

    bool reserve(size_t newCapacity) {
        if (size_ > newCapacity) {
            return false;
        }

        const size_t metaCap = (((newCapacity * sizeof(int8_t)) + GROUP_WIDTH - 1) + sizeof(Slab) - 1) / sizeof(Slab);
        const size_t keyCap = ((newCapacity * sizeof(Key)) + sizeof(Slab) - 1) / sizeof(Slab);
        const size_t elemCap = ((newCapacity * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);

        auto* newPtr = slab_atr::allocate(sa_, metaCap + keyCap + elemCap);
        auto* newMetas = reinterpret_cast<int8_t*>(newPtr);
        auto* newKeys = reinterpret_cast<Key*>(newPtr + metaCap);
        auto* newElems = reinterpret_cast<Elem*>(newPtr + metaCap + keyCap);

        std::memset(newMetas, EMPTY, newCapacity);

        size_t newSize{};
        if (size_ > 0) {
            // TODO: use iterators instead
            for (size_t pos = 0; pos < capacity_; pos++) {
                if (metas_[pos] >= 0) {
                    const auto hash = std::hash<Key>{}(keys_[pos]);
                    const int8_t h2 = hash & 0x7f;
                    size_t newPos = (hash >> 7) % newCapacity;

                    for (size_t probed = 0; probed < newCapacity; ++probed) {
                        const int8_t m = newMetas[newPos];
                        if (m == EMPTY || m == DELETED) {
                            newMetas[newPos] = h2;
                            newKeys[newPos] = keys_[pos];
                            // Move-construct into the freshly allocated (uninitialized) slot.
                            elem_atr::construct(ea_, newElems + newPos, std::move(elems_[pos]));
                            newSize++;
                            break;
                        }
                        newPos = (newPos + 1) % newCapacity;
                    }

                    if (newSize == size_) {
                        break;
                    }
                }
            }
        }

        if (newSize < size_) {
            if (newCapacity) {
                slab_atr::deallocate(sa_, newPtr, metaCap + keyCap + elemCap);
            }
            return false;
        } else {
            release();

            ptr_ = newPtr;
            keys_ = newKeys;
            metas_ = newMetas;
            elems_ = newElems;
            size_ = newSize;
            capacity_ = newCapacity;

            return true;
        }
    }

    [[nodiscard]] bool contains(const Key& key) const {
        if (size_ == 0) {
            return false;
        }

        const size_t hash = std::hash<Key>{}(key);
        const int8_t h2 = hash & 0x7f;
        size_t pos = (hash >> 7) % capacity_;

        for (size_t probed = 0; probed < capacity_; ++probed) {
            const int8_t m = metas_[pos];
            if (m == h2) {
                if (keys_[pos] == key) {
                    return true;
                }
            } else if (m == EMPTY) {
                return false;
            }
            pos = (pos + 1) % capacity_;
        }

        return false;
    }

    // std::map-style: returns true if a new element was inserted, false if the
    // key was already present (the existing element is left untouched). Throws
    // std::bad_alloc when the table is full and the key is absent.
    bool insert(const Key& key, const Elem& elem) {
        if (capacity_ == 0) {
            throw std::bad_alloc();
        }

        const size_t hash = std::hash<Key>{}(key);
        const int8_t h2 = hash & 0x7f;
        size_t pos = (hash >> 7) % capacity_;
        size_t firstDeleted = capacity_;

        for (size_t probed = 0; probed < capacity_; ++probed) {
            const int8_t m = metas_[pos];
            if (m == h2) {
                if (keys_[pos] == key) {
                    return false; // already present
                }
            } else if (m == EMPTY) {
                const size_t target = firstDeleted != capacity_ ? firstDeleted : pos;
                metas_[target] = h2;
                keys_[target] = key;
                elem_atr::construct(ea_, elems_ + target, elem);
                size_++;
                return true;
            } else if (m == DELETED && firstDeleted == capacity_) {
                firstDeleted = pos;
            }
            pos = (pos + 1) % capacity_;
        }

        if (firstDeleted != capacity_) {
            metas_[firstDeleted] = h2;
            keys_[firstDeleted] = key;
            elem_atr::construct(ea_, elems_ + firstDeleted, elem);
            size_++;
            return true;
        }

        throw std::bad_alloc();
    }

    bool insert(const Key& key, Elem&& elem) {
        if (capacity_ == 0) {
            throw std::bad_alloc();
        }

        const size_t hash = std::hash<Key>{}(key);
        const int8_t h2 = hash & 0x7f;
        size_t pos = (hash >> 7) % capacity_;
        size_t firstDeleted = capacity_;

        for (size_t probed = 0; probed < capacity_; ++probed) {
            const int8_t m = metas_[pos];
            if (m == h2) {
                if (keys_[pos] == key) {
                    return false; // already present
                }
            } else if (m == EMPTY) {
                const size_t target = firstDeleted != capacity_ ? firstDeleted : pos;
                metas_[target] = h2;
                keys_[target] = key;
                elem_atr::construct(ea_, elems_ + target, std::move(elem));
                size_++;
                return true;
            } else if (m == DELETED && firstDeleted == capacity_) {
                firstDeleted = pos;
            }
            pos = (pos + 1) % capacity_;
        }

        if (firstDeleted != capacity_) {
            metas_[firstDeleted] = h2;
            keys_[firstDeleted] = key;
            elem_atr::construct(ea_, elems_ + firstDeleted, std::move(elem));
            size_++;
            return true;
        }

        throw std::bad_alloc();
    }

    // std::map-style: returns true if a new element was constructed in place,
    // false if the key was already present (no construction happens). Throws
    // std::bad_alloc when the table is full and the key is absent.
    template<typename... Args>
    bool emplace(const Key& key, Args&&... args) {
        if (capacity_ == 0) {
            throw std::bad_alloc();
        }

        const size_t hash = std::hash<Key>{}(key);
        const int8_t h2 = hash & 0x7f;
        size_t pos = (hash >> 7) % capacity_;
        size_t firstDeleted = capacity_;

        for (size_t probed = 0; probed < capacity_; ++probed) {
            const int8_t m = metas_[pos];
            if (m == h2) {
                if (keys_[pos] == key) {
                    return false; // already present
                }
            } else if (m == EMPTY) {
                const size_t target = firstDeleted != capacity_ ? firstDeleted : pos;
                metas_[target] = h2;
                keys_[target] = key;
                elem_atr::construct(ea_, elems_ + target, std::forward<Args>(args)...);
                size_++;
                return true;
            } else if (m == DELETED && firstDeleted == capacity_) {
                firstDeleted = pos;
            }
            pos = (pos + 1) % capacity_;
        }

        if (firstDeleted != capacity_) {
            metas_[firstDeleted] = h2;
            keys_[firstDeleted] = key;
            elem_atr::construct(ea_, elems_ + firstDeleted, std::forward<Args>(args)...);
            size_++;
            return true;
        }

        throw std::bad_alloc(); // table full, key absent: capacity exhausted
    }

    bool erase(const Key& key) {
        if (size_ == 0) {
            return false;
        }

        const size_t hash = std::hash<Key>{}(key);
        const int8_t h2 = hash & 0x7f;
        size_t pos = (hash >> 7) % capacity_;

        for (size_t probed = 0; probed < capacity_; ++probed) {
            const int8_t m = metas_[pos];
            if (m == h2) {
                if (keys_[pos] == key) {
                    metas_[pos] = DELETED;
                    elems_[pos].~Elem();
                    size_--;
                    return true;
                }
            } else if (m == EMPTY) {
                return false;
            }
            pos = (pos + 1) % capacity_;
        }
        return false;
    }

    // std::map-style checked access: throws std::out_of_range if the key is
    // absent. Use contains()/find-then-branch when a miss is an expected state.
    Elem& at(const Key& key) {
        if (size_ != 0) {
            const size_t hash = std::hash<Key>{}(key);
            const int8_t h2 = hash & 0x7f;
            size_t pos = (hash >> 7) % capacity_;

            for (size_t probed = 0; probed < capacity_; ++probed) {
                const int8_t m = metas_[pos];
                if (m == h2) {
                    if (keys_[pos] == key) {
                        return elems_[pos];
                    }
                } else if (m == EMPTY) {
                    break;
                }
                pos = (pos + 1) % capacity_;
            }
        }

        throw std::out_of_range("hash_map::at: key not found");
    }

    [[nodiscard]] const Elem& at(const Key& key) const {
        if (size_ != 0) {
            const size_t hash = std::hash<Key>{}(key);
            const int8_t h2 = hash & 0x7f;
            size_t pos = (hash >> 7) % capacity_;

            for (size_t probed = 0; probed < capacity_; ++probed) {
                const int8_t m = metas_[pos];
                if (m == h2) {
                    if (keys_[pos] == key) {
                        return elems_[pos];
                    }
                } else if (m == EMPTY) {
                    break;
                }
                pos = (pos + 1) % capacity_;
            }
        }

        throw std::out_of_range("hash_map::at: key not found");
    }

    struct Iterator {
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = std::pair<Key, Elem>;
        using reference = std::pair<const Key&, Elem&>;
        using difference_type = std::ptrdiff_t;

        hash_map* ptr{};
        size_t pos{};

        Iterator() = default;

        Iterator(hash_map* map, bool end) : ptr(map) {
            if (!map->capacity_) {
                return; // pos stays 0 for BOTH begin and end → equal
            }
            if (end) {
                pos = map->capacity_;
                return;
            }
            while (pos < map->capacity_ && map->metas_[pos] < 0) {
                pos++;
            }
        }

        reference operator*() const { return {ptr->keys_[pos], ptr->elems_[pos]}; }

        // const_pointer operator->() const { return map->keys + pos; }

        Iterator& operator++() {
            if (!ptr->capacity_) {
                return *this;
            }

            do {
                pos++;
            } while (pos < ptr->capacity_ && ptr->metas_[pos] < 0);

            return *this;
        }

        Iterator operator++(int) {
            auto tmp = *this;
            ++*this;
            return tmp;
        }

        Iterator& operator--() {
            if (!ptr->capacity_) {
                return *this;
            }

            pos--;
            while (pos > 0 && ptr->metas_[pos] < 0) {
                pos--;
            }
            return *this;
        }

        Iterator operator--(int) {
            auto tmp = *this;
            --*this;
            return tmp;
        }

        bool operator==(const Iterator& other) const { return ptr == other.ptr && pos == other.pos; }

        // bool operator!=(const Iterator& other) const { return !(*this == other); }
    };

    using iterator = Iterator;
    using reverse_iterator = std::reverse_iterator<Iterator>;

    iterator begin() noexcept { return iterator(this, false); }
    iterator end() noexcept { return iterator(this, true); }
    reverse_iterator rbegin() noexcept { return reverse_iterator{end()}; }
    reverse_iterator rend() noexcept { return reverse_iterator{begin()}; }
};
static_assert(std::bidirectional_iterator<hash_map<int, int>::Iterator>);
static_assert(std::ranges::bidirectional_range<hash_map<int, int>>);

template<std::unsigned_integral Slot, typename Elem, class Alloc = std::allocator<Elem>>
struct slot_map {
    static constexpr size_t ALIGN = std::max({alignof(bool*), alignof(Elem), alignof(std::max_align_t)});

    struct alignas(ALIGN) Slab {
        std::byte raw[ALIGN];
    };

    using slab_alloc = typename std::allocator_traits<Alloc>::template rebind_alloc<Slab>;
    using elem_alloc = typename std::allocator_traits<Alloc>::template rebind_alloc<Elem>;
    // using KeyAlloc = typename std::allocator_traits<Alloc>::template rebind_alloc<Key>;
    using slab_atr = std::allocator_traits<slab_alloc>;
    using elem_atr = std::allocator_traits<elem_alloc>;
    // using KeyATr = std::allocator_traits<KeyAlloc>;

    slab_alloc sa_;
    elem_alloc ea_;
    // KeyAlloc ka;
    // std::queue<uint32_t> freeSlots;
    Slab* ptr_{};
    Elem* elems_{};
    bool* slots_{};
    size_t capacity_{};
    // size_t nextSlot{};
    size_t size_{};

    slot_map(const slot_map& other)
        : sa_(slab_atr::select_on_container_copy_construction(other.sa_)),
          ea_(sa_),
          capacity_(other.capacity_),
          size_(other.size_) {
        if (capacity_) {
            const size_t slotCap = ((capacity_ * sizeof(bool)) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t elemCap = ((capacity_ * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);

            ptr_ = slab_atr::allocate(sa_, slotCap + elemCap);
            slots_ = reinterpret_cast<bool*>(ptr_);
            elems_ = reinterpret_cast<Elem*>(ptr_ + slotCap);

            std::memcpy(slots_, other.slots_, capacity_ * sizeof(bool));
            for (size_t i = 0; i < capacity_; ++i) {
                if (slots_[i]) {
                    elem_atr::construct(ea_, elems_ + i, std::as_const(other.elems_[i]));
                }
            }
        }
    }

    slot_map(slot_map&& other) noexcept
        : sa_(std::move(other.sa_)),
          ea_(std::move(other.ea_)),
          ptr_(other.ptr_),
          elems_(other.elems_),
          slots_(other.slots_),
          capacity_(other.capacity_),
          size_(other.size_) {
        other.ptr_ = nullptr;
        other.elems_ = nullptr;
        other.slots_ = nullptr;
        other.capacity_ = 0;
        other.size_ = 0;
    }

    slot_map& operator=(const slot_map& other) {
        if (this == &other) {
            return *this;
        }

        release();

        if constexpr (slab_atr::propagate_on_container_copy_assignment::value
                      && std::is_copy_assignable_v<slab_alloc>) {
            sa_ = other.sa_;
            ea_ = elem_alloc(sa_);
        }

        capacity_ = other.capacity_;
        size_ = other.size_;

        if (capacity_) {
            const size_t slotCap = ((capacity_ * sizeof(bool)) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t elemCap = ((capacity_ * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);

            ptr_ = slab_atr::allocate(sa_, slotCap + elemCap);
            slots_ = reinterpret_cast<bool*>(ptr_);
            elems_ = reinterpret_cast<Elem*>(ptr_ + slotCap);

            std::memcpy(slots_, other.slots_, capacity_ * sizeof(bool));
            for (size_t i = 0; i < capacity_; ++i) {
                if (slots_[i]) {
                    elem_atr::construct(ea_, elems_ + i, std::as_const(other.elems_[i]));
                }
            }
        } else {
            ptr_ = nullptr;
            slots_ = nullptr;
            elems_ = nullptr;
        }

        return *this;
    }

    slot_map& operator=(slot_map&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        release();

        if constexpr (slab_atr::propagate_on_container_move_assignment::value
                      && std::is_move_assignable_v<slab_alloc>) {
            sa_ = std::move(other.sa_);
            ea_ = std::move(other.ea_);
        }

        ptr_ = other.ptr_;
        elems_ = other.elems_;
        slots_ = other.slots_;
        capacity_ = other.capacity_;
        size_ = other.size_;

        other.ptr_ = nullptr;
        other.elems_ = nullptr;
        other.slots_ = nullptr;
        other.capacity_ = 0;
        other.size_ = 0;

        return *this;
    }

    slot_map(size_t capacity = 0, Alloc alloc = std::allocator<Elem>())
        : sa_(slab_alloc(alloc)), ea_(sa_), capacity_(capacity) {
        if (capacity) {
            const size_t slotCap = ((capacity * sizeof(bool)) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t elemCap = ((capacity * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);

            ptr_ = slab_atr::allocate(sa_, (slotCap + elemCap));
            slots_ = reinterpret_cast<bool*>(ptr_);
            elems_ = reinterpret_cast<Elem*>(ptr_ + slotCap);

            std::memset(slots_, 0, capacity * sizeof(bool));
        }
    }

    ~slot_map() { release(); }

    void release() noexcept {
        if (capacity_) {
            clear();
            const size_t slotCap = ((capacity_ * sizeof(bool)) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t elemCap = ((capacity_ * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);
            slab_atr::deallocate(sa_, ptr_, slotCap + elemCap);
            reset();
        }
    }

    void reset() noexcept {
        if (capacity_) {
            ptr_ = nullptr;
            slots_ = nullptr;
            elems_ = nullptr;
            capacity_ = 0;
            size_ = 0;
        }
    }

    [[nodiscard]] size_t capacity() const { return capacity_; }
    [[nodiscard]] size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] bool full() const { return size_ == capacity_; }

    bool reserve(size_t newCapacity) {
        if (size_) {
            return false;
        }

        // Empty but possibly already holding a buffer: release it before reallocating.
        release();

        if (newCapacity == 0) {
            return true;
        }

        const size_t newSlotCap = ((newCapacity * sizeof(bool)) + sizeof(Slab) - 1) / sizeof(Slab);
        const size_t newElemCap = ((newCapacity * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);

        auto* newPtr = slab_atr::allocate(sa_, newSlotCap + newElemCap);
        auto* newSlots = reinterpret_cast<bool*>(newPtr);
        auto* newElems = reinterpret_cast<Elem*>(newPtr + newSlotCap);
        std::memset(newSlots, 0, newCapacity);

        ptr_ = newPtr;
        slots_ = newSlots;
        elems_ = newElems;
        capacity_ = newCapacity;

        return true;
    }

    void clear() {
        if (size_) {
            for (size_t i = 0; i < capacity_; ++i) {
                if (slots_[i]) {
                    std::destroy_at(&elems_[i]);
                }
            }
            std::memset(slots_, 0, capacity_);
            size_ = 0;
        }
    }

    [[nodiscard]] bool contains(Slot slot) const {
        if (slot >= capacity_) {
            return false;
        }
        return slots_[slot];
    }

    [[nodiscard]] Slot free_slot() const {
        if (!capacity_ || size_ == capacity_) {
            throw std::bad_alloc();
        }
        size_t slot = 0;
        while (slot < capacity_ && slots_[slot]) {
            slot++;
        }
        if (slot == capacity_) {
            throw std::bad_alloc();
        }
        return static_cast<Slot>(slot);
    }

    // Returns the slot the element was placed in. Throws std::bad_alloc when the
    // map is full (fixed capacity, cannot grow). Use free_slot() to test for
    // room without throwing.
    template<typename... Args>
    Slot emplace(Args&&... args) {
        size_t slot = 0;
        while (slot < capacity_ && slots_[slot]) {
            slot++;
        }
        if (slot == capacity_) { // full: no free slot, avoid an out-of-bounds write
            throw std::bad_alloc();
        }
        elem_atr::construct(ea_, elems_ + slot, std::forward<Args>(args)...);
        slots_[slot] = true;
        size_++;

        return static_cast<Slot>(slot);
    }

    Slot push(const Elem& elem) {
        size_t slot = 0;
        while (slot < capacity_ && slots_[slot]) {
            slot++;
        }
        if (slot == capacity_) {
            throw std::bad_alloc();
        }
        elem_atr::construct(ea_, elems_ + slot, elem);
        slots_[slot] = true;
        size_++;

        return static_cast<Slot>(slot);
    }

    Slot push(Elem&& elem) {
        size_t slot = 0;
        while (slot < capacity_ && slots_[slot]) {
            slot++;
        }
        if (slot == capacity_) {
            throw std::bad_alloc();
        }

        elem_atr::construct(ea_, elems_ + slot, std::move(elem));
        slots_[slot] = true;
        size_++;

        return static_cast<Slot>(slot);
    }

    template<typename... Args>
    bool emplace_at(Slot slot, Args&&... args) {
        if (slot >= capacity_) {
            throw std::bad_alloc();
        }

        if (slots_[slot]) {
            return false; // Slot already used
        }

        elem_atr::construct(ea_, elems_ + slot, std::forward<Args>(args)...);
        slots_[slot] = true;
        size_++;
        return true;
    }

    bool insert(Slot slot, const Elem& elem) {
        if (slot >= capacity_ || slots_[slot]) {
            return false;
        }

        elem_atr::construct(ea_, elems_ + slot, elem);
        slots_[slot] = true;
        size_++;
        return true;
    }

    bool insert(Slot slot, Elem&& elem) {
        if (slot >= capacity_ || slots_[slot]) {
            return false;
        }

        elem_atr::construct(ea_, elems_ + slot, std::move(elem));
        slots_[slot] = true;
        size_++;
        return true;
    }

    bool erase(Slot slot) {
        if (slot >= capacity_ || !slots_[slot]) {
            return false;
        }

        elems_[slot].~Elem();
        slots_[slot] = false;
        size_--;
        return true;
    }

    // std::vector-style checked access: throws std::out_of_range for an
    // out-of-range or empty slot.
    Elem& at(Slot slot) {
        if (slot >= capacity_ || !slots_[slot]) {
            throw std::out_of_range("slot_map::at: invalid slot");
        }

        return elems_[slot];
    }

    [[nodiscard]] const Elem& at(Slot slot) const {
        if (slot >= capacity_ || !slots_[slot]) {
            throw std::out_of_range("slot_map::at: invalid slot");
        }

        return elems_[slot];
    }

    // NOTE: the iterators needs some refining
    struct Iterator {
        using iterator_category = std::forward_iterator_tag;
        using value_type = std::pair<Slot, Elem>;
        using reference = std::pair<const Slot&, Elem&>;
        using difference_type = std::ptrdiff_t;

        slot_map* ptr{};
        Slot pos{};

        Iterator() = default;

        Iterator(slot_map* map, bool end) : ptr(map) {
            if (!map->capacity_) {
                return;
            }

            if (end) {
                pos = map->capacity_;
            } else {
                while (pos < map->capacity_ && !map->slots_[pos]) {
                    pos++;
                }
            }
        }

        reference operator*() const { return {pos, ptr->elems_[pos]}; }

        Iterator& operator++() {
            do {
                pos++;
            } while (pos < ptr->capacity_ && !ptr->slots_[pos]);

            return *this;
        }

        Iterator operator++(int) {
            auto tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const Iterator& other) const { return ptr == other.ptr && pos == other.pos; }

        // bool operator!=(const Iterator& other) const { return !(*this == other); }
    };

    using iterator = Iterator;

    iterator begin() { return iterator(this, false); }
    iterator end() { return iterator(this, true); }
};
static_assert(std::forward_iterator<slot_map<size_t, int>::Iterator>);
static_assert(std::ranges::forward_range<slot_map<size_t, int>>);

// Fixed-capacity circular buffer. Logical index 0 is the oldest live element and
// index size()-1 the newest; the mapping to storage is private, so an eviction at
// the front does not move any element.
//
// Unlike the other containers here, appending to a full ring_array is not an error:
// emplace_back()/push_back() overwrite the oldest element. Check full() beforehand
// when that element still matters.
template<typename Elem, class Alloc = std::allocator<Elem>>
struct ring_array {
    using elem_alloc = typename std::allocator_traits<Alloc>::template rebind_alloc<Elem>;
    using elem_atr = std::allocator_traits<elem_alloc>;

    elem_alloc ea_;
    Elem* elems_{};
    size_t capacity_{};
    size_t size_{};
    size_t head_{};

    ring_array(size_t capacity = 0, Alloc alloc = Alloc()) : ea_(elem_alloc(alloc)), capacity_(capacity) {
        if (capacity_) {
            elems_ = elem_atr::allocate(ea_, capacity_);
        }
    }

    // Copying normalises the storage: the copy keeps head_ at 0, so logical index i
    // lands in elems_[i].
    ring_array(const ring_array& other)
        : ea_(elem_atr::select_on_container_copy_construction(other.ea_)),
          capacity_(other.capacity_),
          size_(other.size_) {
        if (capacity_) {
            elems_ = elem_atr::allocate(ea_, capacity_);
            for (size_t i = 0; i < size_; i++) {
                elem_atr::construct(ea_, elems_ + i, std::as_const(other[i]));
            }
        }
    }

    ring_array(ring_array&& other) noexcept
        : ea_(std::move(other.ea_)),
          elems_(other.elems_),
          capacity_(other.capacity_),
          size_(other.size_),
          head_(other.head_) {
        other.elems_ = nullptr;
        other.capacity_ = 0;
        other.size_ = 0;
        other.head_ = 0;
    }

    ring_array& operator=(const ring_array& other) {
        if (this == &other) {
            return *this;
        }

        release();

        if constexpr (elem_atr::propagate_on_container_copy_assignment::value
                      && std::is_copy_assignable_v<elem_alloc>) {
            ea_ = other.ea_;
        }

        capacity_ = other.capacity_;
        size_ = other.size_;
        head_ = 0;

        if (capacity_) {
            elems_ = elem_atr::allocate(ea_, capacity_);
            for (size_t i = 0; i < size_; i++) {
                elem_atr::construct(ea_, elems_ + i, std::as_const(other[i]));
            }
        }

        return *this;
    }

    ring_array& operator=(ring_array&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        release();

        if constexpr (elem_atr::propagate_on_container_move_assignment::value
                      && std::is_move_assignable_v<elem_alloc>) {
            ea_ = std::move(other.ea_);
        }

        elems_ = other.elems_;
        capacity_ = other.capacity_;
        size_ = other.size_;
        head_ = other.head_;

        other.elems_ = nullptr;
        other.capacity_ = 0;
        other.size_ = 0;
        other.head_ = 0;

        return *this;
    }

    ~ring_array() { release(); }

    [[nodiscard]] size_t capacity() const { return capacity_; }
    [[nodiscard]] size_t size() const { return size_; }
    [[nodiscard]] bool empty() const { return size_ == 0; }
    [[nodiscard]] bool full() const { return size_ == capacity_; }

    void clear() {
        while (size_) {
            size_--;
            elem_atr::destroy(ea_, elems_ + ((head_ + size_) % capacity_));
        }
        head_ = 0;
    }

    void release() noexcept {
        if (capacity_) {
            clear();
            elem_atr::deallocate(ea_, elems_, capacity_);
            elems_ = nullptr;
            capacity_ = 0;
        }
    }

    bool reserve(size_t newCapacity) {
        // Only possible while empty: growing would have to relocate the live
        // elements, and Elem is not required to be movable.
        if (size_) {
            return false;
        }

        release();
        capacity_ = newCapacity;
        if (capacity_) {
            elems_ = elem_atr::allocate(ea_, capacity_);
        }
        return true;
    }

    Elem& operator[](size_t index) { return elems_[(head_ + index) % capacity_]; }
    const Elem& operator[](size_t index) const { return elems_[(head_ + index) % capacity_]; }

    Elem& at(size_t index) {
        // use logical index where 0 is the oldest and size() - 1 is the latest inserted element
        if (index >= size_) {
            throw std::out_of_range("ring_array::at: index out of range");
        }

        return elems_[(head_ + index) % capacity_];
    }

    [[nodiscard]] const Elem& at(size_t index) const {
        // use logical index where 0 is the oldest and size() - 1 is the latest inserted element
        if (index >= size_) {
            throw std::out_of_range("ring_array::at: index out of range");
        }

        return elems_[(head_ + index) % capacity_];
    }

    Elem& front() { return at(0); }
    [[nodiscard]] const Elem& front() const { return at(0); }
    Elem& back() { return at(size_ - 1); }
    [[nodiscard]] const Elem& back() const { return at(size_ - 1); }

    template<typename... Args>
    Elem& emplace_back(Args&&... args) {
        // if no more possible simply replace the oldest entry. Don't error
        if (!capacity_) {
            throw std::bad_alloc(); // no storage at all, there is nothing to overwrite
        }

        if (size_ == capacity_) {
            // The oldest element sits exactly where the new one belongs, so dropping
            // it frees the slot that the append below writes to.
            pop_front();
        }

        const size_t pos = (head_ + size_) % capacity_;
        elem_atr::construct(ea_, elems_ + pos, std::forward<Args>(args)...);
        size_++;
        return elems_[pos];
    }

    Elem& push_back(const Elem& elem) {
        // if no more possible simply replace the oldest entry. Don't error
        return emplace_back(elem);
    }
    Elem& push_back(Elem&& elem) {
        // if no more possible simply replace the oldest entry. Don't error
        return emplace_back(std::move(elem));
    }

    bool pop_front() {
        if (!size_) {
            return false;
        }

        elem_atr::destroy(ea_, elems_ + head_);
        head_ = (head_ + 1) % capacity_;
        size_--;
        if (!size_) {
            head_ = 0;
        }
        return true;
    }

    bool pop_back() {
        if (!size_) {
            return false;
        }

        size_--;
        elem_atr::destroy(ea_, elems_ + ((head_ + size_) % capacity_));
        if (!size_) {
            head_ = 0;
        }
        return true;
    }

    struct Iterator {
        using iterator_category = std::forward_iterator_tag;
        using difference_type = std::ptrdiff_t;
        using value_type = Elem;
        using pointer = Elem*;
        using reference = Elem&;

        ring_array* ptr{};
        size_t index{};

        Iterator() = default;
        Iterator(ring_array* ptr, size_t index) : ptr(ptr), index(index) {}

        reference operator*() const { return (*ptr)[index]; }
        pointer operator->() const { return &(*ptr)[index]; }

        Iterator& operator++() {
            index++;
            return *this;
        }

        Iterator operator++(int) {
            auto tmp = *this;
            ++(*this);
            return tmp;
        }

        bool operator==(const Iterator& other) const { return ptr == other.ptr && index == other.index; }
    };

    using iterator = Iterator;

    iterator begin() { return iterator(this, 0); }
    iterator end() { return iterator(this, size_); }
};
static_assert(std::forward_iterator<ring_array<int>::Iterator>);
static_assert(std::ranges::forward_range<ring_array<int>>);

// NOLINTEND(readability-identifier-naming)

} // namespace Midori

// namespace Base {

// // Memory

// template<typename T>
// concept Allocator = requires(const T& a, void*) {
//     { a.Allocate(std::declval<size_t>(), std::declval<size_t>()) } -> std::same_as<void*>;
//     { a.Deallocate(std::declval<void*>()) };
// };

// struct Heap {
//     static void* Allocate(size_t size, size_t alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__);
//     static void Deallocate(void* ptr) noexcept;
// };
// static_assert(Allocator<Heap>);

// template<Allocator Alloc>
// struct Arena {
//     static void* Allocate(size_t size, size_t alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__);
//     static void Deallocate(void* ptr) noexcept;
// };
// static_assert(Allocator<Arena<Heap>>);

// template<typename Elem, Allocator Alloc>
// struct Pool {
//     static void* Allocate(size_t size, size_t alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__);
//     static void Deallocate(void* ptr) noexcept;
// };
// static_assert(Allocator<Pool<void, Heap>>);

// // Containers

// template<typename T>
// concept Hashable = requires(const T& a) {
//     { std::hash<T>{}(a) } -> std::convertible_to<std::size_t>;
// };

// template<typename T>
// concept Keyable = Hashable<T> && std::equality_comparable<T> && std::is_trivially_copyable_v<T>;

// template<typename Elem, Allocator Alloc>
// struct Array {
//     Alloc alloc;
//     Elem* elems;
//     size_t size{};
//     size_t capacity{};
// };

// template<Allocator Alloc>
// struct String {
//     Alloc alloc;
//     char* chars;
//     size_t capacity;
//     size_t length;
//     size_t size;
// };

// template<Keyable Key, typename Elem, Allocator Alloc>
// struct FlatHashMap {
//     Alloc alloc;
//     int8_t* ctrl;
//     Key* keys;
//     Elem* elems;
//     size_t size;
//     size_t capacity;
// };

// template<Keyable Key, Allocator Alloc>
// struct FlatHashSet {
//     Alloc alloc;
//     int8_t* ctrl;
//     Key* keys;
//     size_t size;
//     size_t capacity;
// };

// } // namespace Base
