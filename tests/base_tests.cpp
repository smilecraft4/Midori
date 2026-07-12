#include <random>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../deps/doctest.h"
#include "../src/base.hpp"
#include <vector>

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
    Arena arena(4_KiB);
    CHECK(arena.Capacity() == 4_KiB);

    auto* buf1 = static_cast<int*>(arena.Allocate(sizeof(int) * 512));
    buf1[511] = 1;
    CHECK(arena.Size() == sizeof(int) * 512);

    auto* buf2 = static_cast<int*>(arena.Allocate(sizeof(int) * 512));
    buf2[511] = 1;
    CHECK(arena.Size() == sizeof(int) * 1024);

    arena.Reset();
    CHECK(arena.Size() == 0);
    CHECK(arena.Capacity() == 4_KiB);
}

TEST_CASE("Arena vector") {
    Arena arena(10_KiB);

    std::vector<Foo, arena_allocator<Foo>> foos(arena);
    foos.reserve(4);
}

TEST_CASE("Arena Arena Allocator") {
    Arena arena1(10_KiB);

    {
        Arena<arena_allocator<std::byte>> arena2(5_KiB, arena1);
        CHECK(arena1.Size() == 5_KiB);
    }

    {
        Arena<arena_allocator<std::byte>> arena3(2_KiB, arena1);
        CHECK(arena1.Size() == 7_KiB);
    }
}

TEST_CASE("Pool Allocator") {
    Arena arena(1_MiB);
    Pool<Foo, arena_allocator<Foo>> fooPool(2, arena);

    std::random_device rd;
    std::mt19937 mt(rd());

    {
        auto* foo1 = fooPool.Allocate(mt);
        foo1->data[127] = 0.1f;

        auto* foo2 = fooPool.Allocate(mt);
        foo2->data[127] = 0.1f;

        fooPool.Deallocate(foo1);
        fooPool.Deallocate(foo2);
    }

    {
        auto* foo1 = fooPool.Allocate(mt);
        foo1->data[127] = 0.1f;

        auto* foo2 = fooPool.Allocate(mt);
        foo2->data[127] = 0.1f;

        fooPool.Deallocate(foo1);
        fooPool.Deallocate(foo2);
    }
}

TEST_CASE("HashSet") {
    Arena arena(1_MiB);

    Bar bar1{.a = 1, .b = 2};
    Bar bar2{.a = 2, .b = 2};
    Bar bar3{.a = 2, .b = 3};

    HashSet<Bar, arena_allocator<Bar>> set(16, arena);
    CHECK(set.Size() == 0);
    CHECK(set.Capacity() == 16);

    CHECK(set.Reserve(128) == true);
    CHECK(set.Capacity() == 128);
    CHECK(set.Size() == 0);

    CHECK(set.Contains(bar1) == false);
    CHECK(set.Insert(bar1) == true);
    CHECK(set.Contains(bar1) == true);
    CHECK(set.Erase(bar1) == true);
    CHECK(set.Erase(bar1) == false);
    CHECK(set.Erase(bar1) == false);

    CHECK(set.Insert(bar1) == true);
    CHECK(set.Insert(bar2) == true);
    CHECK(set.Insert(bar3) == true);
    CHECK(set.Contains(bar1) == true);
    CHECK(set.Contains(bar2) == true);
    CHECK(set.Contains(bar3) == true);

    CHECK(set.Reserve(2) == false);

    CHECK(set.Reserve(3) == true);
    CHECK(set.Contains(bar1) == true);
    CHECK(set.Contains(bar2) == true);
    CHECK(set.Contains(bar3) == true);

    CHECK(set.Reserve(256) == true);
    CHECK(set.Contains(bar1) == true);
    CHECK(set.Contains(bar2) == true);
    CHECK(set.Contains(bar3) == true);

    // for (auto& key : map) {}
    // for (const auto& key : map) {}

    // move constructor
    // copy constructor
    // move assignment
    // copy assignment
}

TEST_CASE("HashMap") {
    Arena arena(1_MiB);
    std::random_device rd;
    std::mt19937 mt(rd());

    Bar bar1{.a = 1, .b = 2};
    Bar bar2{.a = 2, .b = 2};
    Bar bar3{.a = 2, .b = 3};

    Foo foo1{mt};
    Foo foo3{mt};

    HashMap<Bar, Foo, arena_allocator<Bar>> map(0, arena);
    CHECK(map.Size() == 0);
    CHECK(map.Capacity() == 0);
    CHECK(map.Contains(bar1) == false);
    CHECK(map.Contains(bar2) == false);
    CHECK(map.Contains(bar3) == false);

    map.Reserve(128);
    CHECK(map.Size() == 0);
    CHECK(map.Capacity() == 128);

    map.Insert(bar1, foo1);
    CHECK(map.Size() == 1);
    CHECK(map.Contains(bar1) == true);

    map.Emplace(bar2, mt);
    CHECK(map.Size() == 2);
    CHECK(map.Contains(bar2) == true);

    CHECK(map.Reserve(1) == false);
    CHECK(map.Capacity() == 128);

    CHECK(map.Reserve(2) == true);
    CHECK(map.Contains(bar1) == true);
    CHECK(map.Contains(bar2) == true);

    Foo& foo = map.At(bar1);
    CHECK(map.Size() == 2);
    CHECK(map.Contains(bar1) == true);
    CHECK(foo == foo1);

    map.Erase(bar1);
    CHECK(map.Size() == 1);
    CHECK(map.Contains(bar1) == false);

    map.Erase(bar2);
    CHECK(map.Size() == 0);
    CHECK(map.Contains(bar2) == false);

    // for (auto& key : map) {}
    // for (const auto& key : map) {}
}
