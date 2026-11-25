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
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#include <json.hpp>
#include <lmdb.h>
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
    Multiply = 1,
    Add = 2,
};

enum struct LayerFlags : std::uint8_t {
    None = 0,
    Locked = 1 << 1,
    Mask = 1 << 2,
    Hidden = 1 << 6,
};

constexpr LayerFlags operator|(LayerFlags a, LayerFlags b) noexcept {
    return static_cast<LayerFlags>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}

constexpr LayerFlags& operator|=(LayerFlags& a, LayerFlags b) noexcept {
    a = a | b;
    return a;
}

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

// Target (.mori v2) TileKey layout: 25b X, 25b Y, 2b Mip, 12b Layer
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

// Legacy LMDB formats:
// 1) Raw 8-byte Tile struct (01041ed): { uint16_t layer; int16_t y; int16_t x; uint8_t pad[2]; }
struct LegacyLmdbTileStruct {
    std::uint16_t layer;
    std::int16_t y;
    std::int16_t x;
    std::uint8_t pad[2];
};
static_assert(sizeof(LegacyLmdbTileStruct) == 8);

// 2) Packed 64-bit integer TileKey (b6d99fb): 24b biased X (offset 0), 24b biased Y (offset 24),
//    14b Layer (offset 48), 2b Mip (offset 62)
constexpr std::uint64_t kLmdbPackedXBits = 24ull;
constexpr std::uint64_t kLmdbPackedXOffset = 0ull;
constexpr std::uint64_t kLmdbPackedXMask = ((1ull << kLmdbPackedXBits) - 1ull) << kLmdbPackedXOffset;
constexpr TileAxis kLmdbPackedXBias = 1 << (kLmdbPackedXBits - 1);

constexpr std::uint64_t kLmdbPackedYBits = 24ull;
constexpr std::uint64_t kLmdbPackedYOffset = kLmdbPackedXBits;
constexpr std::uint64_t kLmdbPackedYMask = ((1ull << kLmdbPackedYBits) - 1ull) << kLmdbPackedYOffset;
constexpr TileAxis kLmdbPackedYBias = 1 << (kLmdbPackedYBits - 1);

constexpr std::uint64_t kLmdbPackedLayerBits = 14ull;
constexpr std::uint64_t kLmdbPackedLayerOffset = kLmdbPackedYOffset + kLmdbPackedYBits;
constexpr std::uint64_t kLmdbPackedLayerMask = ((1ull << kLmdbPackedLayerBits) - 1ull) << kLmdbPackedLayerOffset;

constexpr std::uint64_t kLmdbPackedMipBits = 2ull;
constexpr std::uint64_t kLmdbPackedMipOffset = kLmdbPackedLayerOffset + kLmdbPackedLayerBits;
constexpr std::uint64_t kLmdbPackedMipMask = ((1ull << kLmdbPackedMipBits) - 1ull) << kLmdbPackedMipOffset;

// Legacy LayerFileEntry from src/layers.hpp (16 bytes header + trailing nameLen UTF-8 chars without null terminator)
struct LegacyLmdbLayerEntry {
    std::uint16_t id;
    std::uint16_t height;
    float opacity;
    std::uint8_t blendMode;
    bool hidden;
    bool locked;
    bool mask;
    std::uint16_t nameLen;
    std::byte pad[2];
};
static_assert(sizeof(LegacyLmdbLayerEntry) == 16);

struct LegacyLmdbVersion {
    std::uint16_t major;
    std::uint16_t minor;
    std::uint16_t patch;
};
static_assert(sizeof(LegacyLmdbVersion) == 6);

enum struct TileKeyMode {
    Auto,
    Packed,
    Struct,
};

template<typename T>
[[nodiscard]] MDBX_val Val(const T& value) noexcept {
    return MDBX_val{.iov_base = const_cast<T*>(&value), .iov_len = sizeof(T)};
}

