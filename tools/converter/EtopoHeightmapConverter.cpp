#include "EtopoHeightmapConverter.hpp"

#include "EtopoGeoTiffReader.hpp"
#include "HeightmapTileFilter.hpp"
#include "IcosahedralProjection.hpp"
#include "assets/RuntimeAssetCompression.hpp"
#include "assets/RuntimeHeightmapFormat.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <vector>

namespace
{
using RuntimeAssets::HeightmapTileRecord;
constexpr std::size_t kResolution = RuntimeAssets::kHeightmapTileResolution;
using Tile = std::array<std::int16_t, RuntimeAssets::kHeightmapTileSampleCount>;
static_assert(IcosahedralProjection::kVersion == RuntimeAssets::kHeightmapProjectionVersion);

struct TileEdges
{
    std::array<std::int16_t, kResolution> left{}, right{}, bottom{}, top{};
};

std::string FormatDuration(double seconds)
{
    const auto total = static_cast<std::uint64_t>(std::max(0.0, seconds));
    const auto hours = total / 3600, minutes = total % 3600 / 60, secs = total % 60;
    std::ostringstream out;
    if (hours) out << hours << 'h' << std::setfill('0') << std::setw(2) << minutes << 'm';
    else if (minutes) out << minutes << 'm' << std::setfill('0') << std::setw(2) << secs << 's';
    else out << secs << 's';
    return out.str();
}

template <typename T>
bool WriteObject(std::ofstream& stream, const T& value)
{
    stream.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return static_cast<bool>(stream);
}

std::uint32_t Crc32(std::span<const std::uint8_t> bytes)
{
    std::uint32_t crc = 0xffffffffu;
    for (const std::uint8_t value : bytes)
    {
        crc ^= value;
        for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

std::uint32_t Adler32(std::span<const std::uint8_t> bytes)
{
    std::uint32_t a = 1, b = 0;
    for (const auto value : bytes) { a = (a + value) % 65521u; b = (b + a) % 65521u; }
    return (b << 16u) | a;
}

void AppendBig32(std::vector<std::uint8_t>* out, std::uint32_t value)
{
    out->push_back(static_cast<std::uint8_t>(value >> 24u)); out->push_back(static_cast<std::uint8_t>(value >> 16u));
    out->push_back(static_cast<std::uint8_t>(value >> 8u)); out->push_back(static_cast<std::uint8_t>(value));
}

void AppendPngChunk(std::vector<std::uint8_t>* png, const char type[4], std::span<const std::uint8_t> data)
{
    AppendBig32(png, static_cast<std::uint32_t>(data.size()));
    const std::size_t crcStart = png->size();
    png->insert(png->end(), type, type + 4); png->insert(png->end(), data.begin(), data.end());
    AppendBig32(png, Crc32(std::span<const std::uint8_t>(png->data() + crcStart, png->size() - crcStart)));
}

bool WriteRgbPng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> pixels, std::string* error)
{
    if (pixels.size() != static_cast<std::size_t>(width) * height * 3u)
    { if (error) *error = "invalid RGB preview buffer size"; return false; }
    std::vector<std::uint8_t> scanlines(static_cast<std::size_t>(height) * (1u + width * 3u));
    for (std::uint32_t y = 0; y < height; ++y)
    {
        std::uint8_t* row = scanlines.data() + static_cast<std::size_t>(y) * (1u + width * 3u);
        row[0] = 0;
        std::memcpy(row + 1, pixels.data() + static_cast<std::size_t>(y) * width * 3u, width * 3u);
    }
    std::vector<std::uint8_t> deflate{0x78, 0x01};
    std::size_t offset = 0;
    while (offset < scanlines.size())
    {
        const std::size_t count = std::min<std::size_t>(65535, scanlines.size() - offset);
        const bool final = offset + count == scanlines.size();
        deflate.push_back(final ? 1 : 0);
        deflate.push_back(static_cast<std::uint8_t>(count)); deflate.push_back(static_cast<std::uint8_t>(count >> 8u));
        const std::uint16_t inverse = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(count));
        deflate.push_back(static_cast<std::uint8_t>(inverse)); deflate.push_back(static_cast<std::uint8_t>(inverse >> 8u));
        deflate.insert(deflate.end(), scanlines.begin() + offset, scanlines.begin() + offset + count);
        offset += count;
    }
    AppendBig32(&deflate, Adler32(scanlines));
    std::vector<std::uint8_t> png{137, 80, 78, 71, 13, 10, 26, 10};
    std::array<std::uint8_t, 13> ihdr{};
    ihdr[0] = static_cast<std::uint8_t>(width >> 24u); ihdr[1] = static_cast<std::uint8_t>(width >> 16u);
    ihdr[2] = static_cast<std::uint8_t>(width >> 8u); ihdr[3] = static_cast<std::uint8_t>(width);
    ihdr[4] = static_cast<std::uint8_t>(height >> 24u); ihdr[5] = static_cast<std::uint8_t>(height >> 16u);
    ihdr[6] = static_cast<std::uint8_t>(height >> 8u); ihdr[7] = static_cast<std::uint8_t>(height);
    ihdr[8] = 8; ihdr[9] = 2;
    AppendPngChunk(&png, "IHDR", ihdr); AppendPngChunk(&png, "IDAT", deflate); AppendPngChunk(&png, "IEND", {});
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!output) { if (error) *error = "failed writing " + path.string(); return false; }
    return true;
}

