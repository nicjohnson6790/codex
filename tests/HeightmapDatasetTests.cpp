#include "HeightmapDataset.hpp"
#include "HeightmapQuantization.hpp"
#include "HeightmapTileWorkers.hpp"
#include "HeightmapReindex.hpp"
#include <chrono>
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
bool loadForComparison(const HeightmapDataset& dataset, int x, int y, std::vector<float>& samples, std::string& error);

bool validateParallelConversion(const std::filesystem::path& directory, std::string& error)
{
    const auto hardware = std::max(1u, std::thread::hardware_concurrency());
    const std::size_t tileCount = std::max<std::size_t>(64, hardware * 2);
    std::vector<HeightmapTileWorkers::Coordinate> coordinates;
    for (std::size_t i = 0; i < tileCount; ++i)
        coordinates.emplace_back(int(i / 256) - 128, int(i % 256) - 128);
    struct Scratch
    {
        HeightmapQuantization::EncodedTile tile{};
        std::vector<std::byte> filtered, compressed, restored;
        std::vector<std::int16_t> decoded;
        std::size_t worker = 0;
    };
    std::atomic_size_t created{0};
    std::vector<std::size_t> uses(std::min<std::size_t>(hardware, tileCount));
    std::vector<RuntimeAssets::HeightmapTileRecord> records(RuntimeAssets::kHeightmapTileTableCount);
    std::ofstream data(directory / "parallel.heightbin", std::ios::binary);
    std::mutex appendMutex;
    std::promise<void> secondAppended;
    auto second = secondAppended.get_future().share();
    const auto generate = [](auto& tile, std::size_t index) {
        for (std::size_t i = 0; i < tile.size(); ++i)
            tile[i] = static_cast<std::int16_t>(static_cast<int>((i * (index + 1)) % 50000) - 25000);
        tile[0] = RuntimeAssets::kHeightmapInvalidHeight;
        tile[1] = RuntimeAssets::kHeightmapExactZeroHeight;
    };
    if (!HeightmapTileWorkers::Run(coordinates, "parallel fixture", [&] {
        auto scratch = std::make_unique<Scratch>();
        scratch->worker = created.fetch_add(1);
        return scratch;
    }, [&](std::size_t index, int tx, int ty, Scratch& scratch) {
        ++uses[scratch.worker];
        if (index % 7 == 6) return; // Preserve sparse canonical identity.
        generate(scratch.tile, index);
        std::string failure;
        if (!HeightmapQuantization::RoundTrip(scratch.tile, scratch.filtered, scratch.compressed,
                scratch.restored, scratch.decoded, &failure)) throw std::runtime_error(failure);
        if (index == 0)
        {
            // Independent reference call proves the existing API/level is used
            // even for ordinary tiles that also fit with the fast encoder.
            std::vector<std::byte> reference(LZ4_compressBound(int(scratch.filtered.size())));
            const int size = LZ4_compress_HC(reinterpret_cast<const char*>(scratch.filtered.data()),
                reinterpret_cast<char*>(reference.data()), int(scratch.filtered.size()), int(reference.size()), LZ4HC_CLEVEL_MAX);
            reference.resize(size);
            if (reference != scratch.compressed) throw std::runtime_error("HC policy mismatch");
            if (hardware > 1 && second.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
                throw std::runtime_error("tile work did not execute concurrently");
        }
        std::lock_guard lock(appendMutex);
        const auto offset = static_cast<std::uint64_t>(data.tellp());
        data.write(reinterpret_cast<const char*>(scratch.compressed.data()), scratch.compressed.size());
        if (!data) throw std::runtime_error("fixture append failed");
        records[index] = {static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty), 0,
            static_cast<std::uint32_t>(scratch.compressed.size()), RuntimeAssets::kHeightmapFilteredTileBytes,
            RuntimeAssets::kHeightmapTileSampleCount - 1, 0.125f, 18.25f, offset};
        if (index == 1) secondAppended.set_value();
    }, &error)) return false;
    data.close();
    if (created != uses.size() || std::none_of(uses.begin(), uses.end(), [](auto n) { return n > 1; }) ||
        (hardware > 1 && records[0].blobOffset == 0)) return false;
    RuntimeAssets::HeightmapPackHeader header{};
    header.magic = RuntimeAssets::kHeightmapPackMagic; header.version = RuntimeAssets::kHeightmapFormatVersion;
    header.headerSize = sizeof(header); header.tileRecordOffset = sizeof(header);
    header.tileCount = std::count_if(records.begin(), records.end(), [](const auto& r) { return r.compressedSize != 0; });
    header.fileSize = RuntimeAssets::kHeightmapIndexBytes;
    header.tileResolution = 256; header.tileStride = 255;
    header.sampleType = static_cast<std::uint32_t>(RuntimeAssets::HeightSampleType::QuantizedInt16ScaleBias);
    header.filterType = static_cast<std::uint32_t>(RuntimeAssets::HeightFilterType::SerpentineDeltaZigZagBytePlanes);
    header.compressionType = static_cast<std::uint32_t>(RuntimeAssets::CompressionType::Lz4);
    std::memcpy(header.dataFilename.data(), "parallel.heightbin", sizeof("parallel.heightbin"));
    {
        std::ofstream index(directory / "parallel.assetbin", std::ios::binary);
        index.write(reinterpret_cast<const char*>(&header), sizeof(header));
        index.write(reinterpret_cast<const char*>(records.data()), records.size() * sizeof(records[0]));
        if (!index) return false;
    }
    const auto dataset = EtopoHeightmapDataset::open(directory / "parallel.assetbin", error);
    if (!dataset) return false;
    auto expected = std::make_unique<HeightmapQuantization::EncodedTile>();
    // Reverse reads exercise physical offsets independently of canonical order.
    for (std::size_t index = tileCount; index-- > 0;)
    {
        const auto [ty, tx] = coordinates[index];
        if (index % 7 == 6) { if (dataset->containsTile(tx, ty)) return false; continue; }
        std::vector<float> samples;
        if (!loadForComparison(*dataset, tx, ty, samples, error)) return false;
        generate(*expected, index);
        for (std::size_t i = 0; i < samples.size(); ++i)
            if (samples[i] != (i < 2 ? 0.0f : 18.25f + float((*expected)[i]) * 0.125f)) return false;
    }
    // Exceptions must be joined, returned, and retain dataset and tile context.
    if (HeightmapTileWorkers::Run(std::span(coordinates).first(1), "failure fixture",
        [] { return std::make_unique<int>(0); },
        [](std::size_t, int, int, int&) { throw std::runtime_error("injected source read failure"); }, &error) ||
        error.find("failure fixture tile (-128,-128)") == std::string::npos ||
        error.find("injected source read failure") == std::string::npos) return false;
    error.clear();
    return true;
}

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
    bool loadTile(std::int32_t, std::int32_t, std::span<std::byte>, std::span<std::byte>, TileQuantization &, std::string &) const override
    {
        return false; // Geometry-only dataset.
    }
};

