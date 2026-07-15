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
        auto* foo1 = fooPool.Allocate(1, mt);
        foo1->data[127] = 0.1f;

        auto* foo2 = fooPool.Allocate(1, mt);
        foo2->data[127] = 0.1f;

        fooPool.deallocate(foo1, 1);
        fooPool.deallocate(foo2, 1);
    }

    {
        auto* foo1 = fooPool.Allocate(1, mt);
        foo1->data[127] = 0.1f;

        auto* foo2 = fooPool.Allocate(1, mt);
        foo2->data[127] = 0.1f;

        fooPool.deallocate(foo1, 1);
        fooPool.deallocate(foo2, 1);
    }
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

    // move constructor
    // copy constructor
    // move assignment
    // copy assignment
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
}

TEST_CASE("SlotMap") {
    arena arena(32_KiB);

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
}
