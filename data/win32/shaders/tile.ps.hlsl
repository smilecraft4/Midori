#include "atlas.hlsli"
#include "common.hlsli"

Texture2DArray<float4> tilePools[kTileAtlasMax] : register(t0);

// The cell drawn at a tile, from the origin tile; kTileAtlasCellInvalid outside the page table.
uint TileCell(int2 tile) {
    const int2 local = tile - regionOrigin;
    if (any(local < 0) || any(local >= int2(regionSize))) {
        return kTileAtlasCellInvalid;
    }
    return pageTable[local.y * regionSize.x + local.x];
}

// A texel of the view level's grid, which runs on across the tiles, so filtering never sees an edge. A tile shown from
// level 0 while its own level is missing (painting deletes it until the stroke ends) averages the texel's footprint,
// the same box tile_mip.cs.hlsl builds the level with, so nothing changes once it is built.
float4 LevelTexel(int2 texel) {
    const uint shift = 8 - level; // texels per tile: 256 >> level
    const uint cell = TileCell(texel >> shift);
    if (cell == kTileAtlasCellInvalid) {
        return 0.0;
    }
    const uint2 local = uint2(texel & int((1u << shift) - 1u));
    const uint scale = 1u << (level - TileAtlasCellLevel(cell));
    const uint2 origin = TileAtlasCellOrigin(cell) + local * scale;
    const uint page = TileAtlasCellPage(cell);
    const uint atlas = TileAtlasPageAtlas(page);
    const uint slice = TileAtlasPageSlice(page);
    float4 sum = 0.0;
    [loop]
    for (uint y = 0; y < scale; ++y) {
        [loop]
        for (uint x = 0; x < scale; ++x) {
            sum += tilePools[NonUniformResourceIndex(atlas)].Load(int4(origin + uint2(x, y), slice, 0));
        }
    }
    return sum / float(scale * scale);
}

// CanvasFilter in canvas.hpp.
static const uint kFilterNearest = 0;
static const uint kFilterBilinear = 1;
static const uint kFilterBicubic = 2;
static const uint kFilterMitchell = 3;
static const uint kFilterLanczos2 = 4;
static const uint kFilterLanczos3 = 5;

static const float kPi = 3.14159265358979;

// Mitchell-Netravali cubics: B = 0, C = 1/2 is Catmull-Rom; B = C = 1/3 trades a little sharpness for no halo.
float Cubic(float x, float b, float c) {
    x = abs(x);
    if (x < 1.0) {
        return ((12.0 - 9.0 * b - 6.0 * c) * x * x * x + (-18.0 + 12.0 * b + 6.0 * c) * x * x + (6.0 - 2.0 * b)) / 6.0;
    }
    if (x < 2.0) {
        return ((-b - 6.0 * c) * x * x * x + (6.0 * b + 30.0 * c) * x * x + (-12.0 * b - 48.0 * c) * x
                + (8.0 * b + 24.0 * c))
             / 6.0;
    }
    return 0.0;
}

// A windowed sinc over `a` texels each side.
float Lanczos(float x, float a) {
    if (abs(x) < 1e-5) {
        return 1.0;
    }
    if (abs(x) >= a) {
        return 0.0;
    }
    return a * sin(kPi * x) * sin(kPi * x / a) / (kPi * kPi * x * x);
}

float FilterWeight(float x) {
    switch (filter) {
        case kFilterBicubic:
            return Cubic(x, 0.0, 0.5);
        case kFilterMitchell:
            return Cubic(x, 1.0 / 3.0, 1.0 / 3.0);
        case kFilterLanczos2:
            return Lanczos(x, 2.0);
        default:
            return Lanczos(x, 3.0);
    }
}

