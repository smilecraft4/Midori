#ifndef MIDORI_BRUSH_OUTLINE_HLSLI
#define MIDORI_BRUSH_OUTLINE_HLSLI

// Root signature of the brush outline pipeline, embedded in brush_outline.vs.hlsl.
#define BRUSH_OUTLINE_ROOT_SIGNATURE "RootConstants(num32BitConstants = 6, b0)"

// Below this radius, in pixels, the brush shows as a crosshair instead: four arms of kBrushOutlineCrossArm line widths,
// kBrushOutlineCrossGap line widths away from the center.
static const float kBrushOutlineCrossRadius = 4.0;
static const float kBrushOutlineCrossGap = 2.0;
static const float kBrushOutlineCrossArm = 3.0;

// BrushOutlineConstants in win32_graphics.cpp. Pixels of the window's client area, physical.
cbuffer BrushOutline : register(b0) {
    float2 center;
    float2 targetSize;
    float radius;
    float lineWidth;
};

#endif
