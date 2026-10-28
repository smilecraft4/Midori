#include "atlas.hlsli"
#include "blend.hlsli"

// One UAV per atlas, volatile like the tile pipeline's SRVs; then the brush's textures, kMdslTexturesMax of them.
#define STROKE_ROOT_SIGNATURE                                                                          \
    "RootConstants(num32BitConstants = 16, b0),"                                                       \
    "SRV(t0),"                                                                                         \
    "DescriptorTable(UAV(u0, numDescriptors = 16, flags = DESCRIPTORS_VOLATILE)),"                     \
    "CBV(b1),"                                                                                         \
    "DescriptorTable(SRV(t1, numDescriptors = 4, flags = DESCRIPTORS_VOLATILE)),"                      \
    "StaticSampler(s0, filter = FILTER_MIN_MAG_MIP_LINEAR, addressU = TEXTURE_ADDRESS_WRAP, "          \
    "addressV = TEXTURE_ADDRESS_WRAP)"

static const uint kStrokeGroupSize = 8;
// StrokeMode in stroke.hpp.
static const uint kStrokePaint = 0;
static const uint kStrokeErase = 1;
static const uint kStrokeReplace = 2;
static const uint kStrokeIntersect = 3;
static const uint kStrokeExclude = 4;
// maskPage: nothing selected anywhere, so nothing clips; or nothing selected in this tile.
static const uint kStrokeMaskNone = 0xFFFFFFFF;
static const uint kStrokeMaskEmpty = 0xFFFFFFFE;
// StrokeShape in stroke.hpp.
static const uint kStrokeBrush = 0;
static const uint kStrokeLasso = 2;
static const uint kStrokeFillRect = 4;
static const uint kStrokeFillLasso = 5;

// StrokeDab in stroke.hpp, its inputs MdslDab in mdsl.hpp.
struct StrokeDab {
    float2 pos;
    float radius;
    float width; // a rectangle from pos when above 0
    float height;
    float pressure;
    float2 tilt;
    float2 velocity;
    float2 direction;
    float random;
    float along;
    float time;
    float params[11]; // kMdslParamFloatsMax in mdsl.hpp
};

// The brush's, the same for the whole stroke. Mirrors StrokeBrushConstants in win32_graphics.cpp.
static const uint kBrushCurveSamples = 64; // kCurveSamples in curve.hpp
cbuffer Brush : register(b1) {
    int2 strokeOrigin;      // the tile the stroke's coordinates are relative to
    uint brushTextures;     // one bit per texture that has an image
    uint brushPad;
    float4 brushCurves[64]; // kMdslCurveFloats / 4, kBrushCurveSamples per curve
};

Texture2D<float4> brushTextureMaps[4] : register(t1); // kMdslTexturesMax in mdsl.hpp
SamplerState brushSampler : register(s0);

// Mirrors CurveSample in curve.hpp.
float BrushCurve(uint c, float x) {
    const float t = saturate(x) * (kBrushCurveSamples - 1);
    const uint i = min((uint)t, kBrushCurveSamples - 2);
    const uint at = c * kBrushCurveSamples + i;
    const float a = brushCurves[at >> 2][at & 3];
    const float b = brushCurves[(at + 1) >> 2][(at + 1) & 3];
    return lerp(a, b, t - i);
}

// Straight, repeating every 1. A texture without an image is white.
float4 BrushTexture(uint t, float2 uv) {
    if ((brushTextures & (1u << t)) == 0) {
        return 1.0;
    }
    return brushTextureMaps[t].SampleLevel(brushSampler, uv, 0.0);
}

// Paint, PaintMake and BrushPaint: the brush's, translated from its mdsl at runtime.
#include "brush.hlsli"

cbuffer Stroke : register(b0) {
    float4 color; // straight; Erase: how much of each channel goes, times its alpha
    int2 tileGridPos; // relative to the stroke's origin tile, like the dabs
    uint2 rectOrigin;
    uint dstPage;
    uint srcPage; // the tile before the stroke
    uint dabFirst;
    uint dabEnd;
    uint blendMode;
    uint mode;
    uint maskPage; // the selection's mask at this tile, coverage in alpha
    uint shape;
};

StructuredBuffer<StrokeDab> dabs : register(t0);
RWTexture2DArray<float4> tilePools[kTileAtlasMax] : register(u0);

// How much of the texel around `pos` the rectangle covers, along each axis.
float RectMask(StrokeDab dab, float2 pos) {
    const float2 cover = saturate(min(pos + 0.5, dab.pos + float2(dab.width, dab.height)) - max(pos - 0.5, dab.pos));
    return cover.x * cover.y;
}

// The polygon through every dab's position, closed: inside by the nonzero winding rule, its edge antialiased by the
// distance to it.
float LassoMask(float2 pos) {
    if (dabEnd < 3) {
        return 0.0;
    }
    int winding = 0;
    float dist = 1e30;
    float2 a = dabs[dabEnd - 1].pos;
    for (uint i = 0; i < dabEnd; ++i) {
        const float2 b = dabs[i].pos;
        const float2 ab = b - a;
        const float side = ab.x * (pos.y - a.y) - (pos.x - a.x) * ab.y;
        if (a.y <= pos.y) {
            if (b.y > pos.y && side > 0.0) {
                ++winding;
            }
        } else if (b.y <= pos.y && side < 0.0) {
            --winding;
        }
        const float t = saturate(dot(pos - a, ab) / max(dot(ab, ab), 1e-12));
        dist = min(dist, distance(pos, a + t * ab));
        a = b;
    }
    return saturate(0.5 + (winding != 0 ? dist : -dist));
}

