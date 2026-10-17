#include "midori/color.hpp"

#include <cmath>

namespace Midori::Color {

namespace {

float SrgbToLinear(float c) noexcept {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float c) noexcept {
    return c <= 0.0031308f ? c * 12.92f : (1.055f * std::pow(c, 1.0f / 2.4f)) - 0.055f;
}

} // namespace

Linear ToLinear(Srgb srgb) noexcept {
    return Linear{.r = SrgbToLinear(srgb.r), .g = SrgbToLinear(srgb.g), .b = SrgbToLinear(srgb.b), .a = srgb.a};
}

Srgb ToSrgb(Linear linear) noexcept {
    return Srgb{.r = LinearToSrgb(linear.r), .g = LinearToSrgb(linear.g), .b = LinearToSrgb(linear.b), .a = linear.a};
}

// Matrices from https://bottosson.github.io/posts/oklab/
OkLab ToOkLab(Linear linear) noexcept {
    const float l = std::cbrt((0.4122214708f * linear.r) + (0.5363325363f * linear.g) + (0.0514459929f * linear.b));
    const float m = std::cbrt((0.2119034982f * linear.r) + (0.6806995451f * linear.g) + (0.1073969566f * linear.b));
    const float s = std::cbrt((0.0883024619f * linear.r) + (0.2817188376f * linear.g) + (0.6299787005f * linear.b));
    return OkLab{
        .L = (0.2104542553f * l) + (0.7936177850f * m) - (0.0040720468f * s),
        .a = (1.9779984951f * l) - (2.4285922050f * m) + (0.4505937099f * s),
        .b = (0.0259040371f * l) + (0.7827717662f * m) - (0.8086757660f * s),
        .alpha = linear.a,
    };
}

Linear ToLinear(OkLab okLab) noexcept {
    const float l = okLab.L + (0.3963377774f * okLab.a) + (0.2158037573f * okLab.b);
    const float m = okLab.L - (0.1055613458f * okLab.a) - (0.0638541728f * okLab.b);
    const float s = okLab.L - (0.0894841775f * okLab.a) - (1.2914855480f * okLab.b);
    const float l3 = l * l * l;
    const float m3 = m * m * m;
    const float s3 = s * s * s;
    return Linear{
        .r = (4.0767416621f * l3) - (3.3077115913f * m3) + (0.2309699292f * s3),
        .g = (-1.2684380046f * l3) + (2.6097574011f * m3) - (0.3413193965f * s3),
        .b = (-0.0041960863f * l3) - (0.7034186147f * m3) + (1.7076147010f * s3),
        .a = okLab.alpha,
    };
}

Srgb ToSrgb(OkLab okLab) noexcept {
    return ToSrgb(ToLinear(okLab));
}

OkLab ToOkLab(Srgb srgb) noexcept {
    return ToOkLab(ToLinear(srgb));
}

} // namespace Midori::Color
