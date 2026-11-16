// Times the core's hot functions without a window or a GPU. Each one is calibrated to run at least kSampleTime per
// sample, then sampled kSamples times; the fastest and the median time per call are printed. Only Release builds tell
// anything. The first argument keeps the benchmarks whose name holds it.

#include "midori/action.hpp"
#include "midori/color.hpp"
#include "midori/curve.hpp"
#include "midori/embed/round_mdsl.h"
#include "midori/mdsl.hpp"
#include "midori/tile.hpp"
#include "midori/tool.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <print>
#include <string_view>
#include <vector>

#include <qoi.h>

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::chrono::milliseconds kSampleTime{20};
constexpr int kSamples = 9;

// `run` returns something derived from its work, summed into `sink` so the optimizer keeps the work.
template<class Run>
void Bench(std::string_view filter, std::string_view name, Run&& run) noexcept {
    if (!name.contains(filter)) {
        return;
    }
    volatile double sink = 0.0;
    std::uint64_t calls = 1;
    for (;;) {
        const auto start = Clock::now();
        for (std::uint64_t i = 0; i < calls; ++i) {
            sink = sink + static_cast<double>(run());
        }
        if (Clock::now() - start >= kSampleTime) {
            break;
        }
        calls *= 2;
    }
    std::array<double, kSamples> nanoseconds{};
    for (double& sample : nanoseconds) {
        const auto start = Clock::now();
        for (std::uint64_t i = 0; i < calls; ++i) {
            sink = sink + static_cast<double>(run());
        }
        const std::chrono::duration<double, std::nano> elapsed = Clock::now() - start;
        sample = elapsed.count() / static_cast<double>(calls);
    }
    std::ranges::sort(nanoseconds);
    std::println("{:<32} {:>14.1f} {:>14.1f}", name, nanoseconds.front(), nanoseconds[kSamples / 2]);
}

// A level-0 tile as a brush leaves it: soft dabs over transparency, with a little grain, premultiplied.
[[nodiscard]] std::vector<std::byte> TilePainted() noexcept {
    std::vector<std::byte> pixels(Midori::TileBytes(Midori::PixelFormat::RGBA8));
    constexpr auto kSide = static_cast<std::uint32_t>(Midori::kTileSize);
    std::uint32_t random = 12345;
    for (std::uint32_t y = 0; y < kSide; ++y) {
        for (std::uint32_t x = 0; x < kSide; ++x) {
            const float dx = (static_cast<float>(x) - 100.0f) / 90.0f;
            const float dy = (static_cast<float>(y) - 140.0f) / 70.0f;
            random = (random * 1664525u) + 1013904223u;
            const float grain = static_cast<float>(random >> 24) / 2550.0f;
            const float alpha = std::clamp((1.2f - std::sqrt((dx * dx) + (dy * dy))) + grain, 0.0f, 1.0f);
            std::byte* texel = pixels.data() + (((std::size_t{y} * kSide) + x) * 4);
            texel[0] = static_cast<std::byte>(std::lround(alpha * 200.0f));
            texel[1] = static_cast<std::byte>(std::lround(alpha * 90.0f));
            texel[2] = static_cast<std::byte>(std::lround(alpha * 40.0f));
            texel[3] = static_cast<std::byte>(std::lround(alpha * 255.0f));
        }
    }
    return pixels;
}

} // namespace

