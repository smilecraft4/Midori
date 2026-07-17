#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../deps/doctest.h"
#include "../src/base.hpp"
#include <vector>
#include <random>

struct Foo {
    float data[32];
    bool operator==(const Foo& other) const {
        for (size_t i = 0; i < std::size(data); i++) {
            if (data[i] != other.data[i]) {
                return false;
            }
        }
        return true;
    }

    template<class Rng>
    Foo(Rng& rng) {
        std::uniform_real_distribution<float> elemDist(-1.0f, 1.0f);
        for (float& f : data) {
            f = elemDist(rng);
        }
    }

    ~Foo() = default;
};

struct Bar {
    int a;
    int b;
    bool operator==(const Bar& other) const { return a == other.a && b == other.b; }
};

template<>
struct std::hash<Bar> {
    size_t operator()(const Bar& b) const {
        uint64_t x = (uint64_t(uint32_t(b.a)) << 32) | uint32_t(b.b);
        return Splitmix64(x);
    }
};

TEST_CASE("Arena Allocator") {
    arena arena(4_KiB);
    CHECK(arena.capacity() == 4_KiB);

    auto* buf1 = static_cast<int*>(arena.allocate(sizeof(int) * 512));
    buf1[511] = 1;
    CHECK(arena.size() == sizeof(int) * 512);

    auto* buf2 = static_cast<int*>(arena.allocate(sizeof(int) * 512));
    buf2[511] = 1;
    CHECK(arena.size() == sizeof(int) * 1024);

    arena.reset();
    CHECK(arena.size() == 0);
    CHECK(arena.capacity() == 4_KiB);
}

TEST_CASE("Arena Copy/Move") {
    arena a(4_KiB);
    auto* buf = static_cast<int*>(a.allocate(sizeof(int) * 4));
    buf[0] = 42;
    buf[3] = 7;
    CHECK(a.size() == sizeof(int) * 4);

    SUBCASE("copy constructor allocates an independent buffer") {
        arena b(a);
        CHECK(b.capacity() == a.capacity());
        CHECK(b.size() == a.size());
        CHECK(b.data_ != a.data_);
        CHECK(reinterpret_cast<int*>(b.data_)[0] == 42);
        CHECK(reinterpret_cast<int*>(b.data_)[3] == 7);
        // The original is untouched.
        CHECK(reinterpret_cast<int*>(a.data_)[0] == 42);
    }

    SUBCASE("move constructor steals the buffer") {
        std::byte* data = a.data_;
        arena c(std::move(a));
        CHECK(c.data_ == data);
        CHECK(c.size() == sizeof(int) * 4);
        CHECK(c.capacity() == 4_KiB);
        CHECK(a.data_ == nullptr);
        CHECK(a.capacity() == 0);
        CHECK(a.size() == 0);
    }

    SUBCASE("copy assignment") {
        arena d(1_KiB);
        d = a;
        CHECK(d.capacity() == a.capacity());
        CHECK(d.size() == a.size());
        CHECK(d.data_ != a.data_);
        CHECK(reinterpret_cast<int*>(d.data_)[3] == 7);
    }

    SUBCASE("move assignment") {
        arena e(1_KiB);
        std::byte* data = a.data_;
        e = std::move(a);
        CHECK(e.data_ == data);
        CHECK(e.size() == sizeof(int) * 4);
        CHECK(e.capacity() == 4_KiB);
        CHECK(a.capacity() == 0);
        CHECK(a.size() == 0);
    }
}

TEST_CASE("Arena vector") {
    arena arena(10_KiB);

    std::vector<Foo, arena_allocator<Foo>> foos(arena);
    foos.reserve(4);
}

TEST_CASE("Arena Arena Allocator") {
    arena arena1(10_KiB);

    {
        arena<arena_allocator<std::byte>> arena2(5_KiB, arena1);
        CHECK(arena1.size() == 5_KiB);
    }

    {
        arena<arena_allocator<std::byte>> arena3(2_KiB, arena1);
        CHECK(arena1.size() == 7_KiB);
    }
}

