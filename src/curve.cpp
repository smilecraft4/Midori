#include "midori/curve.hpp"

#include <algorithm>

namespace Midori {

namespace {

float Slope(const Curve& curve, std::uint32_t i) noexcept {
    const CurvePoint& a = curve.points[i];
    const CurvePoint& b = curve.points[i + 1];
    return (b.y - a.y) / (b.x - a.x);
}

// Fritsch-Carlson: flat where the curve turns, so it never overshoots a point.
float Tangent(const Curve& curve, std::uint32_t i) noexcept {
    if (i == 0) {
        return Slope(curve, 0);
    }
    if (i == curve.count - 1) {
        return Slope(curve, i - 1);
    }
    const float before = Slope(curve, i - 1);
    const float after = Slope(curve, i);
    if (before * after <= 0.0f) {
        return 0.0f;
    }
    const float h0 = curve.points[i].x - curve.points[i - 1].x;
    const float h1 = curve.points[i + 1].x - curve.points[i].x;
    return 3.0f * (h0 + h1) / ((((2.0f * h1) + h0) / before) + ((h1 + (2.0f * h0)) / after));
}

} // namespace

float CurveEval(const Curve& curve, float x) noexcept {
    x = x > 0.0f ? std::min(x, 1.0f) : 0.0f;
    std::uint32_t k = 0;
    while (k + 2 < curve.count && x > curve.points[k + 1].x) {
        ++k;
    }
    const CurvePoint& a = curve.points[k];
    const CurvePoint& b = curve.points[k + 1];
    const float h = b.x - a.x;
    const float t = (x - a.x) / h;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float y = (((2.0f * t3) - (3.0f * t2) + 1.0f) * a.y) + ((t3 - (2.0f * t2) + t) * h * Tangent(curve, k))
                  + (((-2.0f * t3) + (3.0f * t2)) * b.y) + ((t3 - t2) * h * Tangent(curve, k + 1));
    return std::clamp(y, 0.0f, 1.0f);
}

void CurveBake(const Curve& curve, std::span<float, kCurveSamples> samples) noexcept {
    for (std::uint32_t i = 0; i < kCurveSamples; ++i) {
        samples[i] = CurveEval(curve, static_cast<float>(i) / static_cast<float>(kCurveSamples - 1));
    }
}

bool CurveValid(const Curve& curve) noexcept {
    if (curve.count < 2 || curve.count > kCurvePointsMax || curve.points[0].x != 0.0f
        || curve.points[curve.count - 1].x != 1.0f) {
        return false;
    }
    for (std::uint32_t i = 0; i < curve.count; ++i) {
        const CurvePoint& point = curve.points[i];
        if (!(point.y >= 0.0f && point.y <= 1.0f) || (i > 0 && !(point.x > curve.points[i - 1].x))) {
            return false;
        }
    }
    return true;
}

} // namespace Midori
