#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <json.hpp>
#include <mdbx.h>

namespace {

using Layer = std::uint16_t;
using TileAxis = std::int32_t;
using TileMip = std::uint8_t;
using TileKey = std::uint64_t;

constexpr Layer kLayerMax = 0b111111111111 + 1; // 4096
constexpr std::size_t kLayerNameLenMax = 64;
constexpr std::uint32_t kTileSize = 256;

// Format v2: QOI-compressed 256x256 premultiplied RGBA8 tiles, flat layersOrder without v3's internal root folder
// (CanvasOpen automatically wraps v2 layers in the root folder upon opening).
constexpr std::uint32_t kCanvasFileMagic = 0x4F44494D; // "MIDO"
constexpr std::uint32_t kCanvasFileVersion = 2;

enum struct PixelFormat : std::uint8_t {
    RGBA8 = 0,
};

enum struct BlendMode : std::uint8_t {
    Normal = 0,
};

enum struct LayerFlags : std::uint8_t {
    None = 0,
    Hidden = 1 << 6,
};

enum struct CanvasFileMeta : std::uint32_t {
    Header = 0,
    LayersOrder = 1,
};

struct CanvasFileHeader {
    std::uint32_t magic;
    std::uint32_t version;
    Layer largestLayer;
    PixelFormat pixelFormat;
    std::uint8_t reserved;
};
static_assert(sizeof(CanvasFileHeader) == 12);

struct LayerFileEntry {
    std::array<char, kLayerNameLenMax> name;
    float opacity;
    std::uint32_t contentGeneration;
    std::uint32_t tileCount;
    Layer descendants;
    BlendMode blendMode;
    LayerFlags flags;
};
static_assert(sizeof(LayerFileEntry) == kLayerNameLenMax + 16);

constexpr std::size_t kTileXBitSize = 25;
constexpr std::size_t kTileXBitOffset = 0;
constexpr std::size_t kTileYBitSize = 25;
constexpr std::size_t kTileYBitOffset = kTileXBitOffset + kTileXBitSize;
constexpr std::size_t kTileMipBitSize = 2;
constexpr std::size_t kTileMipBitOffset = kTileYBitOffset + kTileYBitSize;
constexpr std::size_t kTileLayerBitSize = 12;
constexpr std::size_t kTileLayerBitOffset = kTileMipBitOffset + kTileMipBitSize;
static_assert((kTileLayerBitOffset + kTileLayerBitSize) <= sizeof(TileKey) * 8);

constexpr TileAxis kTileCoordMin = -(1 << 24);
constexpr TileAxis kTileCoordMax = (1 << 24) - 1;

constexpr TileKey TileKeyMake(Layer layer, TileMip mip, TileAxis x, TileAxis y) noexcept {
    return ((static_cast<TileKey>(static_cast<std::uint32_t>(x) & ((TileKey{1} << kTileXBitSize) - 1))
             ^ (TileKey{1} << (kTileXBitSize - 1)))
            << kTileXBitOffset)
         | ((static_cast<TileKey>(static_cast<std::uint32_t>(y) & ((TileKey{1} << kTileYBitSize) - 1))
             ^ (TileKey{1} << (kTileYBitSize - 1)))
            << kTileYBitOffset)
         | (static_cast<TileKey>(mip) << kTileMipBitOffset)
         | (static_cast<TileKey>(layer) << kTileLayerBitOffset);
}

template<typename T>
[[nodiscard]] MDBX_val Val(const T& value) noexcept {
    return MDBX_val{.iov_base = const_cast<T*>(&value), .iov_len = sizeof(T)};
}

[[nodiscard]] std::string PathUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

[[nodiscard]] int EnvOpen(MDBX_env* env, const std::filesystem::path& path, MDBX_env_flags_t flags,
                          mdbx_mode_t mode) noexcept {
#if defined(_WIN32)
    return mdbx_env_openW(env, path.c_str(), flags, mode);
#else
    return mdbx_env_open(env, path.c_str(), flags, mode);
#endif
}

struct LegacyTile {
    TileAxis x{};
    TileAxis y{};
    std::filesystem::path path;
};

struct LegacyLayer {
    std::uint32_t oldId{};
    std::uint32_t height{};
    float opacity{1.0f};
    bool hidden{};
    std::string name;
    std::vector<LegacyTile> tiles;
};

[[nodiscard]] bool ParseInt(std::string_view text, TileAxis& out) noexcept {
    if (text.empty()) {
        return false;
    }
    TileAxis value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) {
        return false;
    }
    if (value < kTileCoordMin || value > kTileCoordMax) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] bool ParseTileFilename(std::string_view stem, TileAxis& x, TileAxis& y) noexcept {
    const std::size_t sep = stem.find('_');
    if (sep == std::string_view::npos || stem.find('_', sep + 1) != std::string_view::npos) {
        return false;
    }
    return ParseInt(stem.substr(0, sep), x) && ParseInt(stem.substr(sep + 1), y);
}

