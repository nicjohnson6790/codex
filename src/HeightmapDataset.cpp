#include "HeightmapDataset.hpp"

#include "assets/RuntimeAssetCompression.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <fstream>
#include <limits>

namespace
{
constexpr std::uint16_t zigZagDecode(std::uint16_t folded)
{
    const std::uint16_t magnitude = static_cast<std::uint16_t>(folded >> 1u);
    return (folded & 1u) != 0 ? static_cast<std::uint16_t>(~magnitude) : magnitude;
}
} // namespace

glm::dvec2 positionOffsetXZ(std::int64_t gridX, std::int64_t gridY, const glm::dvec2 &localXZ, const Position &origin)
{
    auto axis = [](std::int64_t grid, double local, std::int64_t originGrid, double originLocal) {
        auto magnitude = [](std::int64_t value) -> std::uint64_t {
            return value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1u : static_cast<std::uint64_t>(value);
        };
        std::uint64_t cells = 0;
        if ((grid < 0) == (originGrid < 0))
        {
            const std::uint64_t a = magnitude(grid), b = magnitude(originGrid);
            cells = a >= b ? a - b : b - a;
        }
        else
            cells = magnitude(grid) + magnitude(originGrid);
        return (grid >= originGrid ? std::ldexp(static_cast<double>(cells), 19) : -std::ldexp(static_cast<double>(cells), 19)) + local -
               originLocal;
    };
    return {
        axis(gridX, localXZ.x, origin.gridX(), origin.localPosition().x),
        axis(gridY, localXZ.y, origin.gridY(), origin.localPosition().z),
    };
}

std::vector<SourceTileCoordinate> collectOverlappingSourceTiles(const SourceHeightmap &source, const HeightmapDataset &dataset,
                                                                const WorldGridQuadtreeLeafId &finalHeightmap)
{
    double minX, minZ, size;
    worldGridQuadtreeLeafExtents(finalHeightmap, minX, minZ, size);
    const double pitch = size / AppConfig::Terrain::kHeightmapLeafIntervalCount;
    minX -= pitch * AppConfig::Terrain::kHeightmapLeafHalo;
    minZ -= pitch * AppConfig::Terrain::kHeightmapLeafHalo;
    const double maxX = minX + pitch * (AppConfig::Terrain::kHeightmapResolution - 1);
    const double maxZ = minZ + pitch * (AppConfig::Terrain::kHeightmapResolution - 1);
    const double determinant = source.xTileAxis.x * source.zTileAxis.y - source.xTileAxis.y * source.zTileAxis.x;
    if (std::abs(determinant) < 1e-12)
        return {};
    double tileMinX = std::numeric_limits<double>::max(), tileMinY = tileMinX;
    double tileMaxX = -tileMinX, tileMaxY = -tileMinX;
    for (const glm::dvec2 corner : std::array<glm::dvec2, 4>{{{minX, minZ}, {maxX, minZ}, {minX, maxZ}, {maxX, maxZ}}})
    {
        const glm::dvec2 relative = positionOffsetXZ(finalHeightmap.gridX, finalHeightmap.gridY, corner, source.basePosition);
        const double x = (relative.x * source.zTileAxis.y - relative.y * source.zTileAxis.x) / determinant;
        const double y = (source.xTileAxis.x * relative.y - source.xTileAxis.y * relative.x) / determinant;
        tileMinX = std::min(tileMinX, x);
        tileMaxX = std::max(tileMaxX, x);
        tileMinY = std::min(tileMinY, y);
        tileMaxY = std::max(tileMaxY, y);
    }
    const auto range = dataset.tileRange();
    if (tileMaxX < range.minX || tileMinX > static_cast<double>(range.maxX) + 1.0 || tileMaxY < range.minY ||
        tileMinY > static_cast<double>(range.maxY) + 1.0)
        return {};
    auto owner = [](double coordinate) { return static_cast<std::int32_t>(std::ceil(coordinate) - 1.0); };
    const auto firstX = std::max(range.minX, owner(std::max(tileMinX, static_cast<double>(range.minX))));
    const auto firstY = std::max(range.minY, owner(std::max(tileMinY, static_cast<double>(range.minY))));
    const auto lastX =
        std::min(range.maxX, static_cast<std::int32_t>(std::floor(std::min(tileMaxX, static_cast<double>(range.maxX) + 1.0))));
    const auto lastY =
        std::min(range.maxY, static_cast<std::int32_t>(std::floor(std::min(tileMaxY, static_cast<double>(range.maxY) + 1.0))));
    std::vector<SourceTileCoordinate> result;
    for (std::int32_t y = firstY; y <= lastY; ++y)
        for (std::int32_t x = firstX; x <= lastX; ++x)
            if (dataset.containsTile(x, y))
                result.push_back({x, y});
    return result;
}

namespace
{
constexpr std::size_t rasterIndex(std::size_t traversalIndex)
{
    const std::size_t y = traversalIndex / RuntimeAssets::kHeightmapTileResolution;
    const std::size_t inRow = traversalIndex % RuntimeAssets::kHeightmapTileResolution;
    const std::size_t x = (y & 1u) == 0u ? inRow : RuntimeAssets::kHeightmapTileResolution - 1u - inRow;
    return y * RuntimeAssets::kHeightmapTileResolution + x;
}
} // namespace

