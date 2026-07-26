#pragma once

#include <cstdint>

namespace Midori {

enum class Error : uint8_t {
    None,

    OutOfRange,
    OutOfCapacity,
    OutOfMemory,
    TileNotFound,
    LayerNotFound,
    FileNotFound,
    MissingTexture,
    UploadSlotMissing,
    DownloadSlotMissing,

    Unknown,
};

}