[[nodiscard]] bool ValidateQoiTile(const std::vector<std::byte>& bytes) noexcept {
    // 14-byte QOI header + 8-byte end marker minimum.
    if (bytes.size() < 22 || std::memcmp(bytes.data(), "qoif", 4) != 0) {
        return false;
    }
    const auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    const auto bigEndian = [data](std::size_t offset) {
        return (std::uint32_t{data[offset]} << 24) | (std::uint32_t{data[offset + 1]} << 16)
             | (std::uint32_t{data[offset + 2]} << 8) | (std::uint32_t{data[offset + 3]});
    };
    const std::uint32_t width = bigEndian(4);
    const std::uint32_t height = bigEndian(8);
    const unsigned char channels = data[12];
    return width == kTileSize && height == kTileSize && channels == 4;
}

[[nodiscard]] bool ReadFileBytes(const std::filesystem::path& path, std::vector<std::byte>& out) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return false;
    }
    const std::streamsize size = stream.tellg();
    if (size <= 0) {
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    return static_cast<bool>(stream.read(reinterpret_cast<char*>(out.data()), size));
}

[[nodiscard]] bool LoadLegacyFolder(const std::filesystem::path& inputDir, std::vector<LegacyLayer>& layers) {
    std::error_code ec;
    if (!std::filesystem::exists(inputDir, ec) || !std::filesystem::is_directory(inputDir, ec)) {
        std::fprintf(stderr, "Error: input directory '%s' does not exist.\n", PathUtf8(inputDir).c_str());
        return false;
    }

    for (std::filesystem::directory_iterator it(inputDir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) {
            continue;
        }
        const std::filesystem::path layerDir = it->path();
        const std::filesystem::path jsonPath = layerDir / "layer.json";
        if (!std::filesystem::is_regular_file(jsonPath, ec)) {
            continue;
        }

        std::ifstream jsonFile(jsonPath);
        if (!jsonFile) {
            std::fprintf(stderr, "Error: failed to open '%s'.\n", PathUtf8(jsonPath).c_str());
            return false;
        }

        nlohmann::json layerJson = nlohmann::json::parse(jsonFile, nullptr, false);
        if (layerJson.is_discarded() || !layerJson.is_object()) {
            std::fprintf(stderr, "Error: invalid JSON in '%s'.\n", PathUtf8(jsonPath).c_str());
            return false;
        }

        LegacyLayer layer{};
        layer.oldId = layerJson.value("id", 0u);
        layer.height = layerJson.value("height", 0u);
        layer.opacity = std::clamp(layerJson.value("opacity", 1.0f), 0.0f, 1.0f);
        layer.hidden = layerJson.value("hidden", false);
        layer.name = layerJson.value("name", std::string{});
        if (layer.name.empty()) {
            layer.name = "Layer " + std::to_string(layer.oldId);
        }

        for (std::filesystem::directory_iterator tileIt(layerDir, ec), tileEnd; !ec && tileIt != tileEnd;
             tileIt.increment(ec)) {
            if (!tileIt->is_regular_file(ec) || tileIt->path().extension() != ".qoi") {
                continue;
            }
            const std::string stem = PathUtf8(tileIt->path().stem());
            TileAxis x = 0;
            TileAxis y = 0;
            if (!ParseTileFilename(stem, x, y)) {
                std::fprintf(stderr, "Warning: skipping unexpected tile file '%s'.\n",
                             PathUtf8(tileIt->path()).c_str());
                continue;
            }
            layer.tiles.push_back(LegacyTile{.x = x, .y = y, .path = tileIt->path()});
        }

        std::ranges::sort(layer.tiles, [](const LegacyTile& a, const LegacyTile& b) {
            return std::tie(a.y, a.x) < std::tie(b.y, b.x);
        });
        layers.push_back(std::move(layer));
    }

    if (layers.empty()) {
        std::fprintf(stderr, "Error: no layer sub-folders with 'layer.json' found in '%s'.\n",
                     PathUtf8(inputDir).c_str());
        return false;
    }

    std::ranges::sort(layers, [](const LegacyLayer& a, const LegacyLayer& b) {
        return std::tie(a.height, a.oldId) < std::tie(b.height, b.oldId);
    });

    if (layers.size() >= kLayerMax) {
        std::fprintf(stderr, "Error: canvas has %zu layers, exceeding maximum (%u).\n", layers.size(),
                     static_cast<unsigned>(kLayerMax - 1));
        return false;
    }
    return true;
}

