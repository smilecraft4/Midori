#include "common.hlsli"

float4 PSMain(ColorVertex input) : SV_TARGET {
    return input.color;
}
