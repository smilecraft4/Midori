#include "test.hpp"

#include "midori/curve.hpp"

#include <array>
#include <limits>

namespace Midori {

namespace {

[[nodiscard]] Curve CurveMake(std::initializer_list<CurvePoint> points) noexcept {
    Curve curve{};
    curve.count = 0;
    for (const CurvePoint point : points) {
        curve.points[curve.count++] = point;
    }
    return curve;
}

} // namespace

void TestCurve(TestContext& t) noexcept {
    // Two points make a straight line, clamped past its ends.
    const Curve line{};
    MIDORI_CHECK(t, CurveValid(line));
    for (const float x : {0.0f, 0.25f, 0.5f, 1.0f}) {
        MIDORI_CHECK_NEAR(t, CurveEval(line, x), x, 1e-6);
    }
    MIDORI_CHECK_NEAR(t, CurveEval(line, -1.0f), 0.0f, 1e-6);
    MIDORI_CHECK_NEAR(t, CurveEval(line, 2.0f), 1.0f, 1e-6);

    // Through its points, never decreasing, never past 0..1.
    const Curve bent = CurveMake({{.x = 0.0f, .y = 0.0f}, {.x = 0.5f, .y = 0.8f}, {.x = 1.0f, .y = 1.0f}});
    MIDORI_CHECK(t, CurveValid(bent));
    MIDORI_CHECK_NEAR(t, CurveEval(bent, 0.5f), 0.8f, 1e-6);
    MIDORI_CHECK_NEAR(t, CurveEval(bent, 1.0f), 1.0f, 1e-6);
    float previous = CurveEval(bent, 0.0f);
    bool monotone = true;
    bool inside = true;
    for (int i = 1; i <= 100; ++i) {
        const float y = CurveEval(bent, static_cast<float>(i) / 100.0f);
        monotone = monotone && y >= previous;
        inside = inside && y >= 0.0f && y <= 1.0f;
        previous = y;
    }
    MIDORI_CHECK(t, monotone);
    MIDORI_CHECK(t, inside);

    // What a file may hold that breaks the invariant.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    MIDORI_CHECK(t, !CurveValid(CurveMake({{.x = 0.0f, .y = 0.0f}})));
    MIDORI_CHECK(t, !CurveValid(CurveMake({{.x = 0.1f, .y = 0.0f}, {.x = 1.0f, .y = 1.0f}})));
    MIDORI_CHECK(t, !CurveValid(CurveMake({{.x = 0.0f, .y = 0.0f}, {.x = 0.9f, .y = 1.0f}})));
    MIDORI_CHECK(t, !CurveValid(CurveMake({{.x = 0.0f, .y = 0.0f}, {.x = 1.0f, .y = 1.5f}})));
    MIDORI_CHECK(t, !CurveValid(CurveMake({{.x = 0.0f, .y = nan}, {.x = 1.0f, .y = 1.0f}})));
    MIDORI_CHECK(
        t, !CurveValid(CurveMake(
               {{.x = 0.0f, .y = 0.0f}, {.x = 0.5f, .y = 0.2f}, {.x = 0.5f, .y = 0.3f}, {.x = 1.0f, .y = 1.0f}})));

    // The baked samples, as the GPU and stamp() read them, follow the curve; a NaN reads the first.
    std::array<float, kCurveSamples> samples{};
    CurveBake(bent, samples);
    MIDORI_CHECK_NEAR(t, samples.front(), CurveEval(bent, 0.0f), 1e-6);
    MIDORI_CHECK_NEAR(t, samples.back(), CurveEval(bent, 1.0f), 1e-6);
    MIDORI_CHECK_NEAR(t, CurveSample(samples, 0.37f), CurveEval(bent, 0.37f), 1e-2);
    MIDORI_CHECK_NEAR(t, CurveSample(samples, 5.0f), samples.back(), 1e-6);
    MIDORI_CHECK(t, CurveSample(samples, nan) == samples.front());
}

} // namespace Midori
