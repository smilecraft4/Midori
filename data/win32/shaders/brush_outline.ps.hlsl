#include "brush_outline.hlsli"

// Distance to the crosshair's four arms, which start a gap away from the center so it stays open.
float CrossDistance(float2 offset) {
    const float gap = kBrushOutlineCrossGap * lineWidth;
    const float end = gap + kBrushOutlineCrossArm * lineWidth;
    const float2 a = abs(offset);
    const float alongX = max(max(gap - a.x, a.x - end), 0.0);
    const float alongY = max(max(gap - a.y, a.y - end), 0.0);
    return min(length(float2(alongX, a.y)), length(float2(a.x, alongY)));
}

// Coverage of the outline ring, or of a crosshair for a brush too small to circle. The pipeline blends it as an
// inversion of what is below: c * (1 - dst) + dst * (1 - c).
float4 PSMain(float4 position : SV_Position) : SV_Target {
    const float2 offset = position.xy - center;
    const float d = radius < kBrushOutlineCrossRadius ? CrossDistance(offset) : abs(length(offset) - radius);
    const float coverage = saturate(lineWidth * 0.5 + 0.5 - d);
    if (coverage <= 0.0) {
        discard;
    }
    return float4(coverage.xxx, 0.0);
}
