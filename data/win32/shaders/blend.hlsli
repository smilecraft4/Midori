#ifndef MIDORI_BLEND_HLSLI
#define MIDORI_BLEND_HLSLI

// BlendMode in color.hpp.
static const uint kBlendNormal = 0;
static const uint kBlendMultiply = 1;
static const uint kBlendAdd = 2;
static const uint kBlendScreen = 3;
static const uint kBlendNormalOkLab = 4;

float3 SrgbToLinear(float3 c) {
    return lerp(c / 12.92, pow((c + 0.055) / 1.055, 2.4), step(0.04045, c));
}

float3 LinearToSrgb(float3 c) {
    return lerp(c * 12.92, 1.055 * pow(c, 1.0 / 2.4) - 0.055, step(0.0031308, c));
}

// Matrices from https://bottosson.github.io/posts/oklab/, as in color.cpp.
float3 SrgbToOkLab(float3 srgb) {
    const float3 c = SrgbToLinear(srgb);
    const float3 lms = pow(float3(0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b,
                                  0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b,
                                  0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b),
                           1.0 / 3.0);
    return float3(0.2104542553 * lms.x + 0.7936177850 * lms.y - 0.0040720468 * lms.z,
                  1.9779984951 * lms.x - 2.4285922050 * lms.y + 0.4505937099 * lms.z,
                  0.0259040371 * lms.x + 0.7827717662 * lms.y - 0.8086757660 * lms.z);
}

float3 OkLabToSrgb(float3 lab) {
    const float3 lms = float3(lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z,
                              lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z,
                              lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z);
    const float3 lms3 = lms * lms * lms;
    // A straight line in OkLab can leave the sRGB gamut.
    const float3 c = saturate(float3(4.0767416621 * lms3.x - 3.3077115913 * lms3.y + 0.2309699292 * lms3.z,
                                     -1.2684380046 * lms3.x + 2.6097574011 * lms3.y - 0.3413193965 * lms3.z,
                                     -0.0041960863 * lms3.x - 0.7034186147 * lms3.y + 1.7076147010 * lms3.z));
    return LinearToSrgb(c);
}

// Premultiplied src over premultiplied dst, separable blend modes in their premultiplied form.
float4 Blend(float4 dst, float4 src, uint mode) {
    const float alpha = src.a + dst.a - src.a * dst.a;
    float3 color;
    switch (mode) {
        case kBlendMultiply:
            color = src.rgb * (1.0 - dst.a) + dst.rgb * (1.0 - src.a) + src.rgb * dst.rgb;
            break;
        case kBlendAdd:
            color = src.rgb + dst.rgb;
            break;
        case kBlendScreen:
            color = src.rgb + dst.rgb - src.rgb * dst.rgb;
            break;
        case kBlendNormalOkLab: {
            // Straight colors mixed by their share of the result's alpha.
            const float3 srcLab = SrgbToOkLab(src.rgb / max(src.a, 1e-6));
            const float3 dstLab = SrgbToOkLab(dst.rgb / max(dst.a, 1e-6));
            const float share = src.a / max(alpha, 1e-6);
            color = OkLabToSrgb(lerp(dstLab, srcLab, share)) * alpha;
            break;
        }
        default:
            color = src.rgb + dst.rgb * (1.0 - src.a);
            break;
    }
    return float4(color, alpha);
}

#endif