// Emulate packed shader sampling independently of the production reconstruction.
bool loadForComparison(const HeightmapDataset &dataset, int x, int y, std::vector<float> &samples, std::string &error)
{
    std::vector<std::byte> input(RuntimeAssets::kHeightmapFilteredTileBytes), filtered(input.size());
    std::vector<std::int16_t> raster(RuntimeAssets::kHeightmapTileSampleCount);
    HeightmapDataset::TileQuantization q;
    if (!dataset.loadTile(x, y, input, filtered, q, error)) return false;
    HeightmapDataset::reconstructTile(filtered, raster);
    samples.resize(raster.size());
    for (std::size_t i = 0; i < raster.size(); ++i)
    {
        std::uint32_t packed;
        std::memcpy(&packed, raster.data() + (i / 2) * 2, sizeof(packed));
        const auto bits = static_cast<std::uint16_t>(packed >> ((i % 2) * 16));
        int code = bits >= 32768 ? int(bits) - 65536 : int(bits);
        if (code == -32768) return false;
        samples[i] = code == 0 ? 0.0f : q.bias + float(code == -32767 ? 0 : code) * q.scale;
    }
    return true;
}

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
        std::uint64_t zeroEdges = 0, previousOffset = 0, reordered = 0;
        std::uint32_t maxCompressed = 0;
        for (std::uint32_t tile = 0; tile < RuntimeAssets::kHeightmapTileTableCount; ++tile)
        {
            RuntimeAssets::HeightmapTileRecord record{};
            index.read(reinterpret_cast<char*>(&record), sizeof(record));
            if (!index) return false;
            if (!record.compressedSize) continue;
            if (record.compressedSize > RuntimeAssets::kHeightmapFilteredTileBytes) return false;
            maxCompressed = std::max(maxCompressed, record.compressedSize);
            if (tile && record.blobOffset < previousOffset) ++reordered;
            previousOffset = record.blobOffset;
            std::vector<float> samples;
            if (!index || !loadForComparison(*dataset, record.tileX, record.tileY, samples, error))
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
            << zeroEdges << " exact-zero positive-edge samples; maximum compressed size " << maxCompressed
            << "; " << reordered << " physical-order inversions\n";
    }
    return true;
}

