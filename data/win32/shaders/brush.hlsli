// What a brush defines, standing in for one at build time: that pipeline only gives the stroke's root signature and
// paints the selection, never a brush stroke. At runtime the brush's translated HLSL is served under this name instead.
struct Paint {
    float3 color;
    float amount;
    float opacity;
};

Paint PaintMake(float3 color, float amount, float opacity) {
    Paint paint;
    paint.color = color;
    paint.amount = amount;
    paint.opacity = opacity;
    return paint;
}

// The inputs in the order of kInputs in mdsl.cpp.
Paint BrushPaint(float2 local, float radius, float4 color, float pressure, float2 tilt, float2 position,
                 float2 velocity, float2 direction, float random, float along, float time, float params[11]) {
    return PaintMake(color.rgb, 0.0, 0.0);
}