[[nodiscard]] bool WriteMoriFile(const std::filesystem::path& outputPath, const std::vector<LegacyLayer>& layers,
                                 std::size_t& totalTilesOut) {
    constexpr intptr_t kFileGrowth = intptr_t{64} << 20;
    constexpr intptr_t kFileSizeMax = intptr_t{1} << 40;

    std::error_code ec;
    if (outputPath.has_parent_path()) {
        std::filesystem::create_directories(outputPath.parent_path(), ec);
    }

    std::filesystem::path tempPath = outputPath;
    tempPath += ".tmp";
    std::filesystem::path tempLockPath = tempPath;
    tempLockPath += MDBX_LOCK_SUFFIX;
    std::filesystem::remove(tempPath, ec);
    std::filesystem::remove(tempLockPath, ec);

    MDBX_env* env = nullptr;
    const auto cleanup = [&]() {
        if (env) {
            mdbx_env_close(env);
            env = nullptr;
        }
        std::filesystem::remove(tempLockPath, ec);
    };

    if (const int rc = mdbx_env_create(&env); rc != MDBX_SUCCESS) {
        std::fprintf(stderr, "Error: mdbx_env_create failed: %s\n", mdbx_strerror(rc));
        return false;
    }
    if (const int rc = mdbx_env_set_maxdbs(env, 3); rc != MDBX_SUCCESS) {
        std::fprintf(stderr, "Error: mdbx_env_set_maxdbs failed: %s\n", mdbx_strerror(rc));
        cleanup();
        return false;
    }
    if (const int rc = mdbx_env_set_geometry(env, -1, -1, kFileSizeMax, kFileGrowth, -1, -1); rc != MDBX_SUCCESS) {
        std::fprintf(stderr, "Error: mdbx_env_set_geometry failed: %s\n", mdbx_strerror(rc));
        cleanup();
        return false;
    }
    if (const int rc = EnvOpen(env, tempPath, MDBX_NOSUBDIR | MDBX_EXCLUSIVE, 0644); rc != MDBX_SUCCESS) {
        std::fprintf(stderr, "Error: mdbx_env_open('%s') failed: %s\n", PathUtf8(tempPath).c_str(), mdbx_strerror(rc));
        cleanup();
        return false;
    }

    MDBX_txn* txn = nullptr;
    if (const int rc = mdbx_txn_begin(env, nullptr, MDBX_TXN_READWRITE, &txn); rc != MDBX_SUCCESS) {
        std::fprintf(stderr, "Error: mdbx_txn_begin failed: %s\n", mdbx_strerror(rc));
        cleanup();
        std::filesystem::remove(tempPath, ec);
        return false;
    }

    const auto failTxn = [&](const char* what, int rc) {
        std::fprintf(stderr, "Error: %s failed: %s\n", what, mdbx_strerror(rc));
        mdbx_txn_abort(txn);
        cleanup();
        std::filesystem::remove(tempPath, ec);
        return false;
    };

    MDBX_dbi metaDbi = 0;
    MDBX_dbi layersDbi = 0;
    MDBX_dbi tilesDbi = 0;
    constexpr MDBX_db_flags_t kDbFlags = MDBX_INTEGERKEY | MDBX_CREATE;
    if (const int rc = mdbx_dbi_open(txn, "meta", kDbFlags, &metaDbi); rc != MDBX_SUCCESS) {
        return failTxn("mdbx_dbi_open(meta)", rc);
    }
    if (const int rc = mdbx_dbi_open(txn, "layers", kDbFlags, &layersDbi); rc != MDBX_SUCCESS) {
        return failTxn("mdbx_dbi_open(layers)", rc);
    }
    if (const int rc = mdbx_dbi_open(txn, "tiles", kDbFlags, &tilesDbi); rc != MDBX_SUCCESS) {
        return failTxn("mdbx_dbi_open(tiles)", rc);
    }

    const Layer layerCount = static_cast<Layer>(layers.size());
    const CanvasFileHeader header{
        .magic = kCanvasFileMagic,
        .version = kCanvasFileVersion,
        .largestLayer = layerCount,
        .pixelFormat = PixelFormat::RGBA8,
        .reserved = 0,
    };
    const auto headerKey = static_cast<std::uint32_t>(CanvasFileMeta::Header);
    MDBX_val key = Val(headerKey);
    MDBX_val val = Val(header);
    if (const int rc = mdbx_put(txn, metaDbi, &key, &val, MDBX_UPSERT); rc != MDBX_SUCCESS) {
        return failTxn("mdbx_put(header)", rc);
    }

    std::vector<Layer> layersOrder(layerCount);
    for (Layer i = 0; i < layerCount; ++i) {
        layersOrder[i] = i;
    }
    const auto orderKey = static_cast<std::uint32_t>(CanvasFileMeta::LayersOrder);
    key = Val(orderKey);
    val = MDBX_val{.iov_base = layersOrder.data(), .iov_len = layersOrder.size() * sizeof(Layer)};
    if (const int rc = mdbx_put(txn, metaDbi, &key, &val, MDBX_UPSERT); rc != MDBX_SUCCESS) {
        return failTxn("mdbx_put(layersOrder)", rc);
    }

    std::vector<std::byte> tileBytes;
    totalTilesOut = 0;

    for (Layer newId = 0; newId < layerCount; ++newId) {
        const LegacyLayer& src = layers[newId];
        std::uint32_t writtenTiles = 0;

        for (const LegacyTile& tile : src.tiles) {
            if (!ReadFileBytes(tile.path, tileBytes) || !ValidateQoiTile(tileBytes)) {
                std::fprintf(stderr, "Error: invalid or unreadable 256x256 RGBA8 QOI tile '%s'.\n",
                             PathUtf8(tile.path).c_str());
                mdbx_txn_abort(txn);
                cleanup();
                std::filesystem::remove(tempPath, ec);
                return false;
            }
            const TileKey tileKey = TileKeyMake(newId, 0, tile.x, tile.y);
            MDBX_val tKey = Val(tileKey);
            MDBX_val tVal{.iov_base = tileBytes.data(), .iov_len = tileBytes.size()};
            if (const int rc = mdbx_put(txn, tilesDbi, &tKey, &tVal, MDBX_UPSERT); rc != MDBX_SUCCESS) {
                return failTxn("mdbx_put(tile)", rc);
            }
            ++writtenTiles;
            ++totalTilesOut;
        }

        LayerFileEntry entry{};
        const std::size_t copyLen = std::min(src.name.size(), kLayerNameLenMax - 1);
        std::memcpy(entry.name.data(), src.name.data(), copyLen);
        entry.name[copyLen] = '\0';
        entry.opacity = src.opacity;
        entry.contentGeneration = 0;
        entry.tileCount = writtenTiles;
        entry.descendants = 0;
        entry.blendMode = BlendMode::Normal;
        entry.flags = src.hidden ? LayerFlags::Hidden : LayerFlags::None;

        const std::uint32_t layerKey = newId;
        MDBX_val lKey = Val(layerKey);
        MDBX_val lVal = Val(entry);
        if (const int rc = mdbx_put(txn, layersDbi, &lKey, &lVal, MDBX_UPSERT); rc != MDBX_SUCCESS) {
            return failTxn("mdbx_put(layer)", rc);
        }
    }

    if (const int rc = mdbx_txn_commit(txn); rc != MDBX_SUCCESS) {
        std::fprintf(stderr, "Error: mdbx_txn_commit failed: %s\n", mdbx_strerror(rc));
        cleanup();
        std::filesystem::remove(tempPath, ec);
        return false;
    }

    cleanup();

    std::filesystem::path outLockPath = outputPath;
    outLockPath += MDBX_LOCK_SUFFIX;
    std::filesystem::remove(outputPath, ec);
    std::filesystem::remove(outLockPath, ec);
    std::filesystem::rename(tempPath, outputPath, ec);
    if (ec) {
        std::fprintf(stderr, "Error: failed to move '%s' to '%s': %s\n", PathUtf8(tempPath).c_str(),
                     PathUtf8(outputPath).c_str(), ec.message().c_str());
        std::filesystem::remove(tempPath, ec);
        return false;
    }
    return true;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2 || argc > 3 || std::wcscmp(argv[1], L"-h") == 0 || std::wcscmp(argv[1], L"--help") == 0) {
        std::fprintf(stderr, "Usage: midori-legacy-converter <legacy-folder> [output.mori]\n");
        return argc >= 2 && (std::wcscmp(argv[1], L"-h") == 0 || std::wcscmp(argv[1], L"--help") == 0) ? 0 : 1;
    }
    const std::filesystem::path inputDir(argv[1]);
    std::filesystem::path outputPath;
    if (argc == 3) {
        outputPath = std::filesystem::path(argv[2]);
    } else {
        outputPath = inputDir.filename().empty() ? inputDir.parent_path() : inputDir;
        outputPath.replace_extension(".mori");
    }
#else
int main(int argc, char* argv[]) {
    if (argc < 2 || argc > 3 || std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0) {
        std::fprintf(stderr, "Usage: midori-legacy-converter <legacy-folder> [output.mori]\n");
        return argc >= 2 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0) ? 0 : 1;
    }
    const std::filesystem::path inputDir(argv[1]);
    std::filesystem::path outputPath;
    if (argc == 3) {
        outputPath = std::filesystem::path(argv[2]);
    } else {
        outputPath = inputDir.filename().empty() ? inputDir.parent_path() : inputDir;
        outputPath.replace_extension(".mori");
    }
#endif

    std::vector<LegacyLayer> layers;
    if (!LoadLegacyFolder(inputDir, layers)) {
        return 1;
    }

    std::size_t totalTiles = 0;
    if (!WriteMoriFile(outputPath, layers, totalTiles)) {
        return 1;
    }

    std::printf("Converted %zu layer(s) and %zu tile(s) -> %s (format v%u)\n", layers.size(), totalTiles,
                PathUtf8(outputPath).c_str(), kCanvasFileVersion);
    return 0;
}
