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

// NOLINTBEGIN(readability-identifier-naming)
template<typename T, class Alloc = std::allocator<std::byte>>
struct arena_allocator {
    arena<Alloc>& arena_;

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

    arena_allocator(arena<Alloc>& arena) : arena_(arena) {}

    arena_allocator(const arena_allocator& other) = default;

    template<typename U>
    arena_allocator(const arena_allocator<U>& other) : arena_(other.arena_) {}

    T* allocate(std::size_t n) {
        if (n == 0) {
            return nullptr;
        }

        void* ptr = arena_.allocate(sizeof(T) * n, alignof(T));
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

    bool operator==(const arena_allocator<T>& other) const noexcept { return &arena_ == &other.arena_; }

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

    [[nodiscard]] size_t count() const { return count_; }

    template<typename... Args>
    T* Allocate(size_t n, Args&&... args) {
        if (!count_) {
            throw std::bad_alloc();
        }

        Chunk* chunk{};
        {
            std::unique_lock lock(mtx_);
            Chunk* chunk = free_;
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

        Key* newKeys = nullptr;
        if (other.capacity_) {
            const size_t metaCap = (((capacity_ * sizeof(int8_t)) + GROUP_WIDTH - 1) + sizeof(Key) - 1) / sizeof(Key);

            auto* ptr = slab_allocator_traits::allocate(alloc_, capacity_ + metaCap);
            std::memcpy(ptr, other.metas_, (capacity_ + metaCap) * sizeof(Key));

            metas_ = reinterpret_cast<int8_t*>(ptr);
            keys_ = ptr + metaCap;
        }

        release();

        keys_ = newKeys;
        size_ = other.size_;
        capacity_ = other.capacity_;

        return *this;
    }

    hash_set& operator=(hash_set&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        release();

        alloc_ = std::move(other.alloc_);
        metas_ = other.metas_;
        keys_ = other.keys_;
        size_ = other.size_;
        capacity_ = other.capacity_;

        other.reset();

        return *this;
    }

    hash_set(Alloc alloc = std::allocator<Key>()) : alloc_(alloc) {}

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
                if (metas_[pos] > 0) {
                    const auto key = keys_[pos];
                    auto hash = std::hash<Key>{}(key);
                    const int8_t h2 = hash & 0x7F;
                    size_t newPos = (hash >> 7) % newCapacity;

                    bool inserted = false;
                    for (size_t probed = 0; probed < capacity_; ++probed) {
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

    bool insert(const Key& key) {
        if (capacity_ == 0) {
            return false;
        }
        if (capacity_ == size_) {
            return false;
        }

        const size_t hash = std::hash<Key>{}(key);
        const int8_t h2 = hash & 0x7F;        // low 7 bits, high bit clear
        size_t pos = (hash >> 7) % capacity_; // H1: start slot

        for (size_t probed = 0; probed < capacity_; ++probed) {
            const int8_t m = metas_[pos];
            if (m == h2) {
                if (keys_[pos] == key) {
                    return true;
                }
            }
            if (m == EMPTY || m == DELETED) { // empty -> available to use
                metas_[pos] = h2;
                keys_[pos] = key;
                size_++;
                return true;
            }
            // Deleted tombstone, or full H2 -> keep probing
            pos = (pos + 1) % capacity_;
        }

        return false;
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
            while (pos < set->capacity_ && set->metas_[pos] <= 0) {
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
            } while (pos < set->capacity_ && set->metas_[pos] <= 0);

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
            while (pos > 0 && set->metas_[pos] <= 0) {
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
    Slab* ptr_;
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
            std::memcpy(ptr_, other.metas_, (metaCap + keyCap + elemCap) * sizeof(Slab));

            metas_ = reinterpret_cast<int8_t*>(ptr_);
            keys_ = reinterpret_cast<Key*>(ptr_ + metaCap);
            elems_ = reinterpret_cast<Elem*>(ptr_ + metaCap + keyCap);
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

        Slab* newPtr{};
        int8_t* newMetas{};
        Key* newKeys{};
        Elem* newElems{};

        if (other.capacity_) {
            const size_t metaCap = (((capacity_ * sizeof(int8_t)) + GROUP_WIDTH - 1) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t keyCap = ((capacity_ * sizeof(Key)) + sizeof(Slab) - 1) / sizeof(Slab);
            const size_t elemCap = ((capacity_ * sizeof(Elem)) + sizeof(Slab) - 1) / sizeof(Slab);
            newPtr = slab_atr::allocate(sa_, metaCap + keyCap + elemCap);

            std::memcpy(newPtr, other.ptr_, (metaCap + keyCap + elemCap) * sizeof(Slab));

            newMetas = reinterpret_cast<int8_t*>(newPtr);
            newKeys = reinterpret_cast<Key*>(newPtr + metaCap);
            newElems = reinterpret_cast<Elem*>(newPtr + metaCap + keyCap);
        }

        release();

        ptr_ = newPtr;
        metas_ = newMetas;
        keys_ = newKeys;
        elems_ = newElems;
        size_ = other.size_;
        capacity_ = other.capacity_;

        return *this;
    }

    hash_map& operator=(hash_map&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        release();

        sa_ = std::move(other.sa_);
        ea_ = std::move(other.ea_);
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
    [[nodiscard]] size_t empty() const { return size_ == 0; }

    void clear() {
        if (size_) {
            size_t pos = 0;
            while (pos < capacity_) {
                if (metas_[pos] > 0) {
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

        std::memset(newMetas, EMPTY, newCapacity + 1);
        // newMetas[newCapacity] = SENTINEL;

        size_t newSize{};
        if (size_ > 0) {
            // TODO: use iterators instead
            for (size_t pos = 0; pos < capacity_; pos++) {
                if (metas_[pos] > 0) {
                    const auto hash = std::hash<Key>{}(keys_[pos]);
                    const int8_t h2 = H2(hash);
                    size_t newPos = H1(hash) % newCapacity;

                    for (size_t probed = 0; probed < capacity_; ++probed) {
                        const int8_t m = newMetas[newPos];
                        if (m == EMPTY || m == DELETED) {
                            newMetas[newPos] = h2;
                            newKeys[newPos] = keys_[pos];
                            newElems[newPos] = elems_[pos]; // may be a copy, bad !!
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
        const int8_t h2 = H2(hash);
        size_t pos = H1(hash) % capacity_;

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

    bool insert(const Key& key, const Elem& elem) {
        if (capacity_ == 0 || capacity_ == size_) {
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
            }
            if (m == EMPTY || m == DELETED) {
                metas_[pos] = h2;
                keys_[pos] = key;
                elems_[pos] = elem;
                size_++;
                return true;
            }
            pos = (pos + 1) % capacity_;
        }

        return false;
    }

    bool insert(const Key& key, Elem&& elem) {
        if (capacity_ == 0 || capacity_ == size_) {
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
            }
            if (m == EMPTY || m == DELETED) {
                metas_[pos] = h2;
                keys_[pos] = key;
                elems_[pos] = elem;
                size_++;
                return true;
            }
            pos = (pos + 1) % capacity_;
        }

        return false;
    }

    template<typename... Args>
    Elem& emplace(const Key& key, Args&&... args) {
        const size_t hash = std::hash<Key>{}(key);
        const int8_t h2 = hash & 0x7f;
        size_t pos = (hash >> 7) % capacity_;

        for (size_t probed = 0; probed < capacity_; ++probed) {
            const int8_t m = metas_[pos];
            if (m == h2) {
                if (keys_[pos] == key) {
                    return elems_[pos];
                }
            }
            if (m == EMPTY || m == DELETED) {
                metas_[pos] = h2;
                keys_[pos] = key;
                elem_atr::construct(ea_, elems_ + pos, std::forward<Args>(args)...);
                size_++;
                return elems_[pos];
            }
            pos = (pos + 1) % capacity_;
        }

        return elems_[pos];
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

    Elem& at(const Key& key) {
        if (size_ == 0) {
            throw std::runtime_error("");
        }

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
                throw std::runtime_error("");
            }
            pos = (pos + 1) % capacity_;
        }

        throw std::runtime_error("");
    }

    [[nodiscard]] Elem& at(const Key& key) const {
        if (size_ == 0) {
            throw std::runtime_error("");
        }

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
                throw std::runtime_error("");
            }
            pos = (pos + 1) % capacity_;
        }

        throw std::runtime_error("");
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
            while (pos < map->capacity_ && map->metas_[pos] <= 0) {
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
            } while (pos < ptr->capacity_ && ptr->metas_[pos] <= 0);

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
            while (pos > 0 && ptr->metas_[pos] <= 0) {
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
    Slab* ptr_;
    Elem* elems_;
    bool* slots_;
    size_t capacity_{};
    // size_t nextSlot{};
    size_t size_{};

    slot_map(const slot_map&) = delete;
    slot_map& operator=(const slot_map&) = delete;
    slot_map(slot_map&&) = delete;
    slot_map& operator=(slot_map&&) = delete;

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

    [[nodiscard]] bool empty() const { return size_ == 0; }

    [[nodiscard]] size_t size() const { return size_; }

    [[nodiscard]] size_t capacity() const { return capacity_; }

    bool reserve(size_t newCapacity) {
        if (size_) {
            return false;
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
        if (slot < 0 || slot >= capacity_) {
            return false;
        }
        return slots_[slot];
    }

    template<typename... Args>
    Slot emplace(Args&&... args) {
        if (!capacity_) {
            return -1;
        }
        size_t slot = 0;
        while (slot < capacity_ && slots_[slot]) {
            slot++;
        }
        elem_atr::construct(ea_, elems_ + slot, std::forward<Args>(args)...);
        slots_[slot] = true;
        size_++;

        return slot;
    }

    Slot push(const Elem& elem) {
        if (!capacity_) {
            return -1;
        }
        size_t slot = 0;
        while (slot < capacity_ && slots_[slot]) {
            slot++;
        }
        elems_[slot] = elem;
        slots_[slot] = true;
        size_++;

        return slot;
    }

    Slot push(Elem&& elem) {
        if (!capacity_) {
            return -1;
        }

        size_t slot = 0;
        while (slot < capacity_ && slots_[slot]) {
            slot++;
        }

        elems_[slot] = std::move(elem);
        slots_[slot] = true;
        size_++;

        return slot;
    }

    template<typename... Args>
    bool emplace_at(Slot slot, Args&&... args) {
        if (slot >= capacity_ || slots_[slot]) {
            return false;
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

        elems_[slot] = elem;
        slots_[slot] = true;
        size_++;
        return true;
    }

    bool insert(Slot slot, Elem&& elem) {
        if (slot >= capacity_ || slots_[slot]) {
            return false;
        }

        elems_[slot] = std::move(elem);
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

    Elem& at(Slot slot) {
        if (slot >= capacity_ || !slots_[slot]) {
            throw std::runtime_error("");
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