std::array<std::uint8_t, 3> HeightColor(double elevation)
{
    if (elevation < 0.0)
    {
        const double t = std::clamp((elevation + 11000.0) / 11000.0, 0.0, 1.0);
        return {static_cast<std::uint8_t>(5 + 25 * t), static_cast<std::uint8_t>(20 + 90 * t),
            static_cast<std::uint8_t>(70 + 150 * t)};
    }
    const double t = std::clamp(elevation / 5000.0, 0.0, 1.0);
    return {static_cast<std::uint8_t>(35 + 210 * t), static_cast<std::uint8_t>(105 + 130 * t),
        static_cast<std::uint8_t>(40 + 190 * t)};
}

bool WritePreview(const std::filesystem::path& path, const IcosahedralProjection& projection,
    const EtopoGeoTiffReader& source, std::string* error)
{
    constexpr std::uint32_t width = 1024, height = 1024;
    std::vector<std::uint8_t> scanlines(static_cast<std::size_t>(height) * (1u + width * 3u));
    const auto bounds = projection.bounds();
    const double centerX = 0.5 * (bounds.minX + bounds.maxX);
    const double centerY = 0.5 * (bounds.minY + bounds.maxY);
    const double worldSpan = 1.04 * std::max(bounds.maxX - bounds.minX, bounds.maxY - bounds.minY);
    const double metersPerPixel = worldSpan / width;
    const double gridHalfWidth = 0.75 * metersPerPixel;
    for (std::uint32_t y = 0; y < height; ++y)
    {
        std::uint8_t* row = scanlines.data() + static_cast<std::size_t>(y) * (1u + width * 3u);
        row[0] = 0;
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const double px = centerX + (static_cast<double>(x) + 0.5 - width * 0.5) * metersPerPixel;
            const double py = centerY - (static_cast<double>(y) + 0.5 - height * 0.5) * metersPerPixel;
            double lon = 0.0, lat = 0.0, elevation = 0.0;
            const double tileSize = RuntimeAssets::kHeightmapTilePhysicalSizeMeters;
            const double distanceToGridX = std::abs(px - std::round(px / tileSize) * tileSize);
            const double distanceToGridY = std::abs(py - std::round(py / tileSize) * tileSize);
            const bool onGrid = distanceToGridX <= gridHalfWidth || distanceToGridY <= gridHalfWidth;
            std::array<std::uint8_t, 3> color = onGrid ? std::array<std::uint8_t, 3>{ 62, 20, 48 } : std::array<std::uint8_t, 3>{ 24, 0, 36 };
            if (projection.inverse(px, py, &lon, &lat) && source.sampleBilinear(lon, lat, &elevation))
            {
                color = HeightColor(elevation);
                if (onGrid) color = { 255, 80, 40 };
            }
            if (std::abs(px) <= gridHalfWidth || std::abs(py) <= gridHalfWidth) color = { 255, 255, 255 };
            std::memcpy(row + 1 + x * 3u, color.data(), 3);
        }
    }

    std::vector<std::uint8_t> deflate{ 0x78, 0x01 };
    std::size_t offset = 0;
    while (offset < scanlines.size())
    {
        const std::size_t count = std::min<std::size_t>(65535, scanlines.size() - offset);
        const bool final = offset + count == scanlines.size();
        deflate.push_back(final ? 1 : 0);
        deflate.push_back(static_cast<std::uint8_t>(count)); deflate.push_back(static_cast<std::uint8_t>(count >> 8u));
        const std::uint16_t inverse = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(count));
        deflate.push_back(static_cast<std::uint8_t>(inverse)); deflate.push_back(static_cast<std::uint8_t>(inverse >> 8u));
        deflate.insert(deflate.end(), scanlines.begin() + offset, scanlines.begin() + offset + count);
        offset += count;
    }
    AppendBig32(&deflate, Adler32(scanlines));

    std::vector<std::uint8_t> png{ 137, 80, 78, 71, 13, 10, 26, 10 };
    std::array<std::uint8_t, 13> ihdr{};
    ihdr[0] = static_cast<std::uint8_t>(width >> 24u); ihdr[1] = static_cast<std::uint8_t>(width >> 16u); ihdr[2] = static_cast<std::uint8_t>(width >> 8u); ihdr[3] = static_cast<std::uint8_t>(width);
    ihdr[4] = static_cast<std::uint8_t>(height >> 24u); ihdr[5] = static_cast<std::uint8_t>(height >> 16u); ihdr[6] = static_cast<std::uint8_t>(height >> 8u); ihdr[7] = static_cast<std::uint8_t>(height);
    ihdr[8] = 8; ihdr[9] = 2;
    AppendPngChunk(&png, "IHDR", ihdr); AppendPngChunk(&png, "IDAT", deflate); AppendPngChunk(&png, "IEND", {});
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!output) { if (error) *error = "failed writing preview PNG"; return false; }
    return true;
}

