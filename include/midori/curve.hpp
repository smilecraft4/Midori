#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace Midori {

constexpr std::uint32_t kCurvePointsMax = 8;
// What the GPU, and stamp(), read of a curve: its values at evenly spaced x, linearly between.
constexpr std::uint32_t kCurveSamples = 64;

struct CurvePoint {
    float x;
    float y;
};

// From 0 to 1 and back into 0 to 1, smoothly through its points without overshooting them. The first point is at x 0,
// the last at x 1, the ones between in increasing x; every y within 0 to 1.
struct Curve {
    std::array<CurvePoint, kCurvePointsMax> points{{{.x = 0.0f, .y = 0.0f}, {.x = 1.0f, .y = 1.0f}}};
    std::uint32_t count{2};

    bool operator==(const Curve&) const = default;
};

// Monotone cubic through the points, x clamped to 0..1.
float CurveEval(const Curve& curve, float x) noexcept;
void CurveBake(const Curve& curve, std::span<float, kCurveSamples> samples) noexcept;
// Holds the invariant above, so it can come from a file.
bool CurveValid(const Curve& curve) noexcept;

// Mirrors BrushCurve in stroke.cs.hlsl. Written so a NaN fails the tests and reads the first sample.
inline float CurveSample(std::span<const float, kCurveSamples> samples, float x) noexcept {
    const float clamped = x < 1.0f ? x : 1.0f;
    const float t = (x > 0.0f ? clamped : 0.0f) * static_cast<float>(kCurveSamples - 1);
    const auto i = std::min(static_cast<std::uint32_t>(t), kCurveSamples - 2);
    const float f = t - static_cast<float>(i);
    return samples[i] + ((samples[i + 1] - samples[i]) * f);
}

} // namespace Midori