TEST_CASE("Pool Allocator") {
    arena arena(1_MiB);
    pool<Foo, arena_allocator<Foo>> fooPool(2, arena);

    std::random_device rd;
    std::mt19937 mt(rd());

    {
        auto* foo1 = fooPool.allocate(1, mt);
        foo1->data[31] = 0.1f;

        auto* foo2 = fooPool.allocate(1, mt);
        foo2->data[31] = 0.1f;

        CHECK(foo1 != nullptr);
        CHECK(foo2 != nullptr);
        CHECK(foo1 != foo2);

        fooPool.deallocate(foo1, 1);
        fooPool.deallocate(foo2, 1);
    }

    {
        auto* foo1 = fooPool.allocate(1, mt);
        foo1->data[31] = 0.1f;

        auto* foo2 = fooPool.allocate(1, mt);
        foo2->data[31] = 0.1f;

        fooPool.deallocate(foo1, 1);
        fooPool.deallocate(foo2, 1);
    }
}

TEST_CASE("Pool Move") {
    arena arena(1_MiB);

    std::random_device rd;
    std::mt19937 mt(rd());

    pool<Foo, arena_allocator<Foo>> pool1(4, arena);

    auto* foo1 = pool1.allocate(1, mt);
    foo1->data[0] = 1.0f;

    // move constructor: pool1's free list is transferred to pool2
    pool<Foo, arena_allocator<Foo>> pool2(std::move(pool1));

    auto* foo2 = pool2.allocate(1, mt);
    foo2->data[0] = 2.0f;
    CHECK(foo2 != nullptr);
    CHECK(foo1 != foo2);

    pool2.deallocate(foo1, 1);
    pool2.deallocate(foo2, 1);

    // move assignment
    pool<Foo, arena_allocator<Foo>> pool3(2, arena);
    pool3 = std::move(pool2);

    auto* foo3 = pool3.allocate(1, mt);
    foo3->data[0] = 3.0f;
    CHECK(foo3 != nullptr);
    pool3.deallocate(foo3, 1);
}

TEST_CASE("HashSet") {
    size_t counter{};
    arena arena(1_MiB);

    Bar bar1{.a = 1, .b = 2};
    Bar bar2{.a = 2, .b = 2};
    Bar bar3{.a = 2, .b = 3};

    hash_set<Bar, arena_allocator<Bar>> set(16, arena);
    CHECK(set.size() == 0);
    CHECK(set.capacity() == 16);

    CHECK(set.reserve(128) == true);
    CHECK(set.capacity() == 128);
    CHECK(set.size() == 0);

    CHECK(set.contains(bar1) == false);
    CHECK(set.insert(bar1) == true);
    CHECK(set.contains(bar1) == true);
    CHECK(set.erase(bar1) == true);
    CHECK(set.erase(bar1) == false);
    CHECK(set.erase(bar1) == false);

    CHECK(set.insert(bar1) == true);
    CHECK(set.insert(bar2) == true);
    CHECK(set.insert(bar3) == true);
    CHECK(set.contains(bar1) == true);
    CHECK(set.contains(bar2) == true);
    CHECK(set.contains(bar3) == true);

    CHECK(set.reserve(2) == false);

    CHECK(set.reserve(3) == true);
    CHECK(set.contains(bar1) == true);
    CHECK(set.contains(bar2) == true);
    CHECK(set.contains(bar3) == true);

    set.clear();
    CHECK(set.reserve(256) == true);
    CHECK(set.insert({5, 9}) == true);
    CHECK(set.insert({10, -9}) == true);
    CHECK(set.insert({10, -3}) == true);
    CHECK(set.insert({10, -1}) == true);
    CHECK(set.size() == 4);

    counter = 0;
    for (const auto& key : set) {
        CHECK(set.contains(key));
        counter++;
    }
    CHECK(counter == set.size());

    counter = 0;
    for (const auto& key : set | std::views::reverse) {
        CHECK(set.contains(key));
        counter++;
    }
    CHECK(counter == set.size());

    SUBCASE("copy constructor") {
        hash_set<Bar, arena_allocator<Bar>> copy(set);
        CHECK(copy.size() == set.size());
        for (const auto& key : set) {
            CHECK(copy.contains(key));
        }
        for (const auto& key : copy) {
            CHECK(set.contains(key));
        }
        // Independent: mutating the copy leaves the source alone.
        CHECK(copy.erase({5, 9}) == true);
        CHECK(copy.contains({5, 9}) == false);
        CHECK(set.contains({5, 9}) == true);
    }

    SUBCASE("copy assignment") {
        hash_set<Bar, arena_allocator<Bar>> copy(8, arena);
        CHECK(copy.insert({99, 99}) == true);
        copy = set;
        CHECK(copy.size() == set.size());
        CHECK(copy.contains({99, 99}) == false); // old contents are dropped
        for (const auto& key : set) {
            CHECK(copy.contains(key));
        }
    }

    SUBCASE("move constructor") {
        const size_t size = set.size();
        hash_set<Bar, arena_allocator<Bar>> moved(std::move(set));
        CHECK(moved.size() == size);
        CHECK(set.size() == 0);
        CHECK(set.capacity() == 0);
        CHECK(moved.contains({5, 9}));
        CHECK(moved.contains({10, -1}));
    }

    SUBCASE("move assignment") {
        const size_t size = set.size();
        hash_set<Bar, arena_allocator<Bar>> moved(8, arena);
        CHECK(moved.insert({77, 77}) == true);
        moved = std::move(set);
        CHECK(moved.size() == size);
        CHECK(set.size() == 0);
        CHECK(moved.contains({77, 77}) == false);
        CHECK(moved.contains({5, 9}));
    }
}