bool WritePackedTilePreview(const std::filesystem::path& indexPath, const std::filesystem::path& dataPath,
    const std::filesystem::path& previewPath, std::string* error)
{
    constexpr std::uint32_t thumbnailSize = 32;
    constexpr std::uint32_t samplesPerPixel = RuntimeAssets::kHeightmapTileResolution / thumbnailSize;
    std::ifstream index(indexPath, std::ios::binary);
    std::ifstream data(dataPath, std::ios::binary);
    RuntimeAssets::HeightmapPackHeader header{};
    index.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!index || !data || header.magic != RuntimeAssets::kHeightmapPackMagic)
    { if (error) *error = "failed reopening packed heightmap for thumbnail preview"; return false; }
    const std::uint32_t columns = static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(header.tileCount))));
    const std::uint32_t rows = (header.tileCount + columns - 1u) / columns;
    const std::uint32_t width = columns * thumbnailSize;
    const std::uint32_t height = rows * thumbnailSize;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 3u);
    for (std::size_t i = 0; i < pixels.size(); i += 3)
    { pixels[i] = 24; pixels[i + 1] = 0; pixels[i + 2] = 36; }
    index.seekg(static_cast<std::streamoff>(header.tileRecordOffset));
    std::vector<std::byte> compressed, filtered;
    std::vector<std::int16_t> decoded;
    for (std::uint32_t tileIndex = 0; tileIndex < header.tileCount; ++tileIndex)
    {
        HeightmapTileRecord record{};
        index.read(reinterpret_cast<char*>(&record), sizeof(record));
        if (!index) { if (error) *error = "failed reading tile record for thumbnail preview"; return false; }
        compressed.resize(record.compressedSize);
        data.seekg(static_cast<std::streamoff>(record.blobOffset));
        data.read(reinterpret_cast<char*>(compressed.data()), record.compressedSize);
        if (!data || !RuntimeAssets::DecompressBytes(RuntimeAssets::CompressionType::Lz4, compressed,
                record.uncompressedSize, &filtered, error) || !HeightmapTileFilter::Decode(filtered, &decoded, error)) return false;

        // Records are written in heightbin order. Pack them densely left to
        // right, then top to bottom, without reserving cells for omitted tiles.
        const std::uint32_t tilePixelX = (tileIndex % columns) * thumbnailSize;
        const std::uint32_t tilePixelY = (tileIndex / columns) * thumbnailSize;
        for (std::uint32_t py = 0; py < thumbnailSize; ++py)
        for (std::uint32_t px = 0; px < thumbnailSize; ++px)
        {
            std::int64_t elevationSum = 0;
            std::uint32_t validCount = 0;
            for (std::uint32_t sy = 0; sy < samplesPerPixel; ++sy)
            for (std::uint32_t sx = 0; sx < samplesPerPixel; ++sx)
            {
                const std::uint32_t sourceX = px * samplesPerPixel + sx;
                const std::uint32_t sourceY = (thumbnailSize - 1u - py) * samplesPerPixel + sy;
                const std::int16_t value = decoded[sourceY * RuntimeAssets::kHeightmapTileResolution + sourceX];
                if (value != RuntimeAssets::kHeightmapInvalidHeight) { elevationSum += value; ++validCount; }
            }
            if (validCount == 0) continue;
            const auto color = HeightColor(static_cast<double>(elevationSum) / validCount);
            const std::size_t destination = (static_cast<std::size_t>(tilePixelY + py) * width + tilePixelX + px) * 3u;
            std::memcpy(pixels.data() + destination, color.data(), color.size());
        }
        // Outline every stored record, including the invalid portion of a
        // partial boundary tile, so the dense packing remains inspectable.
        constexpr std::array<std::uint8_t, 3> border{72, 32, 48};
        for (std::uint32_t i = 0; i < thumbnailSize; ++i)
        {
            const std::size_t top = (static_cast<std::size_t>(tilePixelY) * width + tilePixelX + i) * 3u;
            const std::size_t left = (static_cast<std::size_t>(tilePixelY + i) * width + tilePixelX) * 3u;
            std::memcpy(pixels.data() + top, border.data(), border.size());
            std::memcpy(pixels.data() + left, border.data(), border.size());
        }
    }
    return WriteRgbPng(previewPath, width, height, pixels, error);
}

