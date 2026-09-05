#include "HeightmapDataset.hpp"
#include "HeightmapQuantization.hpp"
#include <memory>
#include "assets/RuntimeAssetCompression.hpp"

#include <cstring>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace
{
class GridDataset final : public HeightmapDataset
{
  public:
    HeightmapDatasetId datasetId() const override
    {
        return 99;
    }
    SampleEncoding sampleEncoding() const override
    {
        return SampleEncoding::Float32Meters;
    }
    TileRange tileRange() const override
    {
        return {-2, -2, 2, 2};
    }
    bool containsTile(std::int32_t x, std::int32_t y) const override
    {
        return x >= -2 && x <= 2 && y >= -2 && y <= 2;
    }
    std::uint64_t tileRevision(std::int32_t, std::int32_t) const override
    {
        return 7;
    }
    bool loadTile(std::int32_t, std::int32_t, std::vector<float> &samples, std::string &) const override
    {
        samples.assign(RuntimeAssets::kHeightmapTileSampleCount, 1.0f);
        return true;
    }
};

WorldGridQuadtreeLeafId minimumLeaf()
{
    WorldGridQuadtreeLeafId leaf{};
    for (int depth = 0; depth < 11; ++depth)
        leaf.subdivisionPath = WorldGridQuadtreeLeafId::appendChild(leaf.subdivisionPath, 3);
    return leaf;
}
// Optional offline integration check of every generated payload through the
// production reader, independently compared with the converter's filter decoder.
bool validateGeneratedPacks(const std::filesystem::path& directory)
{
    for (const char* stem : {"etopo2022", "japan_dem10_delta_sw", "japan_dem10_delta_se",
                            "japan_dem10_delta_ce", "japan_dem10_delta_ne"})
    {
        const auto path = directory / (std::string(stem) + ".assetbin");
        std::string error;
        const auto dataset = EtopoHeightmapDataset::open(path, error);
        if (!dataset) { std::cerr << error << '\n'; return false; }
        std::ifstream index(path, std::ios::binary);
        RuntimeAssets::HeightmapPackHeader header{};
        index.read(reinterpret_cast<char*>(&header), sizeof(header));
        index.seekg(static_cast<std::streamoff>(header.tileRecordOffset));
        std::ifstream data(directory / header.dataFilename.data(), std::ios::binary);
        std::uint64_t zeroEdges = 0;
        for (std::uint32_t tile = 0; tile < header.tileCount; ++tile)
        {
            RuntimeAssets::HeightmapTileRecord record{};
            index.read(reinterpret_cast<char*>(&record), sizeof(record));
            std::vector<float> samples;
            if (!index || !dataset->loadTile(record.tileX, record.tileY, samples, error))
            { std::cerr << error; return false; }
            std::vector<std::byte> compressed(record.compressedSize), filtered;
            data.seekg(static_cast<std::streamoff>(record.blobOffset));
            data.read(reinterpret_cast<char*>(compressed.data()), record.compressedSize);
            std::vector<std::int16_t> encoded;
            if (!data || !RuntimeAssets::DecompressBytes(RuntimeAssets::CompressionType::Lz4,
                    compressed, record.uncompressedSize, &filtered, &error) ||
                !HeightmapTileFilter::Decode(filtered, &encoded, &error)) return false;
            std::uint32_t valid = 0;
            for (std::size_t i = 0; i < encoded.size(); ++i)
            {
                const auto code = encoded[i];
                valid += code != RuntimeAssets::kHeightmapInvalidHeight;
                const float expected = code <= RuntimeAssets::kHeightmapExactZeroHeight ? 0.0f :
                    record.sampleBias + float(code) * record.sampleScale;
                if (!std::isfinite(samples[i]) || samples[i] != expected) return false;
                if (std::string_view(stem) != "etopo2022" &&
                    ((record.tileX == 127 && i % 256 == 255) || (record.tileY == 127 && i / 256 == 255)))
                {
                    if (code != RuntimeAssets::kHeightmapExactZeroHeight || samples[i] != 0.0f) return false;
                    ++zeroEdges;
                }
            }
            if (valid != record.validSampleCount) return false;
        }
        std::cout << stem << ": " << header.tileCount << " runtime tiles verified; "
            << zeroEdges << " exact-zero positive-edge samples\n";
    }
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2) return validateGeneratedPacks(argv[1]) ? 0 : 40;
    std::string quantizationError;
    if (!HeightmapQuantization::SelfTest(&quantizationError))
    { std::cerr << quantizationError; return 30; }
    GridDataset grid;
    SourceHeightmap placement{1, grid.datasetId(), Position{}, {256.0, 0.0}, 2.0, {0.0, 256.0}};
    const auto overlap = collectOverlappingSourceTiles(placement, grid, minimumLeaf());
    if (overlap.size() != 9 || overlap.front() != SourceTileCoordinate{-1, -1} || overlap.back() != SourceTileCoordinate{1, 1})
        return 10;
    placement.xTileAxis = {0.0, 256.0};
    placement.zTileAxis = {-256.0, 0.0};
    if (collectOverlappingSourceTiles(placement, grid, minimumLeaf()).size() != 9)
        return 11;
    auto coarseLeaf = minimumLeaf();
    coarseLeaf.subdivisionPath >>= 18u;
    placement.maxContributionPitch = 32.0;
    if (!collectOverlappingSourceTiles(placement, grid, coarseLeaf).empty())
        return 14;
    auto cutoffLeaf = minimumLeaf();
    cutoffLeaf.subdivisionPath >>= 15u;
    if (collectOverlappingSourceTiles(placement, grid, cutoffLeaf).empty())
        return 15;
    placement.maxContributionPitch = std::numeric_limits<double>::infinity();
    auto farLeaf = minimumLeaf();
    farLeaf.gridX = std::numeric_limits<std::int64_t>::max();
    farLeaf.gridY = std::numeric_limits<std::int64_t>::min();
    if (!collectOverlappingSourceTiles(placement, grid, farLeaf).empty())
        return 12;
    const Position extreme(std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::min(), {100.0, 0.0, 200.0});
    const glm::dvec2 localOffset = positionOffsetXZ(extreme.gridX(), extreme.gridY(), {101.0, 202.0}, extreme);
    if (localOffset != glm::dvec2(1.0, 2.0))
        return 13;

    const std::filesystem::path fixtureDirectory = std::filesystem::temp_directory_path() / "codex_heightmap_dataset_test";
    std::filesystem::remove_all(fixtureDirectory);
    std::filesystem::create_directories(fixtureDirectory);
    std::vector<std::byte> filtered(RuntimeAssets::kHeightmapFilteredTileBytes, std::byte{});
    auto encoded = std::make_unique<HeightmapQuantization::EncodedTile>();
    encoded->fill(123);
    (*encoded)[0] = RuntimeAssets::kHeightmapExactZeroHeight;
    (*encoded)[1] = RuntimeAssets::kHeightmapInvalidHeight;
    (*encoded)[2] = -32766; (*encoded)[3] = 32767;
    (*encoded)[256] = RuntimeAssets::kHeightmapExactZeroHeight;
    (*encoded)[511] = RuntimeAssets::kHeightmapInvalidHeight;
    if (!HeightmapTileFilter::Encode(*encoded, &filtered, &quantizationError)) return 31;
    std::vector<std::byte> compressed;
    std::string error;
    if (!RuntimeAssets::CompressBytes(RuntimeAssets::CompressionType::Lz4, filtered, &compressed, &error))
        return 20;
    RuntimeAssets::HeightmapPackHeader header{};
    header.magic = RuntimeAssets::kHeightmapPackMagic;
    header.version = RuntimeAssets::kHeightmapFormatVersion;
    header.headerSize = sizeof(header);
    header.tileCount = 1;
    header.tileResolution = RuntimeAssets::kHeightmapTileResolution;
    header.tileStride = RuntimeAssets::kHeightmapTileStride;
    header.sampleType = static_cast<std::uint32_t>(RuntimeAssets::HeightSampleType::QuantizedInt16ScaleBias);
    header.invalidHeight = RuntimeAssets::kHeightmapInvalidHeight;
    header.filterType = static_cast<std::uint32_t>(RuntimeAssets::HeightFilterType::SerpentineDeltaZigZagBytePlanes);
    header.compressionType = static_cast<std::uint32_t>(RuntimeAssets::CompressionType::Lz4);
    header.tileRecordOffset = sizeof(header);
    std::memcpy(header.dataFilename.data(), "fixture.heightbin", sizeof("fixture.heightbin"));
    RuntimeAssets::HeightmapTileRecord record{};
    record.tileX = -1;
    record.tileY = 2;
    record.compressedSize = static_cast<std::uint32_t>(compressed.size());
    record.uncompressedSize = RuntimeAssets::kHeightmapFilteredTileBytes;
    record.blobOffset = 0;
    record.sampleScale = 0.125f;
    record.sampleBias = -8.25f;
    {
        std::ofstream index(fixtureDirectory / "fixture.assetbin", std::ios::binary);
        index.write(reinterpret_cast<const char *>(&header), sizeof(header));
        index.write(reinterpret_cast<const char *>(&record), sizeof(record));
        std::ofstream data(fixtureDirectory / "fixture.heightbin", std::ios::binary);
        data.write(reinterpret_cast<const char *>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
    }
    const auto dataset = EtopoHeightmapDataset::open(fixtureDirectory / "fixture.assetbin", error);
    if (!dataset)
    {
        std::cerr << error << '\n';
        return 1;
    }
    if (!dataset->containsTile(-1, 2) || dataset->tileRange().minX != -1 || dataset->tileRange().maxY != 2)
        return 21;
    std::vector<float> samples;
    if (!dataset->loadTile(-1, 2, samples, error))
        return 22;
    if (samples.size() != RuntimeAssets::kHeightmapTileSampleCount)
        return 23;
    for (std::size_t i = 0; i < samples.size(); ++i)
    {
        const auto code = (*encoded)[i];
        const float expected = code == RuntimeAssets::kHeightmapInvalidHeight || code == RuntimeAssets::kHeightmapExactZeroHeight
            ? 0.0f : -8.25f + float(code) * 0.125f;
        if (!std::isfinite(samples[i]) || samples[i] != expected) return 24;
    }
    header.version = 2;
    {
        std::fstream index(fixtureDirectory / "fixture.assetbin", std::ios::binary | std::ios::in | std::ios::out);
        index.write(reinterpret_cast<const char*>(&header), sizeof(header));
    }
    if (EtopoHeightmapDataset::open(fixtureDirectory / "fixture.assetbin", error) ||
        error.find("regenerate") == std::string::npos) return 32;

    std::filesystem::remove_all(fixtureDirectory);
    return 0;
}
