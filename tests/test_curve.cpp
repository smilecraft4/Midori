#include "test.hpp"

#include "midori/curve.hpp"
#include "midori/stroke.hpp"

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

    // Centripetal Catmull-Rom: hits p1 at 0 and p2 at 1, degenerates to a line at tension 0, and bows outward on an
    // arc without looping when a long chord meets a short turn.
    const StrokePoint p0{.x = -10.0, .y = 0.0, .time = 0.0, .pressure = 0.2f};
    const StrokePoint p1{.x = 0.0, .y = 10.0, .time = 1.0, .pressure = 0.4f};
    const StrokePoint p2{.x = 10.0, .y = 10.0, .time = 2.0, .pressure = 0.8f};
    const StrokePoint p3{.x = 20.0, .y = 0.0, .time = 3.0, .pressure = 1.0f};
    const StrokePoint at0 = StrokeCatmullRomAt(p0, p1, p2, p3, 0.0);
    const StrokePoint at1 = StrokeCatmullRomAt(p0, p1, p2, p3, 1.0);
    MIDORI_CHECK_NEAR(t, at0.x, p1.x, 1e-6);
    MIDORI_CHECK_NEAR(t, at0.y, p1.y, 1e-6);
    MIDORI_CHECK_NEAR(t, at1.x, p2.x, 1e-6);
    MIDORI_CHECK_NEAR(t, at1.y, p2.y, 1e-6);
    const StrokePoint linear = StrokeCatmullRomAt(p0, p1, p2, p3, 0.5, 0.0f);
    MIDORI_CHECK_NEAR(t, linear.x, 5.0, 1e-6);
    MIDORI_CHECK_NEAR(t, linear.y, 10.0, 1e-6);
    const StrokePoint arc = StrokeCatmullRomAt(p0, p1, p2, p3, 0.5, 1.0f);
    MIDORI_CHECK_NEAR(t, arc.x, 5.0, 1e-6);
    MIDORI_CHECK(t, arc.y > 10.0);
    MIDORI_CHECK_NEAR(t, arc.pressure, 0.6f, 1e-6);
    const StrokePoint far0{.x = -1000.0, .y = -1000.0};
    const StrokePoint tight = StrokeCatmullRomAt(far0, p1, p2, p3, 0.5, 1.0f);
    MIDORI_CHECK(t, tight.x > p1.x && tight.x < p2.x && tight.y >= 10.0 && tight.y < 15.0);

    // Extrapolated tail: position stops at `tail` along the segment, while pressure, tilt and time reach p2's values.
    const StrokePoint tailHalf = StrokeCatmullRomTailAt(p0, p1, p2, 1.0, 0.5f, 1.0f);
    MIDORI_CHECK_NEAR(t, tailHalf.x, 5.0, 1e-6);
    MIDORI_CHECK(t, tailHalf.y > 10.0);
    MIDORI_CHECK_NEAR(t, tailHalf.pressure, p2.pressure, 1e-6);
    MIDORI_CHECK_NEAR(t, tailHalf.time, p2.time, 1e-6);
    const StrokePoint tailFull = StrokeCatmullRomTailAt(p0, p1, p2, 1.0, 1.0f, 1.0f);
    MIDORI_CHECK_NEAR(t, tailFull.x, p2.x, 1e-6);
    MIDORI_CHECK_NEAR(t, tailFull.y, p2.y, 1e-6);
}

} // namespace Midori