bool ValidateBorders(const std::map<std::pair<int, int>, TileEdges>& edges, std::string* error)
{
    for (const auto& [coordinate, edge] : edges)
    {
        if (const auto right = edges.find({ coordinate.first + 1, coordinate.second }); right != edges.end() && edge.right != right->second.left)
        { if (error) *error = "shared horizontal tile border mismatch"; return false; }
        if (const auto top = edges.find({ coordinate.first, coordinate.second + 1 }); top != edges.end() && edge.top != top->second.bottom)
        { if (error) *error = "shared vertical tile border mismatch"; return false; }
    }
    return true;
}

bool ReopenAndValidate(const std::filesystem::path& indexPath, const std::filesystem::path& dataPath, std::string* error)
{
    std::ifstream index(indexPath, std::ios::binary | std::ios::ate);
    std::ifstream data(dataPath, std::ios::binary | std::ios::ate);
    if (!index || !data) { if (error) *error = "failed to reopen generated heightmap pack"; return false; }
    const auto indexSize = static_cast<std::uint64_t>(index.tellg());
    const auto dataSize = static_cast<std::uint64_t>(data.tellg());
    index.seekg(0); data.seekg(0);
    RuntimeAssets::HeightmapPackHeader header{};
    index.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!index || header.magic != RuntimeAssets::kHeightmapPackMagic || header.version != RuntimeAssets::kHeightmapFormatVersion ||
        header.headerSize != sizeof(header) || header.fileSize != indexSize ||
        header.projectionVersion != RuntimeAssets::kHeightmapProjectionVersion ||
        header.orientationDegrees != IcosahedralProjection::kOrientationDegrees ||
        header.atlasRotationDegrees != IcosahedralProjection::kAtlasRotationDegrees ||
        header.tileRecordOffset + static_cast<std::uint64_t>(header.tileCount) * sizeof(HeightmapTileRecord) != indexSize)
    { if (error) *error = "generated heightmap index header is invalid"; return false; }
    index.seekg(static_cast<std::streamoff>(header.tileRecordOffset));
    std::set<std::pair<int, int>> coordinates;
    std::vector<std::byte> compressed;
    for (std::uint32_t i = 0; i < header.tileCount; ++i)
    {
        HeightmapTileRecord record{};
        index.read(reinterpret_cast<char*>(&record), sizeof(record));
        if (!index || !coordinates.emplace(record.tileX, record.tileY).second || record.compressedSize == 0 ||
            record.uncompressedSize != RuntimeAssets::kHeightmapFilteredTileBytes || record.validSampleCount == 0 ||
            record.blobOffset + record.compressedSize > dataSize)
        { if (error) *error = "generated heightmap tile index record is invalid"; return false; }
        compressed.resize(record.compressedSize);
        data.seekg(static_cast<std::streamoff>(record.blobOffset));
        data.read(reinterpret_cast<char*>(compressed.data()), record.compressedSize);
        std::vector<std::byte> filtered;
        std::vector<std::int16_t> decoded;
        if (!data || !RuntimeAssets::DecompressBytes(RuntimeAssets::CompressionType::Lz4, compressed, record.uncompressedSize, &filtered, error) ||
            !HeightmapTileFilter::Decode(filtered, &decoded, error)) return false;
        const auto valid = std::count_if(decoded.begin(), decoded.end(), [](std::int16_t v) { return v != RuntimeAssets::kHeightmapInvalidHeight; });
        if (valid != record.validSampleCount) { if (error) *error = "reopened tile valid-sample count mismatch"; return false; }
    }
    return true;
}
}