bool compareLogicalPacks(const std::filesystem::path& first, const std::filesystem::path& second)
{
    std::ifstream a(first, std::ios::binary), b(second, std::ios::binary);
    RuntimeAssets::HeightmapPackHeader ha{}, hb{};
    a.read(reinterpret_cast<char*>(&ha), sizeof(ha)); b.read(reinterpret_cast<char*>(&hb), sizeof(hb));
    if (!a || !b || ha.tileCount != hb.tileCount || (ha.version != 3 && ha.version != 4) || (hb.version != 3 && hb.version != 4)) return false;
    std::ifstream da(first.parent_path() / ha.dataFilename.data(), std::ios::binary);
    std::ifstream db(second.parent_path() / hb.dataFilename.data(), std::ios::binary);
    auto readTable = [](auto& input, const auto& header) {
        std::vector<RuntimeAssets::HeightmapTileRecord> table(RuntimeAssets::kHeightmapTileTableCount);
        input.seekg(header.tileRecordOffset);
        if (header.version == 3)
        {
            for (std::uint32_t i = 0; i < header.tileCount; ++i)
            {
                RuntimeAssets::HeightmapTileRecord record{};
                input.read(reinterpret_cast<char*>(&record), sizeof(record));
                table[RuntimeAssets::HeightmapTileTableIndex(record.tileX, record.tileY)] = record;
            }
        }
        else input.read(reinterpret_cast<char*>(table.data()), RuntimeAssets::kHeightmapTileTableBytes);
        return table;
    };
    const auto ta = readTable(a, ha), tb = readTable(b, hb);
    std::vector<std::byte> ca, cb, fa, fb;
    std::string error;
    for (std::uint32_t i = 0; i < RuntimeAssets::kHeightmapTileTableCount; ++i)
    {
        const auto& ra = ta[i]; const auto& rb = tb[i];
        if (!a || !b || bool(ra.compressedSize) != bool(rb.compressedSize)) return false;
        if (!ra.compressedSize) continue;
        if (ra.tileX != rb.tileX || ra.tileY != rb.tileY || ra.sampleScale != rb.sampleScale ||
            ra.sampleBias != rb.sampleBias || ra.validSampleCount != rb.validSampleCount || ra.uncompressedSize != rb.uncompressedSize)
            return false;
        ca.resize(ra.compressedSize); cb.resize(rb.compressedSize);
        da.seekg(ra.blobOffset); db.seekg(rb.blobOffset);
        da.read(reinterpret_cast<char*>(ca.data()), ca.size()); db.read(reinterpret_cast<char*>(cb.data()), cb.size());
        if (!da || !db || !RuntimeAssets::DecompressBytes(RuntimeAssets::CompressionType::Lz4, ca, ra.uncompressedSize, &fa, &error) ||
            !RuntimeAssets::DecompressBytes(RuntimeAssets::CompressionType::Lz4, cb, rb.uncompressedSize, &fb, &error) || fa != fb)
            return false;
    }
    std::cout << first.filename() << ": " << ha.tileCount << " logically identical tiles (metadata and filtered bytes)\n";
    return true;
}

