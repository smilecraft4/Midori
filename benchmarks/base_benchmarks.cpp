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
    {
        Arena arena(1_MiB);

        std::random_device rd;
        std::mt19937 mt(rd());
        std::uniform_int_distribution<int> keyDist(-16, 16);
        std::uniform_int_distribution<int> opsDist(0, 2);

        std::vector<std::tuple<uint8_t, Bar>> ops(2048);
        for (auto& [type, key] : ops) {
            type = opsDist(mt);
            key.a = keyDist(mt);
            key.b = keyDist(mt);
        }

        HashSet<Bar, arena_allocator<Bar>> set(ops.size(), arena);

        ankerl::nanobench::Bench().batch(ops.size()).run("HashSet Insert", [&] {
            for (const auto& [_, key] : ops) {
                ankerl::nanobench::doNotOptimizeAway(set.Insert(key));
            }
        });

        ankerl::nanobench::Bench().batch(ops.size()).run("HashSet Contains", [&] {
            for (const auto& [_, key] : ops) {
                ankerl::nanobench::doNotOptimizeAway(set.Contains(key));
            }
        });

        ankerl::nanobench::Bench().batch(ops.size()).run("HashSet Erase", [&] {
            for (const auto& [_, key] : ops) {
                ankerl::nanobench::doNotOptimizeAway(set.Erase(key));
            }
        });

        ankerl::nanobench::Bench().batch(ops.size()).run("HashSet Mixed", [&] {
            for (const auto& [op, key] : ops) {
                switch (op) {
                    case 0:
                        ankerl::nanobench::doNotOptimizeAway(set.Insert(key));
                        break;
                    case 1:
                        ankerl::nanobench::doNotOptimizeAway(set.Contains(key));
                        break;
                    case 2:
                        ankerl::nanobench::doNotOptimizeAway(set.Erase(key));
                        break;
                    default:
                        break;
                }
            }
        });
    }
    {
        Arena arena(1_MiB);

        std::random_device rd;
        std::mt19937 mt(rd());
        std::uniform_real_distribution<float> elemDist(-1.0f, 1.0f);
        std::uniform_int_distribution<int> keyDist(-16, 16);
        std::uniform_int_distribution<int> opsDist(0, 3);

        std::vector<std::tuple<uint8_t, Bar, Foo>> ops;
        ops.reserve(2048);
        for (size_t i = 0; i < ops.capacity(); i++) {
            ops.emplace_back(std::tuple{opsDist(mt), Bar{.a = keyDist(mt), .b = keyDist(mt)}, Foo{mt}});
        }

        std::vector<Bar> em;
        em.reserve(2048);
        for (size_t i = 0; i < em.capacity(); i++) {
            em.emplace_back(Bar{.a = keyDist(mt), .b = keyDist(mt)});
        }

        HashMap<Bar, Foo, arena_allocator<Bar>> map(ops.size(), arena);

        ankerl::nanobench::Bench().batch(ops.size()).run("HashMap Insert", [&] {
            for (const auto& [ops, key, elem] : ops) {
                ankerl::nanobench::doNotOptimizeAway(map.Insert(key, elem));
            }
        });

        ankerl::nanobench::Bench().batch(ops.size()).run("HashMap Emplace", [&] {
            for (const auto& key : em) {
                ankerl::nanobench::doNotOptimizeAway(map.Emplace(key, mt));
            }
        });

        ankerl::nanobench::Bench().batch(ops.size()).run("HashMap Contains", [&] {
            for (const auto& [ops, key, elem] : ops) {
                ankerl::nanobench::doNotOptimizeAway(map.Contains(key));
            }
        });

        ankerl::nanobench::Bench().batch(ops.size()).run("HashMap Erase", [&] {
            for (const auto& [ops, key, elem] : ops) {
                ankerl::nanobench::doNotOptimizeAway(map.Erase(key));
            }
        });

        ankerl::nanobench::Bench().batch(ops.size()).run("HashMap Mixed", [&] {
            for (const auto& [op, key, elem] : ops) {
                switch (op) {
                    case 0:
                        ankerl::nanobench::doNotOptimizeAway(map.Insert(key, elem));
                        break;
                    case 1:
                        ankerl::nanobench::doNotOptimizeAway(map.Emplace(key, mt));
                        break;
                    case 2:
                        ankerl::nanobench::doNotOptimizeAway(map.Contains(key));
                        break;
                    case 3:
                        ankerl::nanobench::doNotOptimizeAway(map.Erase(key));
                        break;
                    default:
                        break;
                }
            }
        });
    }
}