bool EtopoHeightmapConverter::run(const EtopoConversionConfig& config, EtopoConversionSummary* summary, std::string* error)
{
    *summary = {};
    if (!HeightmapTileFilter::RunSelfTests(error)) return false;
    const IcosahedralProjection projection;
    const auto testBounds = projection.bounds();
    std::uint32_t validProjectionTests = 0;
    for (int y = 0; y < 16; ++y)
    for (int x = 0; x < 32; ++x)
    {
        const double testX = testBounds.minX + (x + 0.5) / 32.0 * (testBounds.maxX - testBounds.minX);
        const double testY = testBounds.minY + (y + 0.5) / 16.0 * (testBounds.maxY - testBounds.minY);
        double testLongitude = 0.0, testLatitude = 0.0;
        if (projection.inverse(testX, testY, &testLongitude, &testLatitude) && std::isfinite(testLongitude) && std::isfinite(testLatitude))
            ++validProjectionTests;
    }
    if (validProjectionTests < 100)
    { if (error) *error = "icosahedral projection coverage self-test failed"; return false; }
    double japanX = 0.0, japanY = 0.0, japanLongitude = 0.0, japanLatitude = 0.0;
    if (!projection.forward(139.6917, 35.6895, &japanX, &japanY) ||
        !projection.inverse(japanX, japanY, &japanLongitude, &japanLatitude) ||
        std::abs(japanLongitude - 139.6917) > 1e-6 || std::abs(japanLatitude - 35.6895) > 1e-6)
    { if (error) *error = "icosahedral projection Japan round-trip self-test failed"; return false; }
    double northX = 0.0, northY = 0.0, eastX = 0.0, eastY = 0.0;
    if (!projection.forward(139.6917, 36.6895, &northX, &northY) ||
        !projection.forward(140.6917, 35.6895, &eastX, &eastY))
    { if (error) *error = "icosahedral projection local-basis check failed"; return false; }
    std::cout << "Tokyo projection: " << japanX << ", " << japanY << " m; local north vector: "
              << northX - japanX << ", " << northY - japanY << " m/degree; local east vector: " << eastX - japanX << ", "
              << eastY - japanY << " m/degree\n";
    if (config.selfTestOnly)
    {
        std::cout << "ETOPO self-tests passed: exhaustive filter round-trip and projection origin/bounds\n";
        return true;
    }
    EtopoGeoTiffReader source;
    if (!source.open(config.sourceTiff, error)) return false;
    summary->sourceWidth = source.width(); summary->sourceHeight = source.height();
    std::cout << "ETOPO source: " << source.width() << 'x' << source.height() << ", bounds [" << source.minLongitude() << ", "
        << source.minLatitude() << "] - [" << source.maxLongitude() << ", " << source.maxLatitude() << "], source "
        << source.sourceSampleDescription() << ", canonical int16 range " << source.sourceMin() << ".." << source.sourceMax() << " m\n";

    const auto bounds = projection.bounds();
    const double tileSize = RuntimeAssets::kHeightmapTilePhysicalSizeMeters;
    const int minX = static_cast<int>(std::floor(bounds.minX / tileSize));
    const int maxX = static_cast<int>(std::ceil(bounds.maxX / tileSize)) - 1;
    const int minY = static_cast<int>(std::floor(bounds.minY / tileSize));
    const int maxY = static_cast<int>(std::ceil(bounds.maxY / tileSize)) - 1;
    if (minX < INT8_MIN || maxX > INT8_MAX || minY < INT8_MIN || maxY > INT8_MAX)
    { if (error) *error = "icosahedral atlas does not fit signed 8-bit tile coordinates"; return false; }
    summary->candidateTiles = (maxX - minX + 1) * (maxY - minY + 1);
    summary->minTileX = minX; summary->maxTileX = maxX; summary->minTileY = minY; summary->maxTileY = maxY;
    summary->minHeight = INT16_MAX; summary->maxHeight = INT16_MIN;

    std::filesystem::create_directories(config.outputRoot);
    const auto indexPath = config.outputRoot / "etopo2022.assetbin";
    const auto dataPath = config.outputRoot / "etopo2022.heightbin";
    std::ofstream data(dataPath, std::ios::binary | std::ios::trunc);
    if (!data) { if (error) *error = "failed creating " + dataPath.string(); return false; }
    std::vector<HeightmapTileRecord> records;
    std::map<std::pair<int, int>, TileEdges> edges;
    Tile tile{};
    std::int32_t completedTiles = 0;
    const auto conversionStarted = std::chrono::steady_clock::now();
    auto reportProgress = [&](int tx, int ty, std::uint32_t validCount, std::size_t compressedBytes) {
        ++completedTiles;
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - conversionStarted).count();
        const double eta = completedTiles ? elapsed * (summary->candidateTiles - completedTiles) / completedTiles : 0.0;
        std::cout << "tile " << completedTiles << '/' << summary->candidateTiles << " (" << std::fixed << std::setprecision(1)
                  << (100.0 * completedTiles / summary->candidateTiles) << "%), (" << tx << ',' << ty << "), " << validCount
                  << " valid, " << compressedBytes << " bytes, " << summary->storedTiles << " stored, elapsed "
                  << FormatDuration(elapsed) << ", ETA " << FormatDuration(eta) << '\n';
    };
    for (int ty = minY; ty <= maxY; ++ty)
    for (int tx = minX; tx <= maxX; ++tx)
    {
        std::uint32_t validCount = 0;
        std::int16_t tileMin = INT16_MAX, tileMax = INT16_MIN;
        for (std::size_t y = 0; y < kResolution; ++y)
        for (std::size_t x = 0; x < kResolution; ++x)
        {
            const std::int64_t latticeX = static_cast<std::int64_t>(tx) * RuntimeAssets::kHeightmapTileStride + static_cast<std::int64_t>(x);
            const std::int64_t latticeY = static_cast<std::int64_t>(ty) * RuntimeAssets::kHeightmapTileStride + static_cast<std::int64_t>(y);
            const double px = static_cast<double>(latticeX) * tileSize / RuntimeAssets::kHeightmapTileStride;
            const double py = static_cast<double>(latticeY) * tileSize / RuntimeAssets::kHeightmapTileStride;
            double lon = 0.0, lat = 0.0, elevation = 0.0;
            std::int16_t value = RuntimeAssets::kHeightmapInvalidHeight;
            if (projection.inverse(px, py, &lon, &lat) && source.sampleBilinear(lon, lat, &elevation))
            {
                const long rounded = std::lround(elevation);
                value = static_cast<std::int16_t>(std::clamp<long>(rounded, INT16_MIN + 1L, INT16_MAX));
                ++validCount; tileMin = std::min(tileMin, value); tileMax = std::max(tileMax, value);
            }
            tile[y * kResolution + x] = value;
        }
        if (validCount == 0) { ++summary->omittedTiles; reportProgress(tx, ty, 0, 0); continue; }
        if (validCount != tile.size()) ++summary->partialTiles;

        std::vector<std::byte> filtered, compressed, decompressed;
        std::vector<std::int16_t> decoded;
        if (!HeightmapTileFilter::Encode(tile, &filtered, error) ||
            !RuntimeAssets::CompressBytes(RuntimeAssets::CompressionType::Lz4, filtered, &compressed, error) ||
            !RuntimeAssets::DecompressBytes(RuntimeAssets::CompressionType::Lz4, compressed, filtered.size(), &decompressed, error) ||
            !HeightmapTileFilter::Decode(decompressed, &decoded, error) || !std::equal(decoded.begin(), decoded.end(), tile.begin()))
        { if (error && error->empty()) *error = "tile filter/compression round-trip mismatch"; return false; }

        const std::uint64_t blobOffset = static_cast<std::uint64_t>(data.tellp());
        data.write(reinterpret_cast<const char*>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
        if (!data) { if (error) *error = "failed writing height tile blob"; return false; }
        records.push_back({ static_cast<std::int8_t>(tx), static_cast<std::int8_t>(ty), 0,
            static_cast<std::uint32_t>(compressed.size()), static_cast<std::uint32_t>(filtered.size()), validCount,
            tileMin, tileMax, 0, blobOffset });
        TileEdges edge;
        for (std::size_t i = 0; i < kResolution; ++i)
        { edge.left[i] = tile[i * kResolution]; edge.right[i] = tile[i * kResolution + kResolution - 1]; edge.bottom[i] = tile[i]; edge.top[i] = tile[(kResolution - 1) * kResolution + i]; }
        edges.emplace(std::make_pair(tx, ty), edge);
        ++summary->storedTiles;
        summary->rawBytes += tile.size() * sizeof(std::int16_t); summary->filteredBytes += filtered.size(); summary->compressedBytes += compressed.size();
        summary->minHeight = std::min(summary->minHeight, tileMin); summary->maxHeight = std::max(summary->maxHeight, tileMax);
        reportProgress(tx, ty, validCount, compressed.size());
    }
    data.close();
    if (!ValidateBorders(edges, error)) return false;

    RuntimeAssets::HeightmapPackHeader header{};
    header.magic = RuntimeAssets::kHeightmapPackMagic; header.version = RuntimeAssets::kHeightmapFormatVersion;
    header.flags = RuntimeAssets::kLittleEndianFlag; header.headerSize = sizeof(header); header.tileCount = static_cast<std::uint32_t>(records.size());
    header.tileResolution = RuntimeAssets::kHeightmapTileResolution; header.tileStride = RuntimeAssets::kHeightmapTileStride;
    header.sampleType = static_cast<std::uint32_t>(RuntimeAssets::HeightSampleType::SignedInt16Meters);
    header.invalidHeight = RuntimeAssets::kHeightmapInvalidHeight; header.filterType = static_cast<std::uint32_t>(RuntimeAssets::HeightFilterType::SerpentineDeltaZigZagBytePlanes);
    header.filterVersion = 1; header.compressionType = static_cast<std::uint32_t>(RuntimeAssets::CompressionType::Lz4);
    header.tilePhysicalSizeMeters = tileSize; header.authalicRadiusMeters = IcosahedralProjection::kAuthalicRadiusMeters;
    header.orientationDegrees = IcosahedralProjection::kOrientationDegrees;
    header.atlasRotationDegrees = IcosahedralProjection::kAtlasRotationDegrees;
    header.projectedBoundsMeters = { bounds.minX, bounds.minY, bounds.maxX, bounds.maxY };
    header.projectionVersion = IcosahedralProjection::kVersion; header.projectionName = RuntimeAssets::MakeMagic('I','C','O','G');
    header.tileRecordOffset = sizeof(header); header.fileSize = sizeof(header) + records.size() * sizeof(HeightmapTileRecord);
    std::strncpy(header.dataFilename.data(), "etopo2022.heightbin", header.dataFilename.size() - 1);
    std::strncpy(header.sourceDataset.data(), "ETOPO_2022_v1_60s_surface", header.sourceDataset.size() - 1);
    std::ofstream index(indexPath, std::ios::binary | std::ios::trunc);
    if (!index || !WriteObject(index, header)) { if (error) *error = "failed writing heightmap index header"; return false; }
    index.write(reinterpret_cast<const char*>(records.data()), static_cast<std::streamsize>(records.size() * sizeof(HeightmapTileRecord)));
    if (!index) { if (error) *error = "failed writing heightmap tile records"; return false; }
    index.close();
    if (!ReopenAndValidate(indexPath, dataPath, error)) return false;
    if (!WritePreview(config.outputRoot / "etopo2022_preview.png", projection, source, error)) return false;
    if (!WritePackedTilePreview(indexPath, dataPath, config.outputRoot / "etopo2022_tiles_preview.png", error)) return false;

    std::cout << "Projection: " << projection.description() << ", orientation ("
        << IcosahedralProjection::kOrientationDegrees[0] << ", " << IcosahedralProjection::kOrientationDegrees[1] << ", "
        << IcosahedralProjection::kOrientationDegrees[2] << ") deg, atlas rotation "
        << IcosahedralProjection::kAtlasRotationDegrees << " deg\n"
        << "Projected bounds: [" << bounds.minX << ", " << bounds.minY << "] - [" << bounds.maxX << ", " << bounds.maxY << "] m\n"
        << "Tile extent: X " << minX << ".." << maxX << ", Y " << minY << ".." << maxY << "\n"
        << "Candidates: " << summary->candidateTiles << ", omitted: " << summary->omittedTiles << ", stored: " << summary->storedTiles
        << ", partial: " << summary->partialTiles << "\n"
        << "Raw/filtered/compressed: " << summary->rawBytes << " / " << summary->filteredBytes << " / " << summary->compressedBytes
        << " bytes; ratio " << (summary->compressedBytes ? static_cast<double>(summary->rawBytes) / summary->compressedBytes : 0.0) << ":1\n"
        << "Generated elevation range: " << summary->minHeight << ".." << summary->maxHeight << " m\n"
        << "Previews: etopo2022_preview.png and etopo2022_tiles_preview.png (dense 32x32 decoded tile thumbnails in blob order)\n"
        << "Validation: filter, LZ4, shared borders, index, and reopened pack succeeded\n";
    return true;
}
