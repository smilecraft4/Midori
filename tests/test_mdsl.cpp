#include "test.hpp"

#include "midori/embed/round_mdsl.h"
#include "midori/mdsl.hpp"
#include "midori/tool.hpp"

namespace Midori {

void TestMdsl(TestContext& t) noexcept {
    // The built-in brush translates, with the params the brush adjust and the brush keys set.
    const auto round = MdslTranslate(round_mdsl);
    if (!MIDORI_CHECK(t, round.has_value())) {
        std::println("       {}:{}: {}", round.error().line, round.error().column, round.error().message);
        return;
    }
    MIDORI_CHECK(t, !round->hlsl.empty());
    MIDORI_CHECK(t, !round->stamp.ops.empty());
    for (const char* name : {"size", "spacing", "hardness", "flow", "opacity"}) {
        MIDORI_CHECK(t, BrushParamFind(round->params, name) >= 0);
    }
    const int size = BrushParamFind(round->params, "size");
    if (size >= 0) {
        MIDORI_CHECK(t, round->params[static_cast<std::size_t>(size)].min == 0.5f);
        MIDORI_CHECK(t, round->params[static_cast<std::size_t>(size)].max == 512.0f);
    }
    // The same source, the same shader: brushes compile once per change.
    const auto again = MdslTranslate(round_mdsl);
    MIDORI_CHECK(t, again && again->hlsl == round->hlsl);

    // What is no brush says so.
    const auto broken = MdslTranslate("this is not a brush");
    MIDORI_CHECK(t, !broken && !broken.error().message.empty());

    // A texture takes vec2 coordinates and an optional float LOD bias in paint().
    const auto textured = MdslTranslate("param texture alpha;\n"
                                        "StampOutput stamp(StampInput input) { StampOutput o; return o; }\n"
                                        "PaintOutput paint(PaintInput input) {\n"
                                        "    PaintOutput o;\n"
                                        "    vec4 a = alpha(input.local) * alpha(input.local, 0.5);\n"
                                        "    o.amount = a.r;\n"
                                        "    return o;\n"
                                        "}\n");
    MIDORI_CHECK(t, textured.has_value());
}

} // namespace Midori