bool validateSpatialTables(const std::filesystem::path& directory, RuntimeAssets::HeightmapPackHeader header,
                           const RuntimeAssets::HeightmapTileRecord& sample)
{
    using namespace RuntimeAssets;
    const auto path = directory / "spatial.assetbin";
    std::vector<HeightmapTileRecord> table(kHeightmapTileTableCount);
    const std::array<HeightmapTileWorkers::Coordinate, 7> positions{{
        {-128,-128}, {-128,127}, {127,-128}, {127,127}, {0,-1}, {0,0}, {-1,0}}};
    for (auto [y,x] : positions)
    {
        auto record = sample; record.tileX = static_cast<std::int8_t>(x); record.tileY = static_cast<std::int8_t>(y);
        table[HeightmapTileTableIndex(x,y)] = record;
    }
    header.tileCount = positions.size();
    auto write = [&] {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(table.data()), kHeightmapTileTableBytes);
    };
    write();
    std::string error;
    const auto dataset = EtopoHeightmapDataset::open(path,error);
    if (!dataset || std::filesystem::file_size(path) != kHeightmapIndexBytes ||
        dataset->tileRange().minX != -128 || dataset->tileRange().maxX != 127 ||
        dataset->tileRange().minY != -128 || dataset->tileRange().maxY != 127) return false;
    for (int y = -128; y <= 127; ++y)
        for (int x = -128; x <= 127; ++x)
        {
            const bool present = std::find(positions.begin(), positions.end(), std::pair{y,x}) != positions.end();
            if (dataset->containsTile(x,y) != present || dataset->tileRevision(x,y) != (present ? 1 : 0)) return false;
        }
    for (int outside : {INT32_MIN, -129, 128, INT32_MAX})
        if (dataset->containsTile(outside,0) || dataset->containsTile(0,outside)) return false;
    for (auto [y,x] : positions)
    {
        std::vector<float> samples;
        if (!loadForComparison(*dataset,x,y,samples,error)) return false; // offset zero is present
    }
    // Mismatched coordinates, inconsistent counts, dirty absent slots, and
    // truncated/oversized tables must fail at open rather than alias neighbors.
    table[0].tileX = 0; write();
    if (EtopoHeightmapDataset::open(path,error)) return false;
    table[0].tileX = -128; ++header.tileCount; write();
    if (EtopoHeightmapDataset::open(path,error)) return false;
    --header.tileCount; table[1].blobOffset = 1; write();
    if (EtopoHeightmapDataset::open(path,error)) return false;
    table[1] = {}; write();
    std::filesystem::resize_file(path,kHeightmapIndexBytes - 1);
    if (EtopoHeightmapDataset::open(path,error)) return false;
    write(); std::filesystem::resize_file(path,kHeightmapIndexBytes + 1);
    if (EtopoHeightmapDataset::open(path,error)) return false;
    std::fill(table.begin(),table.end(),HeightmapTileRecord{}); header.tileCount = 0; write();
    const auto empty = EtopoHeightmapDataset::open(path,error);
    if (!empty || empty->tileRange().maxX >= empty->tileRange().minX || empty->containsTile(0,0)) return false;

    // Real v3 -> v4 migration preserves logical samples and the original index.
    const auto legacy = directory / "legacy.assetbin";
    header.version = 3; header.tileCount = 1; header.fileSize = sizeof(header) + sizeof(sample);
    {
        std::ofstream out(legacy,std::ios::binary);
        out.write(reinterpret_cast<const char*>(&header),sizeof(header));
        out.write(reinterpret_cast<const char*>(&sample),sizeof(sample));
    }
    if (EtopoHeightmapDataset::open(legacy,error)) return false;
    ReindexHeightmap(legacy);
    const auto backup = std::filesystem::path(legacy.string() + ".before-spatial");
    if (!compareLogicalPacks(backup,legacy) || !EtopoHeightmapDataset::open(legacy,error)) return false;
    ReindexHeightmap(legacy); // Already-v4 migration is idempotent.
    return true;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 4 && std::string_view(argv[1]) == "--compare") return compareLogicalPacks(argv[2], argv[3]) ? 0 : 41;
    if (argc == 2) return validateGeneratedPacks(argv[1]) ? 0 : 40;
    std::string quantizationError;
    if (!HeightmapQuantization::SelfTest(&quantizationError))
    { std::cerr << quantizationError; return 30; }
    GridDataset grid;
    // Truly incompressible filtered data must fail export, never create a tile
    // that requires overflow storage in the runtime loader.
    {
        std::vector<std::byte> noise(RuntimeAssets::kHeightmapFilteredTileBytes), compressedNoise;
        std::uint32_t state = 0x918af731u;
        for (auto &value : noise)
        {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            value = static_cast<std::byte>(state & 255u);
        }
        if (HeightmapQuantization::CompressFiltered(noise, compressedNoise, &quantizationError) || !compressedNoise.empty()) return 34;
    }
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
    if (!validateParallelConversion(fixtureDirectory, quantizationError))
    { std::cerr << quantizationError; return 35; }
    std::vector<std::byte> filtered(RuntimeAssets::kHeightmapFilteredTileBytes, std::byte{});
    auto encoded = std::make_unique<HeightmapQuantization::EncodedTile>();
    for (std::size_t i = 0; i < encoded->size(); ++i)
        (*encoded)[i] = static_cast<std::int16_t>(static_cast<int>(i) - 32768);
    (*encoded)[0] = RuntimeAssets::kHeightmapExactZeroHeight;
    (*encoded)[1] = RuntimeAssets::kHeightmapInvalidHeight;
    (*encoded)[2] = -32766; (*encoded)[3] = 32767;
    (*encoded)[4] = 0;
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
    header.fileSize = RuntimeAssets::kHeightmapIndexBytes;
    std::memcpy(header.dataFilename.data(), "fixture.heightbin", sizeof("fixture.heightbin"));
    RuntimeAssets::HeightmapTileRecord record{};
    record.tileX = -1;
    record.tileY = 2;
    record.compressedSize = static_cast<std::uint32_t>(compressed.size());
    record.uncompressedSize = RuntimeAssets::kHeightmapFilteredTileBytes;
    record.validSampleCount = std::count_if(encoded->begin(), encoded->end(), [](auto code) { return code != RuntimeAssets::kHeightmapInvalidHeight; });
    record.blobOffset = 0;
    record.sampleScale = 0.125f;
    record.sampleBias = -8.25f;
    {
        std::ofstream index(fixtureDirectory / "fixture.assetbin", std::ios::binary);
        index.write(reinterpret_cast<const char *>(&header), sizeof(header));
        std::vector<RuntimeAssets::HeightmapTileRecord> table(RuntimeAssets::kHeightmapTileTableCount);
        table[RuntimeAssets::HeightmapTileTableIndex(record.tileX, record.tileY)] = record;
        index.write(reinterpret_cast<const char *>(table.data()), RuntimeAssets::kHeightmapTileTableBytes);
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
    if (!loadForComparison(*dataset, -1, 2, samples, error))
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
    // Reject oversized compressed input and incorrect decoded lengths without
    // touching caller output or attempting an overflow allocation.
    const auto validRecord = record;
    if (!validateSpatialTables(fixtureDirectory, header, validRecord)) return 36;
    for (int corrupt = 0; corrupt < 3; ++corrupt)
    {
        record = validRecord;
        if (corrupt == 0) record.compressedSize = RuntimeAssets::kHeightmapFilteredTileBytes + 1;
        if (corrupt == 1) --record.uncompressedSize;
        if (corrupt == 2) record.compressedSize = 1;
        {
            std::fstream index(fixtureDirectory / "fixture.assetbin", std::ios::binary | std::ios::in | std::ios::out);
            index.seekp(sizeof(header) + RuntimeAssets::HeightmapTileTableIndex(-1, 2) * sizeof(record));
            index.write(reinterpret_cast<const char *>(&record), sizeof(record));
        }
        auto bad = EtopoHeightmapDataset::open(fixtureDirectory / "fixture.assetbin", error);
        if (bad && loadForComparison(*bad, -1, 2, samples, error)) return 33;
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