TEST_CASE("HashSet tombstone reuse") {
    arena arena(64_KiB);
    hash_set<Bar, arena_allocator<Bar>> set(128, arena);

    for (int i = 0; i < 30; i++) {
        CHECK(set.insert({i, 0}) == true);
    }
    CHECK(set.size() == 30);

    for (int i = 0; i < 30; i++) {
        CHECK(set.erase({i, 0}) == true);
    }
    CHECK(set.size() == 0);

    // Re-inserting the same keys must reuse tombstones without creating duplicates.
    for (int i = 0; i < 30; i++) {
        CHECK(set.insert({i, 0}) == true);
    }
    CHECK(set.size() == 30);

    // Inserting an existing key is a no-op that still reports success.
    CHECK(set.insert({0, 0}) == true);
    CHECK(set.size() == 30);

    size_t counter = 0;
    for (const auto& key : set) {
        CHECK(set.contains(key));
        counter++;
    }
    CHECK(counter == 30);
}

TEST_CASE("HashMap") {
    size_t counter{};

    arena arena(1_MiB);
    std::random_device rd;
    std::mt19937 mt(rd());

    Bar bar1{.a = 1, .b = 2};
    Bar bar2{.a = 2, .b = 2};
    Bar bar3{.a = 2, .b = 3};

    Foo foo1{mt};
    Foo foo2{mt};
    Foo foo3{mt};

    hash_map<Bar, Foo, arena_allocator<Bar>> map(0, arena);
    CHECK(map.size() == 0);
    CHECK(map.capacity() == 0);
    CHECK(map.contains(bar1) == false);
    CHECK(map.contains(bar2) == false);
    CHECK(map.contains(bar3) == false);

    map.reserve(128);
    CHECK(map.size() == 0);
    CHECK(map.capacity() == 128);

    map.insert(bar1, foo1);
    CHECK(map.size() == 1);
    CHECK(map.contains(bar1) == true);

    map.emplace(bar2, mt);
    CHECK(map.size() == 2);
    CHECK(map.contains(bar2) == true);

    // emplace of an existing key returns the existing element without inserting.
    Foo& existing = map.emplace(bar1, mt);
    CHECK(map.size() == 2);
    CHECK(existing == foo1);

    CHECK(map.reserve(1) == false);
    CHECK(map.capacity() == 128);

    CHECK(map.reserve(2) == true);
    CHECK(map.contains(bar1) == true);
    CHECK(map.contains(bar2) == true);

    Foo& foo = map.at(bar1);
    CHECK(map.size() == 2);
    CHECK(map.contains(bar1) == true);
    CHECK(foo == foo1);

    map.erase(bar1);
    CHECK(map.size() == 1);
    CHECK(map.contains(bar1) == false);

    map.clear();
    CHECK(map.size() == 0);
    CHECK(map.contains(bar2) == false);

    CHECK(map.reserve(5) == true);
    CHECK(map.insert(bar1, foo1) == true);
    CHECK(map.insert(bar2, foo2) == true);

    counter = 0;
    for (auto [key, elem] : map) {
        CHECK(map.contains(key));
        elem = foo3;
        CHECK(map.at(key) == foo3);
        counter++;
    }
    CHECK(counter == map.size());

    counter = 0;
    for (const auto [key, elem] : map | std::views::reverse) {
        CHECK(map.contains(key));
        counter++;
    }
    CHECK(counter == map.size());

    // At this point map holds { bar1 -> foo3, bar2 -> foo3 }.
    SUBCASE("copy constructor") {
        hash_map<Bar, Foo, arena_allocator<Bar>> copy(map);
        CHECK(copy.size() == map.size());
        for (auto [key, elem] : map) {
            CHECK(copy.contains(key));
            CHECK(copy.at(key) == elem);
        }
        // Independent element storage.
        copy.at(bar1) = foo1;
        CHECK(copy.at(bar1) == foo1);
        CHECK(map.at(bar1) == foo3);
    }

    SUBCASE("copy assignment") {
        hash_map<Bar, Foo, arena_allocator<Bar>> copy(4, arena);
        CHECK(copy.insert(bar3, foo1) == true);
        copy = map;
        CHECK(copy.size() == map.size());
        CHECK(copy.contains(bar3) == false);
        for (auto [key, elem] : map) {
            CHECK(copy.at(key) == elem);
        }
    }

    SUBCASE("move constructor") {
        const size_t size = map.size();
        hash_map<Bar, Foo, arena_allocator<Bar>> moved(std::move(map));
        CHECK(moved.size() == size);
        CHECK(map.size() == 0);
        CHECK(map.capacity() == 0);
        CHECK(moved.contains(bar1));
        CHECK(moved.contains(bar2));
        CHECK(moved.at(bar1) == foo3);
    }

    SUBCASE("move assignment") {
        const size_t size = map.size();
        hash_map<Bar, Foo, arena_allocator<Bar>> moved(4, arena);
        CHECK(moved.insert(bar3, foo2) == true);
        moved = std::move(map);
        CHECK(moved.size() == size);
        CHECK(map.size() == 0);
        CHECK(moved.contains(bar3) == false);
        CHECK(moved.contains(bar1));
        CHECK(moved.contains(bar2));
    }
}

