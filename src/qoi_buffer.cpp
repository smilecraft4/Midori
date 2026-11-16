#include "midori/tile.hpp"

#include <cstddef>
#include <vector>

namespace Midori {

// QOI's worst case for a tile: 5 bytes a texel, plus its header and end marker. Decoding needs less, 4 a texel.
constexpr std::size_t kQoiBytesMax = (std::size_t{kTileSize} * kTileSize * 5) + 14 + 8;

} // namespace Midori

// QOI_MALLOC in qoi.c, apart from the canvas so the tests and benchmark encode tiles without it. A thread encodes or
// decodes one tile at a time, so each thread reuses one buffer, made on its first call; QOI_FREE does nothing. A stream
// stays valid until the thread's next encode or decode.
extern "C" void* MidoriQoiBuffer(std::size_t size) noexcept {
    thread_local std::vector<std::byte> buffer(Midori::kQoiBytesMax);
    return size <= buffer.size() ? buffer.data() : nullptr;
}
