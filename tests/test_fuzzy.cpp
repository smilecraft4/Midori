#include "test.hpp"

#include "midori/action.hpp"

namespace Midori {

void TestFuzzy(TestContext& t) noexcept {
    // In order, case ignored; missing letters or the wrong order do not match.
    MIDORI_CHECK(t, FuzzyScore("lay", "layer.new") > kFuzzyNone);
    MIDORI_CHECK(t, FuzzyScore("LAY", "layer.new") > kFuzzyNone);
    MIDORI_CHECK(t, FuzzyScore("ln", "layer.new") > kFuzzyNone);
    MIDORI_CHECK(t, FuzzyScore("xyz", "layer.new") == kFuzzyNone);
    MIDORI_CHECK(t, FuzzyScore("yal", "layer") == kFuzzyNone);
    // Consecutive letters beat scattered ones.
    MIDORI_CHECK(t, FuzzyScore("lay", "layer") > FuzzyScore("lay", "l.a.y"));
    // Every term must match, in any order.
    MIDORI_CHECK(t, FuzzyScoreTerms("new layer", "layer.new") > kFuzzyNone);
    MIDORI_CHECK(t, FuzzyScoreTerms("layer zzz", "layer.new") == kFuzzyNone);
}

} // namespace Midori
