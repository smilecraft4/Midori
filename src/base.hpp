#pragma once

#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <queue>
#include <source_location>
#include <string>
#include <thread>
#include <type_traits>
#include <limits>
#include <numbers>

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

void Assert(bool cond, const char* msg, const std::source_location& loc = std::source_location::current());

void *Malloc(size_t size);
void *Calloc(size_t value, size_t size);
void *Realloc(void *ptr, size_t size);
void Free(void *ptr);

template<class Alloc = std::allocator<std::byte>>
struct Arena {
    Alloc alloc;
    std::byte* data{};
    std::byte* head{};
    size_t capacity{};

    // TODO Add RAII stuff

    explicit Arena(size_t capacity) : capacity(capacity), alloc(std::allocator<std::byte>()) {
        data = alloc.allocate(capacity);
        head = data;
    }

    explicit Arena(size_t capacity, Alloc allocator) : capacity(capacity), alloc(allocator) {
        data = alloc.allocate(capacity);
        head = data;
    }

    ~Arena() {
        alloc.deallocate(data, capacity);
        capacity = 0;
        head = nullptr;
        data = nullptr;
    }

    [[nodiscard]] size_t Size() const { return head - data; }

    [[nodiscard]] size_t Capacity() const { return capacity; }

    void* Allocate(size_t size, size_t alignment = __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
        void* ptr = head;
        ptr = std::align(alignment, size, ptr, capacity);
        if (ptr == nullptr || (static_cast<std::byte*>(ptr) + size) > (data + capacity)) {
            return nullptr;
        }
        head = static_cast<std::byte*>(ptr) + size;
        return ptr;
    }

    void Deallocate(void* ptr, size_t size = 0) {}

    void Reset() { head = data; }
};

// NOLINTBEGIN(readability-identifier-naming)
template<typename T, class Alloc = std::allocator<std::byte>>
struct arena_allocator {
    Arena<Alloc>& arena;

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

    arena_allocator(Arena<Alloc>& arena) : arena(arena) {}

    arena_allocator(const arena_allocator& other) = default;

    template<typename U>
    arena_allocator(const arena_allocator<U>& other) : arena(other.arena) {}

    T* allocate(std::size_t n) {
        if (n == 0) {
            return nullptr;
        }

        void* ptr = arena.Allocate(sizeof(T) * n, alignof(T));
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

    bool operator==(const arena_allocator<T>& other) const noexcept { return &arena == &other.arena; }

    bool operator!=(const arena_allocator<T>& other) const noexcept { return !(*this == other); }
};
// NOLINTEND(readability-identifier-naming)

template<typename T, class Alloc = std::allocator<T>>
struct Pool {
    union Chunk {
        Chunk* next;
        T data;
    };

    using ChunkAlloc = typename std::allocator_traits<Alloc>::template rebind_alloc<Chunk>;
    using ChunkTraits = std::allocator_traits<ChunkAlloc>;

    ChunkAlloc alloc;
    Chunk* free{};
    size_t count{};

    // TODO Add RAII stuff

    explicit Pool(size_t count) : alloc(std::allocator<Chunk>()), count(count) {
        Chunk* blocks = alloc.allocate(count);
        for (size_t c = 0; c + 1 < count; c++) {
            blocks[c].next = &blocks[c + 1];
        }
        if (count > 0) {
            blocks[count - 1].next = nullptr;
        }
        free = blocks;
    }

    explicit Pool(size_t count, Alloc allocator) : alloc(ChunkAlloc(allocator)), count(count) {
        Chunk* blocks = alloc.allocate(count);
        for (size_t c = 0; c + 1 < count; c++) {
            blocks[c].next = &blocks[c + 1];
        }
        if (count > 0) {
            blocks[count - 1].next = nullptr;
        }
        free = blocks;
    }

    ~Pool() { Clear(); }

    [[nodiscard]] size_t Count() const { return count; }

    [[nodiscard]] size_t Available() const {
        size_t available = 0;
        Chunk* last = free;
        while (last) {
            available++;
            last = last->next;
        }
        return available;
    }

    template<typename... Args>
    T* Allocate(Args&&... args) {
        if (!free) {
            free = alloc.allocate(1);
            count++;
            free->next = nullptr;
        }
        Chunk* chunk = free;
        free = free->next;
        return ::new (&chunk->data) T(std::forward<Args>(args)...);
    }

    void Deallocate(T* obj) {
        obj->~T();
        auto* chunk = reinterpret_cast<Chunk*>(obj);
        chunk->next = free;
        free = chunk;
    }

    void Clear() {
        while (free->next != nullptr) {
            Chunk* prev = free;
            free = free->next;
            alloc.deallocate(prev, 1);
        }
        count = 0;
    }
};

struct ThreadPool {
    ThreadPool(size_t size, const std::string& name = "Worker");
    ~ThreadPool();

    void Emplace(std::function<void()> task);

    void Wait();
    void Stop();

    std::condition_variable cv{};
    std::queue<std::function<void()>> tasks;
    std::vector<std::thread> threads;
    std::mutex tasksMtx;
    bool wait{};
    bool stop{};
};
