#include "test.hpp"

#include "midori/color.hpp"

#include <array>

namespace Midori {

void TestColor(TestContext& t) noexcept {
    constexpr std::array kColors = {
        Color::Srgb{.r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f},
        Color::Srgb{.r = 1.0f, .g = 1.0f, .b = 1.0f, .a = 1.0f},
        Color::Srgb{.r = 0.2f, .g = 0.5f, .b = 0.9f, .a = 0.25f},
        Color::Srgb{.r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f},
    };
    // Each space and back, alpha untouched.
    for (const Color::Srgb color : kColors) {
        const Color::Srgb linear = Color::ToSrgb(Color::ToLinear(color));
        MIDORI_CHECK_NEAR(t, linear.r, color.r, 1e-5);
        MIDORI_CHECK_NEAR(t, linear.g, color.g, 1e-5);
        MIDORI_CHECK_NEAR(t, linear.b, color.b, 1e-5);
        MIDORI_CHECK(t, linear.a == color.a);
        const Color::Srgb okLab = Color::ToSrgb(Color::ToOkLab(color));
        MIDORI_CHECK_NEAR(t, okLab.r, color.r, 2e-4);
        MIDORI_CHECK_NEAR(t, okLab.g, color.g, 2e-4);
        MIDORI_CHECK_NEAR(t, okLab.b, color.b, 2e-4);
        MIDORI_CHECK(t, okLab.a == color.a);
    }

    // sRGB's 0.5 is about 21% of the light; OkLab's white is L 1 with no hue, its black L 0.
    MIDORI_CHECK_NEAR(t, Color::ToLinear(Color::Srgb{.r = 0.5f, .g = 0.5f, .b = 0.5f, .a = 1.0f}).r, 0.21404, 1e-4);
    const Color::OkLab white = Color::ToOkLab(kColors[1]);
    MIDORI_CHECK_NEAR(t, white.L, 1.0f, 1e-3);
    MIDORI_CHECK_NEAR(t, white.a, 0.0f, 1e-3);
    MIDORI_CHECK_NEAR(t, white.b, 0.0f, 1e-3);
    MIDORI_CHECK_NEAR(t, Color::ToOkLab(kColors[0]).L, 0.0f, 1e-6);
}

} // namespace Midori
