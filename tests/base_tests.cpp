#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../deps/doctest.h"
#include "../src/base.hpp"

#include <vector>

struct Foo {
    float data[128];
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

    {
        auto* foo1 = fooPool.Allocate();
        foo1->data[127] = 0.1f;

        auto* foo2 = fooPool.Allocate();
        foo2->data[127] = 0.1f;

        fooPool.Deallocate(foo1);
        fooPool.Deallocate(foo2);
    }

    {
        auto* foo1 = fooPool.Allocate();
        foo1->data[127] = 0.1f;

        auto* foo2 = fooPool.Allocate();
        foo2->data[127] = 0.1f;

        fooPool.Deallocate(foo1);
        fooPool.Deallocate(foo2);
    }
}
