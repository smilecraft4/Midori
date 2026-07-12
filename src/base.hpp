#pragma once

#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <queue>
#include <source_location>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <limits>
#include <numbers>
#include <utility>

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

void* Malloc(size_t size);
void* Calloc(size_t value, size_t size);
void* Realloc(void* ptr, size_t size);
void Free(void* ptr);

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
        // std::align takes `space` by reference and decrements it by the alignment
        // padding, so it must be a local holding the *remaining* space, not the
        // member `capacity` (passing the member would permanently shrink it).
        size_t space = capacity - Size();
        if (std::align(alignment, size, ptr, space) == nullptr) {
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

    ChunkAlloc alloc;
    Chunk* free{};
    size_t count{};

    // TODO Add RAII stuff

    explicit Pool(size_t count) : alloc(std::allocator<Chunk>()), count(count) {
        Chunk* prev{};
        for (size_t c = 0; c < count; c++) {
            free = alloc.allocate(1);
            free->next = prev;
            prev = free;
        }
    }

    explicit Pool(size_t count, Alloc allocator) : alloc(ChunkAlloc(allocator)), count(count) {
        Chunk* prev{};
        for (size_t c = 0; c < count; c++) {
            free = alloc.allocate(1);
            free->next = prev;
            prev = free;
        }
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
            throw std::bad_alloc();
        }
        Chunk* chunk = free;
        free = free->next;
        count--;
        return ::new (&chunk->data) T(std::forward<Args>(args)...);
    }

    void Deallocate(T* obj) {
        obj->~T();
        auto* chunk = reinterpret_cast<Chunk*>(obj);
        chunk->next = free;
        free = chunk;
        count++;
    }

    void Clear() {
        while (free != nullptr) {
            const auto prev = free;
            free = free->next;
            alloc.deallocate(prev, 1);
            count--;
        }
        // Assert(count == 0, "Did not cleared all owned chunks");
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

template<typename Vec, typename T>
bool PushUnique(Vec& vec, const T& value) {
    for (const auto& elem : vec) {
        if (elem == value) {
            return false;
        }
    }
    vec.push_back(value);
    return true;
}

template<typename T>
concept Hashable = requires(const T& a) {
    { std::hash<T>{}(a) } -> std::convertible_to<std::size_t>;
};

template<typename T>
concept Keyable = Hashable<T> && std::equality_comparable<T> && std::is_trivially_copyable_v<T>;

constexpr size_t Splitmix64(size_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

template<Keyable Key, class Alloc = std::allocator<Key>>
struct HashSet {
    static constexpr uint8_t FULL = 0b01111111;     // low 7 bits used for hash
    static constexpr uint8_t EMPTY = 0b10000000;    // free to use
    static constexpr uint8_t DELETED = 0b11111110;  // tombstone ??
    static constexpr uint8_t SENTINEL = 0b11111111; // end of table for iteration
    static constexpr size_t GROUP_WIDTH = 16;

    using SlabAlloc = typename std::allocator_traits<Alloc>::template rebind_alloc<Key>;
    using SlabAllocTraits = std::allocator_traits<SlabAlloc>;

    SlabAlloc alloc;
    uint8_t* metas{};
    Key* keys{};
    size_t size{};
    size_t capacity{};

    HashSet(const HashSet& other)
        : alloc(SlabAllocTraits::select_on_container_copy_construction(other.alloc)),
          size(other.size),
          capacity(other.capacity) {
        if (capacity) {
            const size_t metaCap = (((capacity * sizeof(uint8_t)) + GROUP_WIDTH) + sizeof(Key) - 1) / sizeof(Key);

            auto* ptr = SlabAllocTraits::allocate(alloc, capacity + metaCap);
            std::memcpy(ptr, other.metas, (capacity + metaCap) * sizeof(Key));

            metas = reinterpret_cast<uint8_t*>(ptr);
            keys = ptr + metaCap;
        }
    }

    HashSet(HashSet&& other) noexcept
        : alloc(std::move(other.alloc)),
          metas(other.metas),
          keys(other.keys),
          size(other.size),
          capacity(other.capacity) {
        other.Reset();
    }

    HashSet& operator=(const HashSet& other) {
        if (this == &other) {
            return *this;
        }

        Key* newKeys = nullptr;
        if (other.capacity) {
            const size_t metaCap = (((capacity * sizeof(uint8_t)) + GROUP_WIDTH) + sizeof(Key) - 1) / sizeof(Key);

            auto* ptr = SlabAllocTraits::allocate(alloc, capacity + metaCap);
            std::memcpy(ptr, other.metas, (capacity + metaCap) * sizeof(Key));

            metas = reinterpret_cast<uint8_t*>(ptr);
            keys = ptr + metaCap;
        }

        Release();

        keys = newKeys;
        size = other.size;
        capacity = other.capacity;

        return *this;
    }

    HashSet& operator=(HashSet&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        Release();

        alloc = std::move(other.alloc);
        metas = other.metas;
        keys = other.keys;
        size = other.size;
        capacity = other.capacity;

        other.Reset();

        return *this;
    }

    HashSet(size_t capacity, Alloc alloc = std::allocator<Key>()) : capacity(capacity), alloc(alloc) {
        if (capacity) {
            const size_t metaCap = (((capacity * sizeof(uint8_t)) + GROUP_WIDTH) + sizeof(Key) - 1) / sizeof(Key);

            auto* ptr = SlabAllocTraits::allocate(alloc, capacity + metaCap);

            metas = reinterpret_cast<uint8_t*>(ptr);
            keys = ptr + metaCap;

            std::memset(metas, EMPTY, capacity);
            metas[capacity] = SENTINEL;
        }
    }

    ~HashSet() { Release(); }

    [[nodiscard]] size_t Size() const { return size; }
    [[nodiscard]] size_t Capacity() const { return capacity; }

    void Release() noexcept {
        if (capacity) {
            const size_t metaCap = (((capacity * sizeof(uint8_t)) + GROUP_WIDTH) + sizeof(Key) - 1) / sizeof(Key);

            SlabAllocTraits::deallocate(alloc, keys - metaCap, capacity);

            keys = nullptr;
            metas = nullptr;
        }
    }

    void Reset() noexcept {
        if (capacity) {
            metas = nullptr;
            keys = nullptr;
            capacity = 0;
            size = 0;
        }
    }

    bool Reserve(size_t newCapacity) {
        if (size > newCapacity) {
            return false;
        }

        const size_t metaCap = (((capacity * sizeof(uint8_t)) + GROUP_WIDTH) + sizeof(Key) - 1) / sizeof(Key);

        auto* ptr = SlabAllocTraits::allocate(alloc, newCapacity + metaCap);
        auto* newMetas = reinterpret_cast<uint8_t*>(ptr);
        auto* newKeys = ptr + metaCap;

        std::memset(newMetas, EMPTY, newCapacity);
        metas[capacity] = SENTINEL;

        size_t newSize{};
        if (size > 0) {
            // TODO: use iterators instead
            for (size_t pos = 0; pos < capacity; pos++) {
                if ((metas[pos] != EMPTY && metas[pos] != DELETED)) {
                    const auto key = keys[pos];
                    auto hash = std::hash<Key>{}(key);
                    const uint8_t h2 = hash & 0x7F;
                    size_t newPos = (hash >> 7) % newCapacity;

                    bool inserted = false;
                    for (size_t probed = 0; probed < capacity; ++probed) {
                        const uint8_t m = newMetas[newPos];
                        if (m == EMPTY || m == DELETED) {
                            inserted = true;
                            newMetas[newPos] = h2;
                            newKeys[newPos] = key;
                            newSize++;
                            break;
                        }
                        newPos = (newPos + 1) % newCapacity;
                    }

                    if (newSize == size) {
                        break;
                    }
                }
            }
        }

        if (newSize < size) {
            if (newCapacity) {
                SlabAllocTraits::deallocate(alloc, ptr, metaCap + newCapacity);
            }
            return false;
        } else {
            Release();

            keys = newKeys;
            metas = newMetas;
            size = newSize;
            capacity = newCapacity;

            return true;
        }
    }

    [[nodiscard]] bool Contains(const Key& key) const {
        if (size == 0) {
            return false;
        }

        const size_t hash = std::hash<Key>{}(key);
        const uint8_t h2 = hash & 0x7F;      // low 7 bits, high bit clear
        size_t pos = (hash >> 7) % capacity; // H1: start slot

        for (size_t probed = 0; probed < capacity; ++probed) {
            const uint8_t m = metas[pos];
            if (m == h2) { // full + H2 match -> candidate
                if (keys[pos] == key) {
                    return true;
                }
            } else if (m == EMPTY) { // empty -> not present, stop
                return false;
            }
            // Deleted tombstone, or full-but-different-H2 -> keep probing
            pos = (pos + 1) % capacity;
        }

        return false;
    }

    bool Insert(const Key& key) {
        if (capacity == 0) {
            return false;
        }
        if (capacity == size) {
            return false;
        }

        const size_t hash = std::hash<Key>{}(key);
        const uint8_t h2 = hash & 0x7F;      // low 7 bits, high bit clear
        size_t pos = (hash >> 7) % capacity; // H1: start slot

        for (size_t probed = 0; probed < capacity; ++probed) {
            const uint8_t m = metas[pos];
            if (m == h2) {
                if (keys[pos] == key) {
                    return true;
                }
            }
            if (m == EMPTY || m == DELETED) { // empty -> available to use
                metas[pos] = h2;
                keys[pos] = key;
                size++;
                return true;
            }
            // Deleted tombstone, or full H2 -> keep probing
            pos = (pos + 1) % capacity;
        }

        return false;
    }

    bool Erase(const Key& key) {
        if (size == 0) {
            return false;
        }

        const size_t hash = std::hash<Key>{}(key);
        const uint8_t h2 = hash & 0x7F;      // low 7 bits, high bit clear
        size_t pos = (hash >> 7) % capacity; // H1: start slot

        for (size_t probed = 0; probed < capacity; ++probed) {
            const uint8_t m = metas[pos];
            if (m == h2) { // full + H2 match -> candidate
                if (keys[pos] == key) {
                    metas[pos] = DELETED;
                    size--;
                    return true;
                }
            } else if (m == EMPTY) { // empty -> not present, stop
                return false;
            }
            // Deleted tombstone, or full H2 -> keep probing
            pos = (pos + 1) % capacity;
        }
        return false;
    }

    struct Iterator {};
};

template<Keyable Key, typename Elem, class Alloc = std::allocator<Key>>
struct HashMap {
    static constexpr uint8_t FULL = 0b01111111;     // low 7 bits used for hash
    static constexpr uint8_t EMPTY = 0b10000000;    // free to use
    static constexpr uint8_t DELETED = 0b11111110;  // tombstone
    static constexpr uint8_t SENTINEL = 0b11111111; // tombstone
    static constexpr size_t GROUP_WIDTH = 16;
    static constexpr size_t ALIGN = std::max({alignof(Key), alignof(Elem), alignof(std::max_align_t)});

    struct alignas(ALIGN) Slab {
        std::byte raw[ALIGN];
    };

    using SlabAlloc = typename std::allocator_traits<Alloc>::template rebind_alloc<Slab>;
    using ElemAlloc = typename std::allocator_traits<Alloc>::template rebind_alloc<Elem>;
    using KeyAlloc = typename std::allocator_traits<Alloc>::template rebind_alloc<Key>;
    using SlabATr = std::allocator_traits<SlabAlloc>;
    using ElemATr = std::allocator_traits<ElemAlloc>;
    using KeyATr = std::allocator_traits<KeyAlloc>;

    SlabAlloc sa;
    ElemAlloc ea{sa};
    Slab* ptr;
    uint8_t* metas{};
    Key* keys{};
    Elem* elems{};
    size_t size{};
    size_t capacity{};

    HashMap(const HashMap& other)
        : sa(SlabATr::select_on_container_copy_construction(other.sa)),
          ea(sa),
          size(other.size),
          capacity(other.capacity) {
        if (capacity) {
            const size_t metaCap = (((capacity * sizeof(uint8_t)) + GROUP_WIDTH) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t keyCap = ((capacity * sizeof(Key)) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t elemCap = ((capacity * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);

            ptr = SlabATr::allocate(sa, metaCap + keyCap + elemCap);
            std::memcpy(ptr, other.metas, (metaCap + keyCap + elemCap) * sizeof(Slab));

            metas = reinterpret_cast<uint8_t*>(ptr);
            keys = reinterpret_cast<Key*>(ptr + metaCap);
            elems = reinterpret_cast<Elem*>(ptr + metaCap + keyCap);
        }
    }

    HashMap(HashMap&& other) noexcept
        : sa(std::move(other.sa)),
          ea(std::move(other.ea)),
          ptr(other.ptr),
          metas(other.metas),
          keys(other.keys),
          elems(other.elems),
          size(other.size),
          capacity(other.capacity) {
        other.Reset();
    }

    HashMap& operator=(const HashMap& other) {
        if (this == &other) {
            return *this;
        }

        Slab* newPtr{};
        uint8_t* newMetas{};
        Key* newKeys{};
        Elem* newElems{};

        if (other.capacity) {
            const size_t metaCap = (((capacity * sizeof(uint8_t)) + GROUP_WIDTH) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t keyCap = ((capacity * sizeof(Key)) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t elemCap = ((capacity * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);
            newPtr = SlabATr::allocate(sa, metaCap + keyCap + elemCap);

            std::memcpy(newPtr, other.ptr, (metaCap + keyCap + elemCap) * sizeof(Slab));

            newMetas = reinterpret_cast<uint8_t*>(newPtr);
            newKeys = reinterpret_cast<Key*>(newPtr + metaCap);
            newElems = reinterpret_cast<Elem*>(newPtr + metaCap + keyCap);
        }

        Release();

        ptr = newPtr;
        metas = newMetas;
        keys = newKeys;
        elems = newElems;
        size = other.size;
        capacity = other.capacity;

        return *this;
    }

    HashMap& operator=(HashMap&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        Release();

        sa = std::move(other.sa);
        ea = std::move(other.ea);
        ptr = other.ptr;
        metas = other.metas;
        keys = other.keys;
        elems = other.elems;
        size = other.size;
        capacity = other.capacity;

        other.Reset();

        return *this;
    }

    HashMap(size_t capacity, Alloc alloc) : capacity(capacity), sa(SlabAlloc(alloc)), ea(sa) {
        if (capacity) {
            const size_t metaCap = (((capacity * sizeof(uint8_t)) + GROUP_WIDTH) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t keyCap = ((capacity * sizeof(Key)) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t elemCap = ((capacity * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);

            ptr = SlabATr::allocate(sa, (metaCap + keyCap + elemCap));
            metas = reinterpret_cast<uint8_t*>(ptr);
            keys = reinterpret_cast<Key*>(ptr + metaCap);
            elems = reinterpret_cast<Elem*>(ptr + metaCap + keyCap);

            std::memset(metas, EMPTY, capacity * sizeof(uint8_t));
            metas[capacity] = SENTINEL;
        }
    }

    HashMap(size_t capacity) : capacity(capacity), sa(std::allocator<Slab>()), ea(sa) {
        if (capacity) {
            const size_t metaCap = (((capacity * sizeof(uint8_t)) + GROUP_WIDTH) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t keyCap = ((capacity * sizeof(Key)) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t elemCap = ((capacity * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);

            ptr = SlabATr::allocate(sa, (metaCap + keyCap + elemCap));

            metas = reinterpret_cast<uint8_t*>(ptr);
            keys = reinterpret_cast<Key*>(ptr + metaCap);
            elems = reinterpret_cast<Elem*>(ptr + metaCap + keyCap);

            std::memset(metas, EMPTY, capacity * sizeof(uint8_t));
            metas[capacity] = SENTINEL;
        }
    }

    ~HashMap() { Release(); }

    [[nodiscard]] size_t Size() const { return size; }
    [[nodiscard]] size_t Capacity() const { return capacity; }

    void Release() noexcept {
        if (capacity) {
            const size_t metaCap = (((capacity * sizeof(uint8_t)) + GROUP_WIDTH) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t keyCap = ((capacity * sizeof(Key)) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t elemCap = ((capacity * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);

            SlabATr::deallocate(sa, ptr, metaCap + keyCap + elemCap);

            ptr = nullptr;
            metas = nullptr;
            keys = nullptr;
            elems = nullptr;
            capacity = 0;
            size = 0;
        }
    }

    void Reset() noexcept {
        if (capacity) {
            ptr = nullptr;
            metas = nullptr;
            keys = nullptr;
            elems = nullptr;
            capacity = 0;
            size = 0;
        }
    }

    bool Reserve(size_t newCapacity) {
        if (size > newCapacity) {
            return false;
        }

        const size_t metaCap = (((newCapacity * sizeof(uint8_t)) + GROUP_WIDTH) + sizeof(Slab) - 1) / sizeof(Slab);
        const size_t keyCap = ((newCapacity * sizeof(Key)) + sizeof(Slab) - 1) / sizeof(Slab);
        const size_t elemCap = ((newCapacity * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);

        auto* newPtr = SlabATr::allocate(sa, metaCap + keyCap + elemCap);
        auto* newMetas = reinterpret_cast<uint8_t*>(newPtr);
        auto* newKeys = reinterpret_cast<Key*>(newPtr + metaCap);
        auto* newElems = reinterpret_cast<Elem*>(newPtr + metaCap + keyCap);

        std::memset(newMetas, EMPTY, newCapacity + 1);
        newMetas[newCapacity] = SENTINEL;

        size_t newSize{};
        if (size > 0) {
            // TODO: use iterators instead
            for (size_t pos = 0; pos < capacity; pos++) {
                if ((metas[pos] != EMPTY && metas[pos] != DELETED)) {
                    const auto hash = std::hash<Key>{}(keys[pos]);
                    const uint8_t h2 = H2(hash);
                    size_t newPos = H1(hash) % newCapacity;

                    for (size_t probed = 0; probed < capacity; ++probed) {
                        const uint8_t m = newMetas[newPos];
                        if (m == EMPTY || m == DELETED) {
                            newMetas[newPos] = h2;
                            newKeys[newPos] = keys[pos];
                            newElems[newPos] = elems[pos]; // may be a copy, bad !!
                            newSize++;
                            break;
                        }
                        newPos = (newPos + 1) % newCapacity;
                    }

                    if (newSize == size) {
                        break;
                    }
                }
            }
        }

        if (newSize < size) {
            if (newCapacity) {
                SlabATr::deallocate(sa, newPtr, metaCap + keyCap + elemCap);
            }
            return false;
        } else {
            Release();

            ptr = newPtr;
            keys = newKeys;
            metas = newMetas;
            elems = newElems;
            size = newSize;
            capacity = newCapacity;

            return true;
        }
    }

    [[nodiscard]] bool Contains(const Key& key) const {
        if (size == 0) {
            return false;
        }

        const size_t hash = std::hash<Key>{}(key);
        const uint8_t h2 = H2(hash);
        size_t pos = H1(hash) % capacity;

        for (size_t probed = 0; probed < capacity; ++probed) {
            const uint8_t m = metas[pos];
            if (m == h2) {
                if (keys[pos] == key) {
                    return true;
                }
            } else if (m == EMPTY) {
                return false;
            }
            pos = (pos + 1) % capacity;
        }

        return false;
    }

    static constexpr size_t H2(size_t hash) noexcept { return hash & FULL; }

    static constexpr size_t H1(size_t hash) noexcept { return hash >> 7; }

    bool Insert(const Key& key, const Elem& elem) {
        if (capacity == 0 || capacity == size) {
            return false;
        }

        const size_t hash = std::hash<Key>{}(key);
        const uint8_t h2 = H2(hash);
        size_t pos = H1(hash) % capacity;

        for (size_t probed = 0; probed < capacity; ++probed) {
            const uint8_t m = metas[pos];
            if (m == h2) {
                if (keys[pos] == key) {
                    return true;
                }
            }
            if (m == EMPTY || m == DELETED) {
                metas[pos] = h2;
                keys[pos] = key;
                elems[pos] = elem;
                size++;
                return true;
            }
            pos = (pos + 1) % capacity;
        }

        return false;
    }

    bool Insert(const Key& key, Elem&& elem) {
        if (capacity == 0 || capacity == size) {
            return false;
        }

        const size_t hash = std::hash<Key>{}(key);
        const uint8_t h2 = H2(hash);
        size_t pos = H1(hash) % capacity;

        for (size_t probed = 0; probed < capacity; ++probed) {
            const uint8_t m = metas[pos];
            if (m == h2) {
                if (keys[pos] == key) {
                    return true;
                }
            }
            if (m == EMPTY || m == DELETED) {
                metas[pos] = h2;
                keys[pos] = key;
                elems[pos] = elem;
                size++;
                return true;
            }
            pos = (pos + 1) % capacity;
        }

        return false;
    }

    template<typename... Args>
    Elem& Emplace(const Key& key, Args&&... args) {
        const size_t hash = std::hash<Key>{}(key);
        const uint8_t h2 = H2(hash);
        size_t pos = H1(hash) % capacity;

        for (size_t probed = 0; probed < capacity; ++probed) {
            const uint8_t m = metas[pos];
            if (m == h2) {
                if (keys[pos] == key) {
                    return elems[pos];
                }
            }
            if (m == EMPTY || m == DELETED) {
                metas[pos] = h2;
                keys[pos] = key;
                ElemATr::construct(ea, elems + pos, std::forward<Args>(args)...);
                size++;
                return elems[pos];
            }
            pos = (pos + 1) % capacity;
        }
        
        return elems[pos];
    }

    bool Erase(const Key& key) {
        if (size == 0) {
            return false;
        }

        const size_t hash = std::hash<Key>{}(key);
        const uint8_t h2 = H2(hash);
        size_t pos = H1(hash) % capacity;

        for (size_t probed = 0; probed < capacity; ++probed) {
            const uint8_t m = metas[pos];
            if (m == h2) {
                if (keys[pos] == key) {
                    metas[pos] = DELETED;
                    elems[pos].~Elem();
                    size--;
                    return true;
                }
            } else if (m == EMPTY) {
                return false;
            }
            pos = (pos + 1) % capacity;
        }
        return false;
    }

    Elem& At(const Key& key) {
        if (size == 0) {
            throw std::runtime_error("");
        }

        const size_t hash = std::hash<Key>{}(key);
        const uint8_t h2 = H2(hash);
        size_t pos = H1(hash) % capacity;

        for (size_t probed = 0; probed < capacity; ++probed) {
            const uint8_t m = metas[pos];
            if (m == h2) {
                if (keys[pos] == key) {
                    return elems[pos];
                }
            } else if (m == EMPTY) {
                throw std::runtime_error("");
            }
            pos = (pos + 1) % capacity;
        }

        throw std::runtime_error("");
    }

    Elem& At(const Key& key) const {
        if (size == 0) {
            throw std::runtime_error("");
        }

        const size_t hash = std::hash<Key>{}(key);
        const uint8_t h2 = H2(hash);
        size_t pos = H1(hash) % capacity;

        for (size_t probed = 0; probed < capacity; ++probed) {
            const uint8_t m = metas[pos];
            if (m == h2) {
                if (keys[pos] == key) {
                    return elems[pos];
                }
            } else if (m == EMPTY) {
                throw std::runtime_error("");
            }
            pos = (pos + 1) % capacity;
        }

        throw std::runtime_error("");
    }

    struct Iterator {};
};

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