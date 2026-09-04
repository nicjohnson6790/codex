#pragma once

#include "Position.hpp"
#include "WorldGridQuadtreeTypes.hpp"
#include "assets/RuntimeHeightmapFormat.hpp"

#include <glm/vec2.hpp>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <vector>

using HeightmapDatasetId = std::uint64_t;
using SourceHeightmapId = std::uint64_t;

struct SourceHeightmap
{
    SourceHeightmapId sourceHeightmapId = 0;
    HeightmapDatasetId datasetId = 0;
    Position basePosition{};
    glm::dvec2 xTileAxis{static_cast<double>(Position::kCellSize), 0.0};
    double yScale = 1.0;
    glm::dvec2 zTileAxis{0.0, static_cast<double>(Position::kCellSize)};
    double maxContributionPitch = std::numeric_limits<double>::infinity();
};

struct SourceTileId
{
    HeightmapDatasetId datasetId = 0;
    std::int32_t tileX = 0;
    std::int32_t tileY = 0;
    friend bool operator==(const SourceTileId &, const SourceTileId &) = default;
};

class HeightmapDataset
{
  public:
    struct TileRange
    {
        std::int32_t minX = 0, minY = 0, maxX = -1, maxY = -1;
    };
    enum class SampleEncoding
    {
        Float32Meters,
        SignedInt16Meters
    };
    virtual ~HeightmapDataset() = default;
    [[nodiscard]] virtual HeightmapDatasetId datasetId() const = 0;
    [[nodiscard]] virtual SampleEncoding sampleEncoding() const = 0;
    [[nodiscard]] virtual TileRange tileRange() const = 0;
    [[nodiscard]] virtual bool containsTile(std::int32_t tileX, std::int32_t tileY) const = 0;
    [[nodiscard]] virtual std::uint64_t tileRevision(std::int32_t tileX, std::int32_t tileY) const = 0;
    virtual bool loadTile(std::int32_t tileX, std::int32_t tileY, std::vector<float> &samples, std::string &error) const = 0;
};

class EtopoHeightmapDataset final : public HeightmapDataset
{
  public:
    static std::shared_ptr<EtopoHeightmapDataset> open(const std::filesystem::path &indexPath, std::string &error,
                                                       HeightmapDatasetId datasetId = 0x45544f504f323032ULL);

    [[nodiscard]] HeightmapDatasetId datasetId() const override
    {
        return m_datasetId;
    }
    [[nodiscard]] SampleEncoding sampleEncoding() const override
    {
        return SampleEncoding::SignedInt16Meters;
    }
    [[nodiscard]] TileRange tileRange() const override
    {
        return m_tileRange;
    }
    [[nodiscard]] bool containsTile(std::int32_t tileX, std::int32_t tileY) const override;
    [[nodiscard]] std::uint64_t tileRevision(std::int32_t tileX, std::int32_t tileY) const override;
    bool loadTile(std::int32_t tileX, std::int32_t tileY, std::vector<float> &samples, std::string &error) const override;

    [[nodiscard]] const RuntimeAssets::HeightmapPackHeader &header() const
    {
        return m_header;
    }

  private:
    const RuntimeAssets::HeightmapTileRecord *findRecord(std::int32_t tileX, std::int32_t tileY) const;

    HeightmapDatasetId m_datasetId = 0x45544f504f323032ULL; // "ETOPO202"
    RuntimeAssets::HeightmapPackHeader m_header{};
    std::vector<RuntimeAssets::HeightmapTileRecord> m_records;
    TileRange m_tileRange{};
    std::filesystem::path m_dataPath;
};

struct SourceTileCoordinate
{
    std::int32_t x = 0, y = 0;
    friend bool operator==(const SourceTileCoordinate &, const SourceTileCoordinate &) = default;
};

[[nodiscard]] glm::dvec2 positionOffsetXZ(std::int64_t gridX, std::int64_t gridY, const glm::dvec2 &localXZ, const Position &origin);

[[nodiscard]] std::vector<SourceTileCoordinate> collectOverlappingSourceTiles(const SourceHeightmap &source,
                                                                              const HeightmapDataset &dataset,
                                                                              const WorldGridQuadtreeLeafId &finalHeightmap);