// The distance from `pos` to the polygon through dabs[1, dabEnd), closed, positive inside by the nonzero winding rule;
// `nearest` is the closest point of its outline.
float FillDistance(float2 pos, out float2 nearest) {
    int winding = 0;
    float best = 1e30;
    nearest = pos;
    float2 a = dabs[dabEnd - 1].pos;
    for (uint i = 1; i < dabEnd; ++i) {
        const float2 b = dabs[i].pos;
        const float2 ab = b - a;
        const float side = ab.x * (pos.y - a.y) - (pos.x - a.x) * ab.y;
        if (a.y <= pos.y) {
            if (b.y > pos.y && side > 0.0) {
                ++winding;
            }
        } else if (b.y <= pos.y && side < 0.0) {
            --winding;
        }
        const float2 q = a + saturate(dot(pos - a, ab) / max(dot(ab, ab), 1e-12)) * ab;
        const float2 to = pos - q;
        if (dot(to, to) < best) {
            best = dot(to, to);
            nearest = q;
        }
        a = b;
    }
    return winding != 0 ? sqrt(best) : -sqrt(best);
}

// The brush once per texel over the polygon, like a stamp stretched to the shape: `local` points out to the nearest
// edge, its length 1 on the edge and 0 the stamp's radius in, so the brush's falloff runs along the edge and its middle
// fills the inside.
float4 FillPaint(float2 pos, float2 canvasPos) {
    const StrokeDab dab = dabs[0];
    if (dabEnd < 4 || dab.radius <= 0.0) {
        return 0.0;
    }
    float2 nearest;
    const float inside = FillDistance(pos, nearest);
    const float gap = abs(inside);
    float2 outward = float2(1.0, 0.0);
    if (gap > 1e-4) {
        outward = (inside > 0.0 ? nearest - pos : pos - nearest) / gap;
    }
    const float2 local = outward * max(1.0 - inside / dab.radius, 0.0);
    // The inputs in the order of kInputs in mdsl.cpp.
    const Paint stamp = BrushPaint(local, dab.radius, color, dab.pressure, dab.tilt, canvasPos, dab.velocity,
                                   dab.direction, dab.random, dab.along, dab.time, dab.params);
    return float4(saturate(stamp.color), 1.0) * saturate(stamp.opacity) * saturate(stamp.amount);
}

// A brush compiled at runtime takes the build's root signature.
#ifndef MIDORI_RUNTIME
[RootSignature(STROKE_ROOT_SIGNATURE)]
#endif
[numthreads(kStrokeGroupSize, kStrokeGroupSize, 1)]
void CSMain(uint2 id : SV_DispatchThreadID) {
    const uint2 texel = rectOrigin + id;
    if (any(texel >= kTileTexels)) {
        return;
    }
    const float2 pos = float2(tileGridPos) * kTileTexels + float2(texel) + 0.5;
    const float2 canvasPos = float2(strokeOrigin) * kTileTexels + pos;

    // Each stamp pulls the paint toward its color at its opacity by its amount and never past that opacity, so a
    // constant opacity caps the stroke like Photoshop's and a varying one only ever darkens. A brush only reaches a
    // texel past its stamp's radius, where the CPU marked the tiles dirty, and saturating its result keeps a NaN out.
    float4 paint = 0.0; // premultiplied by its coverage, not yet by the stroke color's alpha
    if (shape == kStrokeLasso) {
        const float coverage = LassoMask(pos);
        paint = float4(color.rgb * coverage, coverage);
    } else if (shape == kStrokeFillRect || shape == kStrokeFillLasso) {
        paint = FillPaint(pos, canvasPos);
    } else {
        for (uint i = dabFirst; i < dabEnd; ++i) {
            const StrokeDab dab = dabs[i];
            Paint stamp;
            if (dab.width > 0.0) {
                stamp = PaintMake(color.rgb, RectMask(dab, pos), 1.0);
            } else if (dab.radius > 0.0 && distance(pos, dab.pos) < dab.radius + 1.0) {
                // The inputs in the order of kInputs in mdsl.cpp.
                stamp = BrushPaint((pos - dab.pos) / dab.radius, dab.radius, color, dab.pressure, dab.tilt, canvasPos,
                                   dab.velocity, dab.direction, dab.random, dab.along, dab.time, dab.params);
            } else {
                continue;
            }
            const float opacity = saturate(stamp.opacity);
            if (paint.a < opacity) {
                paint = lerp(paint, float4(saturate(stamp.color), 1.0) * opacity, saturate(stamp.amount));
            }
        }
    }
    if (maskPage == kStrokeMaskEmpty) {
        paint = 0.0;
    } else if (maskPage != kStrokeMaskNone) {
        paint *= tilePools[TileAtlasPageAtlas(maskPage)][uint3(texel, TileAtlasPageSlice(maskPage))].a;
    }
    const float coverage = paint.a;
    const float4 below = tilePools[TileAtlasPageAtlas(srcPage)][uint3(texel, TileAtlasPageSlice(srcPage))];
    const float4 src = paint * color.a;
    float4 result = Blend(below, src, blendMode);
    if (mode == kStrokeErase) {
        result = Blend(below, src, kBlendErase);
    } else if (mode == kStrokeReplace) {
        result = src;
    } else if (mode == kStrokeIntersect) {
        result = below * coverage;
    } else if (mode == kStrokeExclude) {
        result = below + src - 2.0 * below * src;
    }
    tilePools[TileAtlasPageAtlas(dstPage)][uint3(texel, TileAtlasPageSlice(dstPage))] = result;
}
