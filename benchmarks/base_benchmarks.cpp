#define ANKERL_NANOBENCH_IMPLEMENT
#include "../deps/nanobench.h"
#include "../src/base.hpp"
#include <random>
#include <cstdint>
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

int main() {
    constexpr size_t N = 2048;

    // ---------------------------------------------------------------- hash_set
    {
        arena arena(1_MiB);

        std::random_device rd;
        std::mt19937 mt(rd());
        std::uniform_int_distribution<int> keyDist(-16, 16);
        std::uniform_int_distribution<int> opsDist(0, 2);

        std::vector<std::tuple<uint8_t, Bar>> ops(N);
        for (auto& [type, key] : ops) {
            type = static_cast<uint8_t>(opsDist(mt));
            key.a = keyDist(mt);
            key.b = keyDist(mt);
        }

        hash_set<Bar, arena_allocator<Bar>> set(ops.size(), arena);

        ankerl::nanobench::Bench().batch(ops.size()).run("HashSet Insert", [&] {
            for (const auto& [type, key] : ops) {
                ankerl::nanobench::doNotOptimizeAway(set.insert(key));
            }
        });

        ankerl::nanobench::Bench().batch(ops.size()).run("HashSet Contains", [&] {
            for (const auto& [type, key] : ops) {
                ankerl::nanobench::doNotOptimizeAway(set.contains(key));
            }
        });

        ankerl::nanobench::Bench().batch(set.size()).run("HashSet Iterator", [&] {
            for (const auto& key : set) {
                ankerl::nanobench::doNotOptimizeAway(key);
            }
        });

        ankerl::nanobench::Bench().batch(ops.size()).run("HashSet Erase", [&] {
            for (const auto& [type, key] : ops) {
                ankerl::nanobench::doNotOptimizeAway(set.erase(key));
            }
        });

        ankerl::nanobench::Bench().batch(ops.size()).run("HashSet Mixed", [&] {
            for (const auto& [op, key] : ops) {
                switch (op) {
                    case 0:
                        ankerl::nanobench::doNotOptimizeAway(set.insert(key));
                        break;
                    case 1:
                        ankerl::nanobench::doNotOptimizeAway(set.contains(key));
                        break;
                    case 2:
                        ankerl::nanobench::doNotOptimizeAway(set.erase(key));
                        break;
                    default:
                        break;
                }
            }
        });
    }

    // ---------------------------------------------------------------- hash_map
    {
        arena arena(1_MiB);

        std::random_device rd;
        std::mt19937 mt(rd());
        std::uniform_int_distribution<int> keyDist(-16, 16);
        std::uniform_int_distribution<int> opsDist(0, 3);

        std::vector<std::tuple<uint8_t, Bar, Foo>> ops;
        ops.reserve(N);
        for (size_t i = 0; i < ops.capacity(); i++) {
            ops.emplace_back(static_cast<uint8_t>(opsDist(mt)), Bar{.a = keyDist(mt), .b = keyDist(mt)}, Foo{mt});
        }

        std::vector<Bar> em;
        em.reserve(N);
        for (size_t i = 0; i < em.capacity(); i++) {
            em.emplace_back(Bar{.a = keyDist(mt), .b = keyDist(mt)});
        }

        hash_map<Bar, Foo, arena_allocator<Bar>> map(ops.size(), arena);

        ankerl::nanobench::Bench().batch(ops.size()).run("HashMap Insert", [&] {
            for (const auto& [op, key, elem] : ops) {
                ankerl::nanobench::doNotOptimizeAway(map.insert(key, elem));
            }
        });

        ankerl::nanobench::Bench().batch(em.size()).run("HashMap Emplace", [&] {
            for (const auto& key : em) {
                ankerl::nanobench::doNotOptimizeAway(map.emplace(key, mt));
            }
        });

        ankerl::nanobench::Bench().batch(ops.size()).run("HashMap Contains", [&] {
            for (const auto& [op, key, elem] : ops) {
                ankerl::nanobench::doNotOptimizeAway(map.contains(key));
            }
        });

        ankerl::nanobench::Bench().batch(map.size()).run("HashMap Iterator", [&] {
            for (auto [key, elem] : map) {
                ankerl::nanobench::doNotOptimizeAway(key);
                ankerl::nanobench::doNotOptimizeAway(elem);
            }
        });

        ankerl::nanobench::Bench().batch(ops.size()).run("HashMap Erase", [&] {
            for (const auto& [op, key, elem] : ops) {
                ankerl::nanobench::doNotOptimizeAway(map.erase(key));
            }
        });

        ankerl::nanobench::Bench().batch(ops.size()).run("HashMap Mixed", [&] {
            for (const auto& [op, key, elem] : ops) {
                switch (op) {
                    case 0:
                        ankerl::nanobench::doNotOptimizeAway(map.insert(key, elem));
                        break;
                    case 1:
                        ankerl::nanobench::doNotOptimizeAway(map.emplace(key, mt));
                        break;
                    case 2:
                        ankerl::nanobench::doNotOptimizeAway(map.contains(key));
                        break;
                    case 3:
                        ankerl::nanobench::doNotOptimizeAway(map.erase(key));
                        break;
                    default:
                        break;
                }
            }
        });
    }

    // ---------------------------------------------------------------- slot_map
    {
        arena arena(4_MiB);

        std::random_device rd;
        std::mt19937 mt(rd());

        slot_map<uint32_t, Foo, arena_allocator<Foo>> slots(N, arena);

        ankerl::nanobench::Bench().batch(N).run("SlotMap Emplace", [&] {
            for (size_t i = 0; i < N; i++) {
                ankerl::nanobench::doNotOptimizeAway(slots.emplace(mt));
            }
        });

        // Collect the live handles for the lookup benchmarks.
        std::vector<uint32_t> handles;
        handles.reserve(slots.size());
        for (auto [slot, elem] : slots) {
            handles.push_back(slot);
        }

        ankerl::nanobench::Bench().batch(handles.size()).run("SlotMap At", [&] {
            for (auto handle : handles) {
                ankerl::nanobench::doNotOptimizeAway(slots.at(handle));
            }
        });

        ankerl::nanobench::Bench().batch(handles.size()).run("SlotMap Contains", [&] {
            for (auto handle : handles) {
                ankerl::nanobench::doNotOptimizeAway(slots.contains(handle));
            }
        });

        ankerl::nanobench::Bench().batch(slots.size()).run("SlotMap Iterator", [&] {
            for (auto [slot, elem] : slots) {
                ankerl::nanobench::doNotOptimizeAway(slot);
                ankerl::nanobench::doNotOptimizeAway(elem);
            }
        });

        ankerl::nanobench::Bench().batch(handles.size()).run("SlotMap Erase", [&] {
            for (auto handle : handles) {
                ankerl::nanobench::doNotOptimizeAway(slots.erase(handle));
            }
        });
    }
}
