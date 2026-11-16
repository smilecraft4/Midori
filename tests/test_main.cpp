#include "test.hpp"

#include <array>
#include <print>
#include <string_view>

namespace {

struct TestCase {
    const char* name;
    void (*run)(Midori::TestContext&) noexcept;
};

constexpr std::array kTests = {
    TestCase{.name = "color", .run = Midori::TestColor},
    TestCase{.name = "curve", .run = Midori::TestCurve},
    TestCase{.name = "fuzzy", .run = Midori::TestFuzzy},
    TestCase{.name = "keymap", .run = Midori::TestKeymap},
    TestCase{.name = "mdsl", .run = Midori::TestMdsl},
    TestCase{.name = "tile keys", .run = Midori::TestTileKeys},
    TestCase{.name = "atlas cells", .run = Midori::TestAtlasCells},
    TestCase{.name = "viewport", .run = Midori::TestViewport},
};

} // namespace

// Runs every test, or those whose name holds the first argument. Exits 1 when a check failed.
int main(int argc, char** argv) {
    const std::string_view filter = argc > 1 ? argv[1] : "";
    int failed = 0;
    int run = 0;
    for (const TestCase& test : kTests) {
        if (!std::string_view(test.name).contains(filter)) {
            continue;
        }
        Midori::TestContext t{.test = test.name, .checks = 0, .failures = 0};
        test.run(t);
        ++run;
        failed += t.failures > 0 ? 1 : 0;
        std::println("{} {} ({} checks, {} failed)", t.failures > 0 ? "FAIL" : "ok  ", test.name, t.checks, t.failures);
    }
    std::println("{} of {} tests passed", run - failed, run);
    return failed > 0 || run == 0 ? 1 : 0;
}