[[nodiscard]] std::string PathUtf8(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

[[nodiscard]] std::string HexEncode(const void* data, std::size_t size) {
    static constexpr char kHex[] = "0123456789abcdef";
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

[[nodiscard]] std::string PrintableAscii(const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::string out;
    out.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        if (bytes[i] >= 32 && bytes[i] < 127) {
            out.push_back(static_cast<char>(bytes[i]));
        } else if (bytes[i] == 0) {
            out += "\\0";
        } else {
            out.push_back('.');
        }
    }
    return out;
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
    TileMip mip{0};
    std::uint64_t rawKey{};
    std::filesystem::path path;
    std::vector<std::byte> data;
};

struct LegacyLayer {
    std::uint32_t oldId{};
    std::uint32_t height{};
    float opacity{1.0f};
    BlendMode blendMode{BlendMode::Normal};
    bool hidden{};
    bool locked{};
    bool mask{};
    bool fromLayerDb{};
    std::string name;
    std::vector<std::byte> rawEntry;
    std::vector<LegacyTile> tiles;
};

struct RawRecord {
    std::vector<std::byte> key;
    std::vector<std::byte> val;
};

struct RawDatabaseDump {
    std::string name;
    bool integerKey{};
    std::vector<RawRecord> records;
};

struct LmdbInspection {
    std::string detectedTileKeyMode;
    std::optional<LegacyLmdbVersion> version;
    std::vector<RawDatabaseDump> databases;
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

[[nodiscard]] bool ValidateQoiTile(const std::byte* bytes, std::size_t size) noexcept {
    // 14-byte QOI header + 8-byte end marker minimum.
    if (size < 22 || std::memcmp(bytes, "qoif", 4) != 0) {
        return false;
    }
    const auto* data = reinterpret_cast<const unsigned char*>(bytes);
    const auto bigEndian = [data](std::size_t offset) {
        return (std::uint32_t{data[offset]} << 24) | (std::uint32_t{data[offset + 1]} << 16)
             | (std::uint32_t{data[offset + 2]} << 8) | (std::uint32_t{data[offset + 3]});
    };
    const std::uint32_t width = bigEndian(4);
    const std::uint32_t height = bigEndian(8);
    const unsigned char channels = data[12];
    return width == kTileSize && height == kTileSize && channels == 4;
}

[[nodiscard]] bool ValidateQoiTile(const std::vector<std::byte>& bytes) noexcept {
    return ValidateQoiTile(bytes.data(), bytes.size());
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

[[nodiscard]] bool WriteFileBytes(const std::filesystem::path& path, const void* data, std::size_t size) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return false;
    }
    if (size > 0) {
        stream.write( static_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    return static_cast<bool>(stream);
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
        layer.locked = layerJson.value("locked", false);
        layer.mask = layerJson.value("mask", false);
        layer.blendMode = static_cast<BlendMode>(layerJson.value("blendMode", 0u));
        layer.fromLayerDb = true;
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

struct DecodedTileKey {
    std::uint32_t layer{};
    TileMip mip{};
    TileAxis x{};
    TileAxis y{};
};

[[nodiscard]] DecodedTileKey DecodePackedLmdbKey(std::uint64_t key) noexcept {
    return DecodedTileKey{
        .layer = static_cast<std::uint32_t>((key & kLmdbPackedLayerMask) >> kLmdbPackedLayerOffset),
        .mip = static_cast<TileMip>((key & kLmdbPackedMipMask) >> kLmdbPackedMipOffset),
        .x = static_cast<TileAxis>((key & kLmdbPackedXMask) >> kLmdbPackedXOffset) - kLmdbPackedXBias,
        .y = static_cast<TileAxis>((key & kLmdbPackedYMask) >> kLmdbPackedYOffset) - kLmdbPackedYBias,
    };
}

[[nodiscard]] DecodedTileKey DecodeStructLmdbKey(std::uint64_t key) noexcept {
    LegacyLmdbTileStruct s{};
    std::memcpy(&s, &key, sizeof(s));
    return DecodedTileKey{
        .layer = s.layer,
        .mip = 0,
        .x = s.x,
        .y = s.y,
    };
}

// Heuristic score for how plausible a decoded (layer, mip, x, y) is as a canvas tile coordinate.
[[nodiscard]] int ScoreDecodedKey(const DecodedTileKey& d, const std::map<std::uint32_t, LegacyLayer>& knownLayers) {
    int score = 0;
    if (d.mip == 0) {
        score += 2;
    }
    if (d.layer < kLayerMax) {
        score += 3;
    }
    if (knownLayers.contains(d.layer)) {
        score += 4;
    }
    // Canvas tiles painted by hand are almost always near the origin (within +-4096 tiles = +-1M px).
    if (std::abs(d.x) <= 4096 && std::abs(d.y) <= 4096) {
        score += 10;
    } else if (std::abs(d.x) <= 65536 && std::abs(d.y) <= 65536) {
        score += 2;
    } else {
        score -= 10;
    }
    return score;
}

[[nodiscard]] bool LoadLegacyLmdb(const std::filesystem::path& inputPath, TileKeyMode keyMode,
                                  std::vector<LegacyLayer>& layers, LmdbInspection& inspection) {
    std::error_code ec;
    if (!std::filesystem::exists(inputPath, ec)) {
        std::fprintf(stderr, "Error: input path '%s' does not exist.\n", PathUtf8(inputPath).c_str());
        return false;
    }

    const bool isDir = std::filesystem::is_directory(inputPath, ec);
    const std::string pathStr = inputPath.string();

    MDB_env* env = nullptr;
    if (const int rc = mdb_env_create(&env); rc != MDB_SUCCESS) {
        std::fprintf(stderr, "Error: mdb_env_create failed: %s\n", mdb_strerror(rc));
        return false;
    }
    mdb_env_set_maxdbs(env, 16);
    mdb_env_set_mapsize(env, std::size_t{1} << 30);

    unsigned int envFlags = MDB_RDONLY | MDB_NOLOCK;
    if (!isDir) {
        envFlags |= MDB_NOSUBDIR;
    }
    if (const int rc = mdb_env_open(env, pathStr.c_str(), envFlags, 0644); rc != MDB_SUCCESS) {
        std::fprintf(stderr, "Error: mdb_env_open('%s') failed: %s\n", PathUtf8(inputPath).c_str(), mdb_strerror(rc));
        mdb_env_close(env);
        return false;
    }

    MDB_txn* txn = nullptr;
    if (const int rc = mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn); rc != MDB_SUCCESS) {
        std::fprintf(stderr, "Error: mdb_txn_begin failed: %s\n", mdb_strerror(rc));
        mdb_env_close(env);
        return false;
    }

    // Enumerate the unnamed database to discover all named sub-databases in the LMDB file.
    std::vector<std::string> dbNames;
    {
        MDB_dbi mainDbi = 0;
        if (mdb_dbi_open(txn, nullptr, 0, &mainDbi) == MDB_SUCCESS) {
            MDB_cursor* cur = nullptr;
            if (mdb_cursor_open(txn, mainDbi, &cur) == MDB_SUCCESS) {
                MDB_val key{};
                MDB_val val{};
                int rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
                while (rc == MDB_SUCCESS) {
                    std::string name(static_cast<const char*>(key.mv_data), key.mv_size);
                    // Remove trailing null if any
                    while (!name.empty() && name.back() == '\0') {
                        name.pop_back();
                    }
                    if (!name.empty()) {
                        dbNames.push_back(std::move(name));
                    }
                    rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
                }
                mdb_cursor_close(cur);
            }
        }
    }

    if (dbNames.empty()) {
        // Fallback to standard names if unnamed cursor didn't list them
        for (const char* candidate : {"metadata", "meta", "layers", "tiles"}) {
            dbNames.emplace_back(candidate);
        }
    }

    for (const std::string& dbName : dbNames) {
        MDB_dbi dbi = 0;
        if (mdb_dbi_open(txn, dbName.c_str(), 0, &dbi) != MDB_SUCCESS) {
            continue;
        }
        unsigned int dbFlags = 0;
        mdb_dbi_flags(txn, dbi, &dbFlags);

        RawDatabaseDump dump{};
        dump.name = dbName;
        dump.integerKey = (dbFlags & MDB_INTEGERKEY) != 0;

        MDB_cursor* cur = nullptr;
        if (mdb_cursor_open(txn, dbi, &cur) == MDB_SUCCESS) {
            MDB_val key{};
            MDB_val val{};
            int rc = mdb_cursor_get(cur, &key, &val, MDB_FIRST);
            while (rc == MDB_SUCCESS) {
                RawRecord rec{};
                const auto* kPtr = static_cast<const std::byte*>(key.mv_data);
                const auto* vPtr = static_cast<const std::byte*>(val.mv_data);
                rec.key.assign(kPtr, kPtr + key.mv_size);
                rec.val.assign(vPtr, vPtr + val.mv_size);
                dump.records.push_back(std::move(rec));
                rc = mdb_cursor_get(cur, &key, &val, MDB_NEXT);
            }
            mdb_cursor_close(cur);
        }
        inspection.databases.push_back(std::move(dump));
    }

    mdb_txn_abort(txn);
    mdb_env_close(env);

    // Parse metadata if present
    for (const RawDatabaseDump& db : inspection.databases) {
        if (db.name == "metadata" || db.name == "meta") {
            for (const RawRecord& rec : db.records) {
                if (rec.val.size() == sizeof(LegacyLmdbVersion)) {
                    LegacyLmdbVersion ver{};
                    std::memcpy(&ver, rec.val.data(), sizeof(ver));
                    inspection.version = ver;
                }
            }
        }
    }

    // Parse layers database (best-effort lossy parsing: even if mangled or missing, tiles will reconstruct layers)
    std::map<std::uint32_t, LegacyLayer> layerMap;
    for (const RawDatabaseDump& db : inspection.databases) {
        if (db.name != "layers") {
            continue;
        }
        for (const RawRecord& rec : db.records) {
            std::uint32_t keyId = 0;
            if (rec.key.size() == sizeof(std::uint64_t)) {
                std::uint64_t k64 = 0;
                std::memcpy(&k64, rec.key.data(), sizeof(k64));
                keyId = static_cast<std::uint32_t>(k64);
            } else if (rec.key.size() == sizeof(std::uint32_t)) {
                std::memcpy(&keyId, rec.key.data(), sizeof(keyId));
            } else if (rec.key.size() == sizeof(std::uint16_t)) {
                std::uint16_t k16 = 0;
                std::memcpy(&k16, rec.key.data(), sizeof(k16));
                keyId = k16;
            }

            LegacyLayer layer{};
            layer.oldId = keyId;
            layer.height = keyId;
            layer.opacity = 1.0f;
            layer.blendMode = BlendMode::Normal;
            layer.fromLayerDb = true;
            layer.rawEntry = rec.val;

            if (rec.val.size() >= sizeof(LegacyLmdbLayerEntry)) {
                LegacyLmdbLayerEntry entry{};
                std::memcpy(&entry, rec.val.data(), sizeof(entry));

                // Sanity check parsed fields so mangled memory doesn't produce NaN opacity or garbage height
                if (entry.height < kLayerMax) {
                    layer.height = entry.height;
                }
                if (std::isfinite(entry.opacity) && entry.opacity >= 0.0f && entry.opacity <= 1.0f) {
                    layer.opacity = entry.opacity;
                }
                if (entry.blendMode <= static_cast<std::uint8_t>(BlendMode::Add)) {
                    layer.blendMode = static_cast<BlendMode>(entry.blendMode);
                }
                layer.hidden = (static_cast<std::uint8_t>(entry.hidden) & 1u) != 0;
                layer.locked = (static_cast<std::uint8_t>(entry.locked) & 1u) != 0;
                layer.mask = (static_cast<std::uint8_t>(entry.mask) & 1u) != 0;

                const std::size_t availBytes = rec.val.size() - sizeof(LegacyLmdbLayerEntry);
                const std::size_t nameBytes = std::min<std::size_t>(availBytes, entry.nameLen);
                if (nameBytes > 0 && nameBytes <= 256) {
                    const auto* chars = reinterpret_cast<const char*>(rec.val.data() + sizeof(LegacyLmdbLayerEntry));
                    std::string candidate(chars, nameBytes);
                    while (!candidate.empty() && candidate.back() == '\0') {
                        candidate.pop_back();
                    }
                    // Keep only printable characters
                    bool printable = !candidate.empty();
                    for (unsigned char c : candidate) {
                        if (c < 32 && c != '\t') {
                            printable = false;
                            break;
                        }
                    }
                    if (printable) {
                        layer.name = std::move(candidate);
                    }
                }
            }

            if (layer.name.empty()) {
                layer.name = "Layer " + std::to_string(layer.oldId);
            }
            layerMap[layer.oldId] = std::move(layer);
        }
    }

    // Locate the tiles database
    const RawDatabaseDump* tilesDb = nullptr;
    for (const RawDatabaseDump& db : inspection.databases) {
        if (db.name == "tiles") {
            tilesDb = &db;
            break;
        }
    }

    if (!tilesDb) {
        std::fprintf(stderr, "Error: no 'tiles' database found in LMDB file '%s'.\n", PathUtf8(inputPath).c_str());
        return false;
    }

    // Auto-detect tile key format if set to Auto
    TileKeyMode effectiveMode = keyMode;
    if (effectiveMode == TileKeyMode::Auto) {
        long long packedScore = 0;
        long long structScore = 0;
        for (const RawRecord& rec : tilesDb->records) {
            if (rec.key.size() != sizeof(std::uint64_t)) {
                continue;
            }
            std::uint64_t rawKey = 0;
            std::memcpy(&rawKey, rec.key.data(), sizeof(rawKey));
            packedScore += ScoreDecodedKey(DecodePackedLmdbKey(rawKey), layerMap);
            structScore += ScoreDecodedKey(DecodeStructLmdbKey(rawKey), layerMap);
        }
        effectiveMode = (packedScore >= structScore) ? TileKeyMode::Packed : TileKeyMode::Struct;
        std::printf("Auto-detected LMDB tile key format: %s (packedScore=%lld, structScore=%lld)\n",
                    effectiveMode == TileKeyMode::Packed ? "packed-64bit (b6d99fb)" : "raw-struct (01041ed)",
                    packedScore, structScore);
    }
    inspection.detectedTileKeyMode = (effectiveMode == TileKeyMode::Packed) ? "packed" : "struct";

    // Parse tiles and reconstruct any missing layers
    std::size_t skippedInvalidTiles = 0;
    for (const RawRecord& rec : tilesDb->records) {
        if (rec.key.size() != sizeof(std::uint64_t)) {
            std::fprintf(stderr, "Warning: skipping tile record with %zu-byte key (expected 8).\n", rec.key.size());
            ++skippedInvalidTiles;
            continue;
        }
        std::uint64_t rawKey = 0;
        std::memcpy(&rawKey, rec.key.data(), sizeof(rawKey));

        const DecodedTileKey decoded =
            (effectiveMode == TileKeyMode::Packed) ? DecodePackedLmdbKey(rawKey) : DecodeStructLmdbKey(rawKey);

        if (decoded.mip != 0) {
            continue;
        }
        if (decoded.x < kTileCoordMin || decoded.x > kTileCoordMax || decoded.y < kTileCoordMin
            || decoded.y > kTileCoordMax) {
            std::fprintf(stderr, "Warning: skipping out-of-range tile key %#018llx -> layer=%u, x=%d, y=%d.\n",
                         static_cast<unsigned long long>(rawKey), decoded.layer, decoded.x, decoded.y);
            ++skippedInvalidTiles;
            continue;
        }
        if (!ValidateQoiTile(rec.val)) {
            std::fprintf(stderr, "Warning: skipping non-QOI or corrupt tile key %#018llx (layer=%u, x=%d, y=%d, %zu B).\n",
                         static_cast<unsigned long long>(rawKey), decoded.layer, decoded.x, decoded.y, rec.val.size());
            ++skippedInvalidTiles;
            continue;
        }

        auto it = layerMap.find(decoded.layer);
        if (it == layerMap.end()) {
            LegacyLayer reconstructed{};
            reconstructed.oldId = decoded.layer;
            reconstructed.height = decoded.layer;
            reconstructed.opacity = 1.0f;
            reconstructed.blendMode = BlendMode::Normal;
            reconstructed.fromLayerDb = false;
            reconstructed.name = "Layer " + std::to_string(decoded.layer);
            it = layerMap.emplace(decoded.layer, std::move(reconstructed)).first;
        }

        it->second.tiles.push_back(LegacyTile{
            .x = decoded.x,
            .y = decoded.y,
            .mip = decoded.mip,
            .rawKey = rawKey,
            .data = rec.val,
        });
    }

    if (skippedInvalidTiles > 0) {
        std::fprintf(stderr, "Note: skipped %zu invalid/corrupt tile record(s).\n", skippedInvalidTiles);
    }

    for (auto& [id, layer] : layerMap) {
        std::ranges::sort(layer.tiles, [](const LegacyTile& a, const LegacyTile& b) {
            return std::tie(a.y, a.x) < std::tie(b.y, b.x);
        });
        layers.push_back(std::move(layer));
    }

    if (layers.empty()) {
        // Ensure at least one empty base layer if the file had no layers or tiles
        LegacyLayer base{};
        base.oldId = 0;
        base.height = 0;
        base.opacity = 1.0f;
        base.name = "Base Layer";
        layers.push_back(std::move(base));
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

[[nodiscard]] bool DumpInspectionToDirectory(const std::filesystem::path& dumpDir,
                                             const std::vector<LegacyLayer>& layers,
                                             const LmdbInspection* inspection) {
    std::error_code ec;
    std::filesystem::create_directories(dumpDir, ec);
    if (ec) {
        std::fprintf(stderr, "Error: failed to create dump directory '%s': %s\n", PathUtf8(dumpDir).c_str(),
                     ec.message().c_str());
        return false;
    }

    nlohmann::json rootJson = nlohmann::json::object();

    // 1. Dump raw LMDB databases (metadata, layers, tiles keys/sizes/hex) if input was an LMDB file
    if (inspection) {
        rootJson["detectedTileKeyMode"] = inspection->detectedTileKeyMode;
        if (inspection->version.has_value()) {
            rootJson["parsedVersion"] = {
                {"major", inspection->version->major},
                {"minor", inspection->version->minor},
                {"patch", inspection->version->patch},
            };
        }

        nlohmann::json dbsJson = nlohmann::json::array();
        for (const RawDatabaseDump& db : inspection->databases) {
            const std::filesystem::path dbDir = dumpDir / db.name;
            if (db.name != "tiles") {
                std::filesystem::create_directories(dbDir, ec);
            }

            nlohmann::json dbJson = {
                {"name", db.name},
                {"integerKey", db.integerKey},
                {"recordCount", db.records.size()},
                {"records", nlohmann::json::array()},
            };

            for (std::size_t i = 0; i < db.records.size(); ++i) {
                const RawRecord& rec = db.records[i];
                nlohmann::json recJson = {
                    {"index", i},
                    {"keySize", rec.key.size()},
                    {"keyHex", HexEncode(rec.key.data(), rec.key.size())},
                    {"keyAscii", PrintableAscii(rec.key.data(), rec.key.size())},
                    {"valSize", rec.val.size()},
                };

                if (rec.key.size() == sizeof(std::uint64_t)) {
                    std::uint64_t k64 = 0;
                    std::memcpy(&k64, rec.key.data(), sizeof(k64));
                    recJson["keyU64"] = k64;

                    if (db.name == "tiles") {
                        const DecodedTileKey asPacked = DecodePackedLmdbKey(k64);
                        const DecodedTileKey asStruct = DecodeStructLmdbKey(k64);
                        recJson["decodedAsPacked"] = {
                            {"layer", asPacked.layer},
                            {"mip", asPacked.mip},
                            {"x", asPacked.x},
                            {"y", asPacked.y},
                        };
                        recJson["decodedAsStruct"] = {
                            {"layer", asStruct.layer},
                            {"x", asStruct.x},
                            {"y", asStruct.y},
                        };
                    }
                } else if (rec.key.size() == sizeof(std::uint32_t)) {
                    std::uint32_t k32 = 0;
                    std::memcpy(&k32, rec.key.data(), sizeof(k32));
                    recJson["keyU32"] = k32;
                }

                if (db.name != "tiles") {
                    recJson["valHex"] = HexEncode(rec.val.data(), rec.val.size());
                    recJson["valAscii"] = PrintableAscii(rec.val.data(), rec.val.size());

                    (void)WriteFileBytes(dbDir / (std::to_string(i) + "_key.bin"), rec.key.data(), rec.key.size());
                    (void)WriteFileBytes(dbDir / (std::to_string(i) + ".bin"), rec.val.data(), rec.val.size());
                    if ((db.name == "metadata" || db.name == "meta") && i == 0) {
                        (void)WriteFileBytes(dumpDir / "metadata.bin", rec.val.data(), rec.val.size());
                    }

                    if (db.name == "layers" && rec.val.size() >= sizeof(LegacyLmdbLayerEntry)) {
                        LegacyLmdbLayerEntry entry{};
                        std::memcpy(&entry, rec.val.data(), sizeof(entry));
                        recJson["decodedLayerEntry"] = {
                            {"id", entry.id},
                            {"height", entry.height},
                            {"opacity", entry.opacity},
                            {"blendMode", entry.blendMode},
                            {"hidden", static_cast<std::uint8_t>(entry.hidden)},
                            {"locked", static_cast<std::uint8_t>(entry.locked)},
                            {"mask", static_cast<std::uint8_t>(entry.mask)},
                            {"nameLen", entry.nameLen},
                            {"trailingAscii",
                             PrintableAscii(rec.val.data() + sizeof(LegacyLmdbLayerEntry),
                                            rec.val.size() - sizeof(LegacyLmdbLayerEntry))},
                        };
                    }
                } else {
                    // For tiles, include the QOI header hex preview
                    const std::size_t previewLen = std::min<std::size_t>(rec.val.size(), 22);
                    recJson["valHeaderHex"] = HexEncode(rec.val.data(), previewLen);
                    recJson["validQoi"] = ValidateQoiTile(rec.val);
                }

                dbJson["records"].push_back(std::move(recJson));
            }
            dbsJson.push_back(std::move(dbJson));
        }
        rootJson["databases"] = std::move(dbsJson);
    }

    // 2. Dump in Format 1 folder layout (<dumpDir>/<layer_id>/layer.bin + layer.json + <x>_<y>.qoi / .bin)
    nlohmann::json layersSummary = nlohmann::json::array();
    std::vector<std::byte> tileBytes;

    for (std::size_t heightIdx = 0; heightIdx < layers.size(); ++heightIdx) {
        const LegacyLayer& layer = layers[heightIdx];
        const std::filesystem::path layerDir = dumpDir / std::to_string(layer.oldId);
        std::filesystem::create_directories(layerDir, ec);

        nlohmann::json layerJson = {
            {"id", layer.oldId},
            {"height", layer.height},
            {"sortedOrder", heightIdx},
            {"opacity", layer.opacity},
            {"blendMode", static_cast<std::uint8_t>(layer.blendMode)},
            {"hidden", layer.hidden},
            {"locked", layer.locked},
            {"mask", layer.mask},
            {"fromLayerDb", layer.fromLayerDb},
            {"name", layer.name},
            {"tileCount", layer.tiles.size()},
        };

        if (!layer.rawEntry.empty()) {
            layerJson["rawEntryHex"] = HexEncode(layer.rawEntry.data(), layer.rawEntry.size());
            (void)WriteFileBytes(layerDir / "layer.bin", layer.rawEntry.data(), layer.rawEntry.size());
        }

        {
            std::ofstream layerJsonStream(layerDir / "layer.json", std::ios::binary | std::ios::trunc);
            if (layerJsonStream) {
                layerJsonStream << layerJson.dump(4) << '\n';
            }
        }

        for (const LegacyTile& tile : layer.tiles) {
            const std::byte* bytesPtr = nullptr;
            std::size_t bytesLen = 0;
            if (!tile.data.empty()) {
                bytesPtr = tile.data.data();
                bytesLen = tile.data.size();
            } else if (!tile.path.empty() && ReadFileBytes(tile.path, tileBytes)) {
                bytesPtr = tileBytes.data();
                bytesLen = tileBytes.size();
            }
            if (bytesPtr && bytesLen > 0) {
                const char* ext = ValidateQoiTile(bytesPtr, bytesLen) ? ".qoi" : ".bin";
                const std::string tileName = std::to_string(tile.x) + "_" + std::to_string(tile.y) + ext;
                (void)WriteFileBytes(layerDir / tileName, bytesPtr, bytesLen);
            }
        }

        layersSummary.push_back(std::move(layerJson));
    }

    rootJson["layers"] = std::move(layersSummary);

    {
        std::ofstream dumpJsonStream(dumpDir / "dump.json", std::ios::binary | std::ios::trunc);
        if (!dumpJsonStream) {
            std::fprintf(stderr, "Error: failed to write '%s'.\n", PathUtf8(dumpDir / "dump.json").c_str());
            return false;
        }
        dumpJsonStream << rootJson.dump(4) << '\n';
    }

    std::printf("Dumped %zu layer(s) and raw database info to '%s'\n", layers.size(), PathUtf8(dumpDir).c_str());
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
            const std::byte* bytesPtr = nullptr;
            std::size_t bytesLen = 0;
            if (!tile.data.empty()) {
                bytesPtr = tile.data.data();
                bytesLen = tile.data.size();
            } else {
                if (!ReadFileBytes(tile.path, tileBytes)) {
                    std::fprintf(stderr, "Error: unreadable QOI tile '%s'.\n", PathUtf8(tile.path).c_str());
                    mdbx_txn_abort(txn);
                    cleanup();
                    std::filesystem::remove(tempPath, ec);
                    return false;
                }
                bytesPtr = tileBytes.data();
                bytesLen = tileBytes.size();
            }

            if (!ValidateQoiTile(bytesPtr, bytesLen)) {
                std::fprintf(stderr, "Error: invalid 256x256 RGBA8 QOI tile (layer=%u, x=%d, y=%d).\n", src.oldId,
                             tile.x, tile.y);
                mdbx_txn_abort(txn);
                cleanup();
                std::filesystem::remove(tempPath, ec);
                return false;
            }

            const TileKey tileKey = TileKeyMake(newId, 0, tile.x, tile.y);
            MDBX_val tKey = Val(tileKey);
            MDBX_val tVal{.iov_base = const_cast<std::byte*>(bytesPtr), .iov_len = bytesLen};
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
        entry.blendMode = src.blendMode;
        entry.flags = LayerFlags::None;
        if (src.hidden) {
            entry.flags |= LayerFlags::Hidden;
        }
        if (src.locked) {
            entry.flags |= LayerFlags::Locked;
        }
        if (src.mask) {
            entry.flags |= LayerFlags::Mask;
        }

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

void PrintUsage() {
    std::fprintf(
        stderr,
        "Usage: midori-legacy-converter [options] <input-folder-or-lmdb> [output.mori]\n"
        "\n"
        "Options:\n"
        "  --dump <dir>             Dump metadata, layer infos, raw binary records, and QOI tiles to <dir>\n"
        "  --no-convert             Only perform --dump without writing an output .mori file\n"
        "  --key-format <mode>      LMDB tile key decoding mode: auto (default), packed (b6d99fb), struct (01041ed)\n"
        "  -h, --help               Show this help message\n");
}

[[nodiscard]] bool IsLmdbDirectory(const std::filesystem::path& dir) {
    std::error_code ec;
    return std::filesystem::is_regular_file(dir / "data.mdb", ec);
}

int RunConverter(const std::vector<std::filesystem::path>& args) {
    std::filesystem::path inputPath;
    std::filesystem::path outputPath;
    std::filesystem::path dumpDir;
    bool noConvert = false;
    TileKeyMode keyMode = TileKeyMode::Auto;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string arg = PathUtf8(args[i]);
        if (arg == "-h" || arg == "--help") {
            PrintUsage();
            return 0;
        }
        if (arg == "--dump") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "Error: --dump requires a directory argument.\n");
                return 1;
            }
            dumpDir = args[++i];
            continue;
        }
        if (arg == "--no-convert") {
            noConvert = true;
            continue;
        }
        if (arg == "--key-format") {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "Error: --key-format requires an argument (auto, packed, struct).\n");
                return 1;
            }
            const std::string modeStr = PathUtf8(args[++i]);
            if (modeStr == "auto") {
                keyMode = TileKeyMode::Auto;
            } else if (modeStr == "packed") {
                keyMode = TileKeyMode::Packed;
            } else if (modeStr == "struct") {
                keyMode = TileKeyMode::Struct;
            } else {
                std::fprintf(stderr, "Error: unknown --key-format '%s' (expected auto, packed, or struct).\n",
                             modeStr.c_str());
                return 1;
            }
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "Error: unknown option '%s'.\n", arg.c_str());
            PrintUsage();
            return 1;
        }
        if (inputPath.empty()) {
            inputPath = args[i];
        } else if (outputPath.empty()) {
            outputPath = args[i];
        } else {
            std::fprintf(stderr, "Error: unexpected positional argument '%s'.\n", arg.c_str());
            PrintUsage();
            return 1;
        }
    }

    if (inputPath.empty()) {
        PrintUsage();
        return 1;
    }

    if (outputPath.empty() && !noConvert) {
        outputPath = inputPath.filename().empty() ? inputPath.parent_path() : inputPath;
        outputPath.replace_extension(".mori");
    }

    std::error_code ec;
    if (!std::filesystem::exists(inputPath, ec)) {
        std::fprintf(stderr, "Error: input '%s' does not exist.\n", PathUtf8(inputPath).c_str());
        return 1;
    }

    std::vector<LegacyLayer> layers;
    LmdbInspection inspection{};
    const bool isFolderFormat = std::filesystem::is_directory(inputPath, ec) && !IsLmdbDirectory(inputPath);

    if (isFolderFormat) {
        if (!LoadLegacyFolder(inputPath, layers)) {
            return 1;
        }
    } else {
        if (!LoadLegacyLmdb(inputPath, keyMode, layers, inspection)) {
            return 1;
        }
    }

    if (!dumpDir.empty()) {
        if (!DumpInspectionToDirectory(dumpDir, layers, isFolderFormat ? nullptr : &inspection)) {
            return 1;
        }
    }

    if (!noConvert) {
        std::size_t totalTiles = 0;
        if (!WriteMoriFile(outputPath, layers, totalTiles)) {
            return 1;
        }
        std::printf("Converted %zu layer(s) and %zu tile(s) -> %s (format v%u)\n", layers.size(), totalTiles,
                    PathUtf8(outputPath).c_str(), kCanvasFileVersion);
    }

    return 0;
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::filesystem::path> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return RunConverter(args);
}
#else
int main(int argc, char* argv[]) {
    std::vector<std::filesystem::path> args;
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    return RunConverter(args);
}
#endif