TEST_CASE("HashMap tombstone reuse") {
    arena arena(1_MiB);
    std::random_device rd;
    std::mt19937 mt(rd());

    hash_map<Bar, Foo, arena_allocator<Bar>> map(128, arena);

    for (int i = 0; i < 30; i++) {
        CHECK(map.insert({i, 0}, Foo{mt}) == true);
    }
    CHECK(map.size() == 30);

    for (int i = 0; i < 30; i++) {
        CHECK(map.erase({i, 0}) == true);
    }
    CHECK(map.size() == 0);

    // Re-inserting the same keys must reuse tombstones without creating duplicates.
    for (int i = 0; i < 30; i++) {
        CHECK(map.insert({i, 0}, Foo{mt}) == true);
    }
    CHECK(map.size() == 30);

    size_t counter = 0;
    for (auto [key, elem] : map) {
        CHECK(map.contains(key));
        counter++;
    }
    CHECK(counter == 30);
}

TEST_CASE("SlotMap") {
    arena arena(64_KiB);

    std::random_device rd;
    std::mt19937 mt(rd());

    Foo foo1{mt};
    Foo foo2{mt};
    Foo foo3{mt};
    Foo foo4{mt};

    slot_map<size_t, Foo, arena_allocator<Foo>> foos(0, arena);
    CHECK(foos.size() == 0);
    CHECK(foos.capacity() == 0);

    foos.reserve(8);
    CHECK(foos.size() == 0);
    CHECK(foos.capacity() == 8);

    auto slot1 = foos.push(foo1);
    CHECK(slot1 == 0);
    CHECK(foos.size() == 1);
    CHECK(foos.contains(slot1));

    auto slot4 = foos.emplace(mt);
    CHECK(slot4 == 1);
    CHECK(foos.size() == 2);
    CHECK(foos.contains(slot4));

    foos.erase(slot4);
    CHECK(foos.size() == 1);
    CHECK(!foos.contains(slot4));

    foos.clear();
    CHECK(foos.size() == 0);
    CHECK(foos.capacity() == 8);

    CHECK(foos.insert(2, foo2));
    CHECK(foos.size() == 1);
    CHECK(foos.contains(2));

    CHECK(foos.emplace_at(4, mt));
    CHECK(foos.size() == 2);
    CHECK(foos.contains(4));

    size_t counter = 0;
    for (auto [slot, elem] : foos) {
        elem = foo4;
        CHECK(foos.at(slot) == foo4);
        counter++;
    }
    CHECK(counter == foos.size());

    // foos now holds live elements at slots 2 and 4 (both == foo4).
    SUBCASE("copy constructor") {
        slot_map<size_t, Foo, arena_allocator<Foo>> copy(foos);
        CHECK(copy.size() == foos.size());
        CHECK(copy.contains(2));
        CHECK(copy.contains(4));
        CHECK(copy.at(2) == foos.at(2));
        // Independent storage.
        copy.at(2) = foo1;
        CHECK(copy.at(2) == foo1);
        CHECK(foos.at(2) == foo4);
    }

    SUBCASE("copy assignment") {
        slot_map<size_t, Foo, arena_allocator<Foo>> copy(4, arena);
        CHECK(copy.insert(0, foo1));
        copy = foos;
        CHECK(copy.size() == foos.size());
        CHECK(copy.contains(0) == false);
        CHECK(copy.contains(2));
        CHECK(copy.contains(4));
    }

    SUBCASE("move constructor") {
        const size_t size = foos.size();
        slot_map<size_t, Foo, arena_allocator<Foo>> moved(std::move(foos));
        CHECK(moved.size() == size);
        CHECK(foos.size() == 0);
        CHECK(foos.capacity() == 0);
        CHECK(moved.contains(2));
        CHECK(moved.contains(4));
    }

    SUBCASE("move assignment") {
        const size_t size = foos.size();
        slot_map<size_t, Foo, arena_allocator<Foo>> moved(4, arena);
        CHECK(moved.insert(0, foo1));
        moved = std::move(foos);
        CHECK(moved.size() == size);
        CHECK(foos.size() == 0);
        CHECK(moved.contains(0) == false);
        CHECK(moved.contains(2));
        CHECK(moved.contains(4));
    }
}

TEST_CASE("SlotMap full") {
    arena arena(32_KiB);

    std::random_device rd;
    std::mt19937 mt(rd());

    Foo foo{mt};

    slot_map<size_t, Foo, arena_allocator<Foo>> foos(2, arena);
    CHECK(foos.emplace(mt) == 0);
    CHECK(foos.emplace(mt) == 1);
    CHECK(foos.size() == 2);

    // Once full, allocating variants must report the sentinel instead of writing
    // out of bounds.
    CHECK(foos.free_slot() == static_cast<size_t>(-1));
    CHECK(foos.emplace(mt) == static_cast<size_t>(-1));
    CHECK(foos.push(foo) == static_cast<size_t>(-1));
    CHECK(foos.size() == 2);

    // Freeing a slot makes room again.
    CHECK(foos.erase(0) == true);
    CHECK(foos.free_slot() == 0);
    CHECK(foos.push(foo) == 0);
    CHECK(foos.size() == 2);
}
