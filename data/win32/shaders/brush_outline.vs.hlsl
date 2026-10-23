#include "brush_outline.hlsli"

// The quad around the outline, the crosshair and a pixel of antialiasing, as a 4-vertex strip.
[RootSignature(BRUSH_OUTLINE_ROOT_SIGNATURE)]
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    const float crossExtent = (kBrushOutlineCrossGap + kBrushOutlineCrossArm) * lineWidth;
    const float extent = max(radius, crossExtent) + lineWidth + 1.0;
    const float2 pixel = center + (float2(id & 1, id >> 1) * 2.0 - 1.0) * extent;
    return float4(pixel / targetSize * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
