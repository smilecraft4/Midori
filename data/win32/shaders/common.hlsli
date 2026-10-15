#ifndef MIDORI_COMMON_HLSLI
#define MIDORI_COMMON_HLSLI

// Vertex-to-pixel interface shared by every pipeline that only interpolates a color.
struct ColorVertex {
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};

#endif