// The selection's coverage at a canvas point, from its level-0 mask tile; 0 where it has none.
float SelectionAt(float2 canvas) {
    const int2 texel = int2(floor(canvas));
    const int2 local = (texel >> 8) - regionOrigin;
    if (any(local < 0) || any(local >= int2(regionSize))) {
        return 0.0;
    }
    const uint cell = pageTable[regionSize.x * regionSize.y + local.y * regionSize.x + local.x];
    if (cell == kTileAtlasCellInvalid) {
        return 0.0;
    }
    const uint page = TileAtlasCellPage(cell);
    const uint2 origin = TileAtlasCellOrigin(cell) + uint2(texel & 255);
    return tilePools[NonUniformResourceIndex(TileAtlasPageAtlas(page))]
        .Load(int4(origin, TileAtlasPageSlice(page), 0))
        .a;
}

// Filtered by hand over the premultiplied texels: nearest, bilinear over 2x2, or a separable kernel over 4x4 (cubics,
// Lanczos-2) or 6x6 (Lanczos-3), which keep edges sharp where bilinear blurs them, rotated or between zoom steps.
float4 CanvasColor(float2 canvas) {
    const float2 pos = canvas / float(1u << level) - 0.5;
    const int2 base = int2(floor(pos));
    const float2 f = pos - float2(base);
    if (filter == kFilterNearest) {
        return LevelTexel(base + int2(round(f)));
    }
    if (filter == kFilterBilinear) {
        const float4 top = lerp(LevelTexel(base), LevelTexel(base + int2(1, 0)), f.x);
        const float4 bottom = lerp(LevelTexel(base + int2(0, 1)), LevelTexel(base + int2(1, 1)), f.x);
        return lerp(top, bottom, f.y);
    }

    // Taps base + [1 - radius, radius] on each axis, weighted by their distance to `pos`. Lanczos weights do not sum to
    // one, so both axes are normalized.
    const int radius = filter == kFilterLanczos3 ? 3 : 2;
    float wx[6];
    float wy[6];
    float sumX = 0.0;
    float sumY = 0.0;
    for (int i = 0; i < 2 * radius; ++i) {
        const float offset = float(i + 1 - radius);
        wx[i] = FilterWeight(f.x - offset);
        wy[i] = FilterWeight(f.y - offset);
        sumX += wx[i];
        sumY += wy[i];
    }
    float4 color = 0.0;
    for (int y = 0; y < 2 * radius; ++y) {
        float4 row = 0.0;
        for (int x = 0; x < 2 * radius; ++x) {
            row += LevelTexel(base + int2(x + 1 - radius, y + 1 - radius)) * wx[x];
        }
        color += row * wy[y];
    }
    color /= sumX * sumY;
    // Negative lobes overshoot at sharp edges: back to a valid premultiplied color.
    color.a = saturate(color.a);
    color.rgb = clamp(color.rgb, 0.0, color.a);
    return color;
}

// The selection's outline as Photoshop draws it: one pixel wide on its inner side, still dashes of 4 black and 4 white
// pixels running along the edge.
float4 PSMain(float4 position : SV_Position) : SV_Target {
    const float2 canvas =
        float2(dot(pixelToCanvasX, position.xy), dot(pixelToCanvasY, position.xy)) + canvasOffset;
    if (selection != 0 && SelectionAt(canvas) >= 0.5) {
        const float2 right = float2(pixelToCanvasX.x, pixelToCanvasY.x);
        const float2 down = float2(pixelToCanvasX.y, pixelToCanvasY.y);
        const bool across = SelectionAt(canvas + down) < 0.5 || SelectionAt(canvas - down) < 0.5;
        const bool along = SelectionAt(canvas + right) < 0.5 || SelectionAt(canvas - right) < 0.5;
        if (across || along) {
            // A horizontal edge dashes along x, a vertical one along y.
            const uint dash = uint(across ? position.x : position.y) / 4;
            return (dash & 1) != 0 ? float4(1.0, 1.0, 1.0, 1.0) : float4(0.0, 0.0, 0.0, 1.0);
        }
    }
    return CanvasColor(canvas);
}
