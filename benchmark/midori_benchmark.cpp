#include <benchmark/benchmark.h>

#include <climits>
#include <cstdlib>
#include <vector>

static std::vector<int> MidoriDummy(size_t num) {
  std::vector<int> vec;
  for (size_t i = 0; i < num; i++) {
    vec.push_back(rand() % INT_MAX);
  }
  return vec;
}

static void BM_MidoriDummy(benchmark::State& state) {
  size_t num = 2048;
  for (auto _ : state) {
    MidoriDummy(num);
  }
}
BENCHMARK(BM_MidoriDummy);