int main(int argc, char** argv) {
    using namespace Midori;
    const std::string_view filter = argc > 1 ? argv[1] : "";
    std::println("{:<32} {:>14} {:>14}", "benchmark", "fastest ns", "median ns");

    const auto round = MdslTranslate(round_mdsl);
    if (!round) {
        std::println("round.mdsl does not translate: {}", round.error().message);
        return 1;
    }
    Bench(filter, "mdsl.translate round", [] { return MdslTranslate(round_mdsl)->hlsl.size(); });

    // stamp() with the round brush's defaults, as a stroke runs it once per dab.
    BrushState brush{};
    BrushParamsDefault(brush, *round);
    std::array<float, kMdslParamFloatsMax> params{};
    for (std::size_t i = 0; i < round->params.size(); ++i) {
        const MdslParam& param = round->params[i];
        std::copy_n(brush.params[i].value.begin(), MdslComponents(param.type), params.begin() + param.offset);
    }
    std::array<float, kMdslCurveFloats> curves{};
    BrushCurvesBake(brush, curves);
    const MdslStampInput stampInput{
        .params = params,
        .curves = curves,
        .dab = {.pressure = 0.7f,
                .tiltX = 0.1f,
                .tiltY = -0.2f,
                .velocityX = 300.0f,
                .velocityY = 40.0f,
                .directionX = 0.99f,
                .directionY = 0.13f,
                .random = 0.5f,
                .along = 120.0f,
                .time = 0.4f},
    };
    Bench(filter, "mdsl.stamp round", [&] { return MdslStampRun(round->stamp, stampInput).radius; });

    // A tile into and out of the canvas file and the cache. Encoding writes into the thread's one buffer, so the
    // decoded input is a copy of it.
    const std::vector<std::byte> painted = TilePainted();
    const std::vector<std::byte> empty(painted.size());
    const qoi_desc desc{.width = kTileSize, .height = kTileSize, .channels = 4, .colorspace = QOI_SRGB};
    Bench(filter, "qoi.encode painted tile", [&] {
        int size = 0;
        qoi_encode(painted.data(), &desc, &size);
        return size;
    });
    Bench(filter, "qoi.encode empty tile", [&] {
        int size = 0;
        qoi_encode(empty.data(), &desc, &size);
        return size;
    });
    int encodedSize = 0;
    const auto* encodedData = static_cast<const std::byte*>(qoi_encode(painted.data(), &desc, &encodedSize));
    const std::vector<std::byte> encoded(encodedData, encodedData + encodedSize);
    std::println("{:<32} {:>14} bytes, {:.1f}% of raw", "  painted tile as QOI", encodedSize,
                 100.0 * encodedSize / static_cast<double>(painted.size()));
    Bench(filter, "qoi.decode painted tile", [&] {
        qoi_desc decoded{};
        const void* pixels = qoi_decode(encoded.data(), static_cast<int>(encoded.size()), &decoded, 4);
        return pixels ? decoded.width : 0u;
    });

    // The F1 search scores every action's name per key typed.
    Bench(filter, "fuzzy.search every action", [] {
        float total = 0.0f;
        for (const ActionInfo& action : kActions) {
            total += std::max(FuzzyScoreTerms("lay new", action.name), 0.0f);
        }
        return total;
    });

    Curve curve{};
    curve.points = {{{.x = 0.0f, .y = 0.0f}, {.x = 0.3f, .y = 0.6f}, {.x = 0.7f, .y = 0.8f}, {.x = 1.0f, .y = 1.0f}}};
    curve.count = 4;
    Bench(filter, "curve.bake 4 points", [&] {
        std::array<float, kCurveSamples> samples{};
        CurveBake(curve, samples);
        return samples[kCurveSamples / 2];
    });

    // A key press against the default bindings.
    KeymapState keymap{};
    for (const KeyBindingText& text : kDefaultBindings) {
        if (auto binding = KeyBindingParse(text.keys, kActions[std::to_underlying(text.action)].kind)) {
            binding->action = text.action;
            binding->arg = text.arg;
            binding->context = text.context;
            keymap.bindings.push_back(*binding);
        }
    }
    Bench(filter, "keymap.press default bindings", [&] {
        const std::int32_t first = KeymapPress(keymap, {.key = Key::K, .mods = KeyMod::LCtrl}, ActionContext::Canvas);
        const std::int32_t second = KeymapPress(keymap, {.key = Key::N, .mods = KeyMod::None}, ActionContext::Canvas);
        return first + second;
    });

    // The colour panel converts per pixel of its OkLab fields.
    Bench(filter, "color.oklab round trip x1024", [] {
        float total = 0.0f;
        for (int i = 0; i < 1024; ++i) {
            const float v = static_cast<float>(i) / 1023.0f;
            total += Color::ToSrgb(Color::ToOkLab(Color::Srgb{.r = v, .g = 1.0f - v, .b = 0.5f, .a = 1.0f})).g;
        }
        return total;
    });
    return 0;
}
