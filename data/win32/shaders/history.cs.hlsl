#include "atlas.hlsli"

// Copies 32^2 blocks between level-0 atlas cells and the history store, one group per block. One UAV per atlas,
// volatile like the tile pipeline's SRVs.
#define HISTORY_ROOT_SIGNATURE                                                                         \
    "SRV(t0),"                                                                                         \
    "UAV(u0, space = 1),"                                                                              \
    "DescriptorTable(UAV(u0, numDescriptors = 16, flags = DESCRIPTORS_VOLATILE))"

static const uint kHistoryGroupSize = 8;
static const uint kHistoryBlockBytes = kTileAtlasBlockSize * kTileAtlasBlockSize * 4;

// HistoryGpuOp in win32_graphics.cpp.
struct HistoryOp {
    uint slot;
    uint page;
    uint origin; // texel origin of the block in its page, x | y << 16
    uint swap;   // 0: the block goes into the slot; 1: they trade places
};

StructuredBuffer<HistoryOp> ops : register(t0);
RWByteAddressBuffer store : register(u0, space1); // RGBA8 rows of kTileAtlasBlockSize texels per slot
RWTexture2DArray<float4> tilePools[kTileAtlasMax] : register(u0);

// The atlas is UNORM8, so both directions are exact.
uint Pack(float4 color) {
    const uint4 bytes = uint4(round(saturate(color) * 255.0));
    return bytes.r | (bytes.g << 8) | (bytes.b << 16) | (bytes.a << 24);
}

float4 Unpack(uint bits) {
    return float4(bits & 0xFF, (bits >> 8) & 0xFF, (bits >> 16) & 0xFF, bits >> 24) / 255.0;
}

[RootSignature(HISTORY_ROOT_SIGNATURE)]
[numthreads(kHistoryGroupSize, kHistoryGroupSize, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    const HistoryOp op = ops[id.z];
    const uint3 texel = uint3(uint2(op.origin & 0xFFFF, op.origin >> 16) + id.xy, TileAtlasPageSlice(op.page));
    const uint address = op.slot * kHistoryBlockBytes + ((id.y * kTileAtlasBlockSize) + id.x) * 4;
    const uint atlas = TileAtlasPageAtlas(op.page);
    const float4 tile = tilePools[atlas][texel];
    if (op.swap != 0) {
        tilePools[atlas][texel] = Unpack(store.Load(address));
    }
    store.Store(address, Pack(tile));
}
