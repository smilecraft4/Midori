#ifndef MIDORI_COMMON_HLSLI
#define MIDORI_COMMON_HLSLI

// Root signature of the canvas pipeline, embedded in tile.vs.hlsl and created from its bytecode. The page table is a
// root SRV. One SRV per atlas; volatile, so the atlases not created yet may stay null and new ones may be written while
// a frame is in flight.
#define TILE_ROOT_SIGNATURE                                                                            \
    "RootConstants(num32BitConstants = 11, b0, visibility = SHADER_VISIBILITY_PIXEL),"                 \
    "SRV(t0, space = 1, visibility = SHADER_VISIBILITY_PIXEL),"                                        \
    "DescriptorTable(SRV(t0, numDescriptors = 16, flags = DESCRIPTORS_VOLATILE),"                      \
    " visibility = SHADER_VISIBILITY_PIXEL)"

// CanvasViewConstants in win32_graphics.cpp. A pixel center p (physical pixels of the window) lands on the canvas at
// (dot(pixelToCanvasX, p), dot(pixelToCanvasY, p)) + canvasOffset, in canvas units from the view's origin tile, so the
// numbers stay small anywhere on the canvas.
cbuffer CanvasView : register(b0) {
    float2 pixelToCanvasX;
    float2 pixelToCanvasY;
    float2 canvasOffset;
    int2 regionOrigin; // the page table's first tile, from the origin tile
    uint2 regionSize;  // tiles in the page table, row by row
    uint level;        // the view's level: its texels span 2^level canvas units
};

// The cell drawn at each tile of the region, kTileAtlasCellInvalid where nothing is.
StructuredBuffer<uint> pageTable : register(t0, space1);

#endif