std::shared_ptr<EtopoHeightmapDataset> EtopoHeightmapDataset::open(const std::filesystem::path &indexPath, std::string &error)
{
    std::ifstream input(indexPath, std::ios::binary);
    auto result = std::shared_ptr<EtopoHeightmapDataset>(new EtopoHeightmapDataset());
    if (!input.read(reinterpret_cast<char *>(&result->m_header), sizeof(result->m_header)))
    {
        error = "could not read ETOPO heightmap header: " + indexPath.string();
        return {};
    }
    const auto &h = result->m_header;
    if (h.magic != RuntimeAssets::kHeightmapPackMagic || h.version != RuntimeAssets::kHeightmapFormatVersion ||
        h.tileResolution != RuntimeAssets::kHeightmapTileResolution || h.tileStride != RuntimeAssets::kHeightmapTileStride ||
        h.sampleType != static_cast<std::uint32_t>(RuntimeAssets::HeightSampleType::SignedInt16Meters) ||
        h.filterType != static_cast<std::uint32_t>(RuntimeAssets::HeightFilterType::SerpentineDeltaZigZagBytePlanes) ||
        h.compressionType != static_cast<std::uint32_t>(RuntimeAssets::CompressionType::Lz4))
    {
        error = "unsupported ETOPO runtime heightmap format";
        return {};
    }
    result->m_records.resize(h.tileCount);
    input.seekg(static_cast<std::streamoff>(h.tileRecordOffset));
    if (!input.read(reinterpret_cast<char *>(result->m_records.data()),
                    static_cast<std::streamsize>(result->m_records.size() * sizeof(result->m_records.front()))))
    {
        error = "could not read ETOPO tile index";
        return {};
    }
    if (!result->m_records.empty())
    {
        result->m_tileRange = {
            result->m_records.front().tileX,
            result->m_records.front().tileY,
            result->m_records.front().tileX,
            result->m_records.front().tileY,
        };
        for (const auto &record : result->m_records)
        {
            result->m_tileRange.minX = std::min<std::int32_t>(result->m_tileRange.minX, record.tileX);
            result->m_tileRange.minY = std::min<std::int32_t>(result->m_tileRange.minY, record.tileY);
            result->m_tileRange.maxX = std::max<std::int32_t>(result->m_tileRange.maxX, record.tileX);
            result->m_tileRange.maxY = std::max<std::int32_t>(result->m_tileRange.maxY, record.tileY);
        }
    }
    result->m_dataPath = indexPath.parent_path() / h.dataFilename.data();
    if (!std::filesystem::exists(result->m_dataPath))
    {
        error = "ETOPO tile data file is missing: " + result->m_dataPath.string();
        return {};
    }
    return result;
}

const RuntimeAssets::HeightmapTileRecord *EtopoHeightmapDataset::findRecord(std::int32_t tileX, std::int32_t tileY) const
{
    const auto it = std::lower_bound(
        m_records.begin(), m_records.end(), std::pair{tileY, tileX}, [](const RuntimeAssets::HeightmapTileRecord &record, const auto &key) {
            return std::pair{static_cast<std::int32_t>(record.tileY), static_cast<std::int32_t>(record.tileX)} < key;
        });
    return it != m_records.end() && it->tileX == tileX && it->tileY == tileY ? &*it : nullptr;
}

bool EtopoHeightmapDataset::containsTile(std::int32_t tileX, std::int32_t tileY) const
{
    return findRecord(tileX, tileY) != nullptr;
}

std::uint64_t EtopoHeightmapDataset::tileRevision(std::int32_t tileX, std::int32_t tileY) const
{
    return containsTile(tileX, tileY) ? 1 : 0;
}

bool EtopoHeightmapDataset::loadTile(std::int32_t tileX, std::int32_t tileY, std::vector<float> &samples, std::string &error) const
{
    const auto *record = findRecord(tileX, tileY);
    if (!record)
    {
        error = "requested ETOPO tile is not indexed";
        return false;
    }
    std::ifstream input(m_dataPath, std::ios::binary);
    std::vector<std::byte> compressed(record->compressedSize);
    input.seekg(static_cast<std::streamoff>(record->blobOffset));
    if (!input.read(reinterpret_cast<char *>(compressed.data()), static_cast<std::streamsize>(compressed.size())))
    {
        error = "could not read ETOPO tile blob";
        return false;
    }
    std::vector<std::byte> filtered;
    if (!RuntimeAssets::DecompressBytes(static_cast<RuntimeAssets::CompressionType>(m_header.compressionType), compressed,
                                        record->uncompressedSize, &filtered, &error))
        return false;
    if (filtered.size() != RuntimeAssets::kHeightmapFilteredTileBytes)
    {
        error = "ETOPO tile has invalid filtered size";
        return false;
    }
    samples.assign(RuntimeAssets::kHeightmapTileSampleCount, 0.0f);
    std::uint16_t previous = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(filtered[0])) |
                             static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(filtered[1]) << 8u);
    samples[rasterIndex(0)] = previous == std::bit_cast<std::uint16_t>(RuntimeAssets::kHeightmapInvalidHeight)
                                  ? 0.0f
                                  : static_cast<float>(std::bit_cast<std::int16_t>(previous));
    constexpr std::size_t low = 2;
    constexpr std::size_t high = 2 + RuntimeAssets::kHeightmapTileSampleCount - 1;
    for (std::size_t i = 1; i < RuntimeAssets::kHeightmapTileSampleCount; ++i)
    {
        const std::uint16_t folded = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(filtered[low + i - 1])) |
                                     static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(filtered[high + i - 1]) << 8u);
        previous = static_cast<std::uint16_t>(previous + zigZagDecode(folded));
        samples[rasterIndex(i)] = previous == std::bit_cast<std::uint16_t>(RuntimeAssets::kHeightmapInvalidHeight)
                                      ? 0.0f
                                      : static_cast<float>(std::bit_cast<std::int16_t>(previous));
    }
    return true;
}
