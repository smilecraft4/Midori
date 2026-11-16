#pragma once

#include <cmath>
#include <print>

namespace Midori {

// The test running now. A failed check prints itself and counts, and the test goes on, so one run shows every failure.
struct TestContext {
    const char* test;
    int checks;
    int failures;
};

inline bool TestCheck(TestContext& t, bool passed, const char* expression, const char* file, int line) noexcept {
    ++t.checks;
    if (!passed) {
        ++t.failures;
        std::println("  FAIL {}:{}: {}", file, line, expression);
    }
    return passed;
}

inline bool TestCheckNear(TestContext& t, double actual, double expected, double tolerance, const char* expression,
                          const char* file, int line) noexcept {
    const bool passed = std::abs(actual - expected) <= tolerance;
    if (!TestCheck(t, passed, expression, file, line)) {
        std::println("       {:.9g}, expected {:.9g} within {}", actual, expected, tolerance);
    }
    return passed;
}

// The tests, one function each, listed in test_main.cpp.
void TestColor(TestContext& t) noexcept;
void TestCurve(TestContext& t) noexcept;
void TestFuzzy(TestContext& t) noexcept;
void TestKeymap(TestContext& t) noexcept;
void TestMdsl(TestContext& t) noexcept;
void TestTileKeys(TestContext& t) noexcept;
void TestAtlasCells(TestContext& t) noexcept;
void TestViewport(TestContext& t) noexcept;

} // namespace Midori

#define MIDORI_CHECK(t, condition) ::Midori::TestCheck(t, (condition), #condition, __FILE__, __LINE__)
#define MIDORI_CHECK_NEAR(t, actual, expected, tolerance)                                                              \
    ::Midori::TestCheckNear(t, static_cast<double>(actual), static_cast<double>(expected), tolerance,                  \
                            #actual " ~ " #expected, __FILE__, __LINE__)
