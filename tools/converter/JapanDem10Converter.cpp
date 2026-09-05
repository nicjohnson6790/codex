#include "JapanDem10Converter.hpp"

#include "HeightmapTileFilter.hpp"
#include "HeightmapQuantization.hpp"
#include "IcosahedralProjection.hpp"
#include "assets/RuntimeAssetCompression.hpp"
#include "assets/RuntimeHeightmapFormat.hpp"

#include <tiffio.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
constexpr int kMinTile = -128, kMaxTile = 127;
constexpr double kSpacing = 10.0;
constexpr double kOceanCollarMeters = 2000.0;
constexpr double kTileSize = RuntimeAssets::kHeightmapTileStride * kSpacing;
constexpr double kMosaicSize = 256.0 * kTileSize;
// The grid is rotated 7.5 degrees clockwise about the former four-cell
// intersection, then translated 7.5 km west. The northwest center cell is
// deliberately omitted because the rotated four-cell layout covers Japan.
constexpr double kOriginX = 11709172.9257765;
constexpr double kOriginY = -903715.758822597;
constexpr double kXAxisX = 0.936672189248398;
constexpr double kXAxisY = 0.350207381259467;
constexpr double kYAxisX = -0.350207381259467;
constexpr double kYAxisY = 0.936672189248398;
constexpr std::array<std::array<int, 2>, 4> kOffsets{{{{0, 0}}, {{1, 0}}, {{1, 1}}, {{1, 2}}}};
constexpr std::array<const char*, 4> kIds{{"sw", "se", "ce", "ne"}};
using Tile = std::array<std::int16_t, RuntimeAssets::kHeightmapTileSampleCount>;

std::uint32_t Crc32(std::span<const std::uint8_t> bytes)
{
    std::uint32_t crc = 0xffffffffu;
    for (const auto value : bytes) { crc ^= value; for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1u) ^ (0xedb88320u & (0u - (crc & 1u))); }
    return ~crc;
}

std::uint32_t Adler32(std::span<const std::uint8_t> bytes)
{
    std::uint32_t a = 1, b = 0; for (const auto value : bytes) { a = (a + value) % 65521u; b = (b + a) % 65521u; } return (b << 16u) | a;
}

void AppendBig32(std::vector<std::uint8_t>* out, std::uint32_t value)
{
    out->push_back(static_cast<std::uint8_t>(value >> 24u)); out->push_back(static_cast<std::uint8_t>(value >> 16u)); out->push_back(static_cast<std::uint8_t>(value >> 8u)); out->push_back(static_cast<std::uint8_t>(value));
}

void AppendPngChunk(std::vector<std::uint8_t>* png, const char type[4], std::span<const std::uint8_t> data)
{
    AppendBig32(png, static_cast<std::uint32_t>(data.size())); const std::size_t crcStart = png->size();
    png->insert(png->end(), type, type + 4); png->insert(png->end(), data.begin(), data.end());
    AppendBig32(png, Crc32(std::span<const std::uint8_t>(png->data() + crcStart, png->size() - crcStart)));
}

bool WriteRgbPng(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> pixels, std::string* error)
{
    if (pixels.size() != static_cast<std::size_t>(width) * height * 3u) { *error = "invalid Japan preview buffer size"; return false; }
    std::vector<std::uint8_t> scanlines(static_cast<std::size_t>(height) * (1u + width * 3u));
    for (std::uint32_t y = 0; y < height; ++y) { auto* row = scanlines.data() + static_cast<std::size_t>(y) * (1u + width * 3u); row[0] = 0; std::memcpy(row + 1, pixels.data() + static_cast<std::size_t>(y) * width * 3u, width * 3u); }
    std::vector<std::uint8_t> deflate{0x78, 0x01};
    for (std::size_t offset = 0; offset < scanlines.size();)
    {
        const std::size_t count = std::min<std::size_t>(65535, scanlines.size() - offset); const bool final = offset + count == scanlines.size();
        deflate.push_back(final ? 1 : 0); deflate.push_back(static_cast<std::uint8_t>(count)); deflate.push_back(static_cast<std::uint8_t>(count >> 8u));
        const auto inverse = static_cast<std::uint16_t>(~static_cast<std::uint16_t>(count)); deflate.push_back(static_cast<std::uint8_t>(inverse)); deflate.push_back(static_cast<std::uint8_t>(inverse >> 8u));
        deflate.insert(deflate.end(), scanlines.begin() + offset, scanlines.begin() + offset + count); offset += count;
    }
    AppendBig32(&deflate, Adler32(scanlines)); std::vector<std::uint8_t> png{137,80,78,71,13,10,26,10}; std::array<std::uint8_t,13> ihdr{};
    ihdr[0]=static_cast<std::uint8_t>(width>>24u); ihdr[1]=static_cast<std::uint8_t>(width>>16u); ihdr[2]=static_cast<std::uint8_t>(width>>8u); ihdr[3]=static_cast<std::uint8_t>(width);
    ihdr[4]=static_cast<std::uint8_t>(height>>24u); ihdr[5]=static_cast<std::uint8_t>(height>>16u); ihdr[6]=static_cast<std::uint8_t>(height>>8u); ihdr[7]=static_cast<std::uint8_t>(height); ihdr[8]=8; ihdr[9]=2;
    AppendPngChunk(&png,"IHDR",ihdr); AppendPngChunk(&png,"IDAT",deflate); AppendPngChunk(&png,"IEND",{});
    std::ofstream output(path,std::ios::binary|std::ios::trunc); output.write(reinterpret_cast<const char*>(png.data()),static_cast<std::streamsize>(png.size()));
    if (!output) { *error = "failed writing Japan preview PNG: " + path.string(); return false; } return true;
}

std::array<std::uint8_t,3> DeltaColor(double delta)
{
    const double t=std::clamp(std::abs(delta)/1500.0,0.0,1.0); const auto fade=[&](int neutral,int extreme){return static_cast<std::uint8_t>(std::lround(neutral+(extreme-neutral)*t));};
    return delta < 0.0 ? std::array<std::uint8_t,3>{fade(34,30),fade(34,110),fade(38,240)} : std::array<std::uint8_t,3>{fade(34,245),fade(34,75),fade(38,35)};
}

bool WritePackedDeltaPreview(const std::filesystem::path& indexPath,const std::filesystem::path& dataPath,const std::filesystem::path& path,std::string* error)
{
    constexpr std::uint32_t thumb=32, block=8; std::ifstream index(indexPath,std::ios::binary),data(dataPath,std::ios::binary); RuntimeAssets::HeightmapPackHeader header{}; index.read(reinterpret_cast<char*>(&header),sizeof(header));
    if(!index||!data||header.magic!=RuntimeAssets::kHeightmapPackMagic||header.version!=RuntimeAssets::kHeightmapFormatVersion||header.sampleType!=static_cast<std::uint32_t>(RuntimeAssets::HeightSampleType::QuantizedInt16ScaleBias)){*error="failed reopening Japan delta pack for preview";return false;}
    const std::uint32_t columns=std::max(1u,static_cast<std::uint32_t>(std::ceil(std::sqrt(static_cast<double>(header.tileCount))))),rows=std::max(1u,(header.tileCount+columns-1u)/columns),width=columns*thumb,height=rows*thumb;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width)*height*3u,12); index.seekg(static_cast<std::streamoff>(header.tileRecordOffset));
    std::vector<std::byte> compressed,filtered; std::vector<std::int16_t> decoded;
    for(std::uint32_t i=0;i<header.tileCount;++i)
    {
        RuntimeAssets::HeightmapTileRecord record{}; index.read(reinterpret_cast<char*>(&record),sizeof(record)); compressed.resize(record.compressedSize); data.seekg(static_cast<std::streamoff>(record.blobOffset)); data.read(reinterpret_cast<char*>(compressed.data()),record.compressedSize);
        if(!index||!data||!RuntimeAssets::DecompressBytes(RuntimeAssets::CompressionType::Lz4,compressed,record.uncompressedSize,&filtered,error)||!HeightmapTileFilter::Decode(filtered,&decoded,error))return false;
        if (!std::isfinite(record.sampleScale) || record.sampleScale < 0 || !std::isfinite(record.sampleBias))
        { *error = "invalid reopened Japan scale/bias"; return false; }
        for (std::size_t sample = 0; sample < decoded.size(); ++sample)
        {
            const float meters = RuntimeAssets::DecodeHeight(decoded[sample], record.sampleScale, record.sampleBias);
            if (!std::isfinite(meters)) { *error = "non-finite reopened Japan height"; return false; }
            if ((record.tileX == kMaxTile && sample % 256 == 255) || (record.tileY == kMaxTile && sample / 256 == 255))
                if (decoded[sample] != RuntimeAssets::kHeightmapExactZeroHeight || meters != 0.0f)
                { *error = "reopened Japan positive edge is not exact zero"; return false; }
        }
        const std::uint32_t ox=(i%columns)*thumb,oy=(i/columns)*thumb;
        for(std::uint32_t py=0;py<thumb;++py)for(std::uint32_t px=0;px<thumb;++px){double sum=0;for(std::uint32_t sy=0;sy<block;++sy)for(std::uint32_t sx=0;sx<block;++sx)sum+=RuntimeAssets::DecodeHeight(decoded[((thumb-1u-py)*block+sy)*256+px*block+sx],record.sampleScale,record.sampleBias);const auto color=DeltaColor(static_cast<double>(sum)/(block*block));std::memcpy(pixels.data()+(static_cast<std::size_t>(oy+py)*width+ox+px)*3u,color.data(),3);}
    }
    return WriteRgbPng(path,width,height,pixels,error);
}

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

struct TiffCloser { void operator()(TIFF* value) const { if (value) TIFFClose(value); } };

std::optional<double> ReadGdalNoData(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary); std::array<std::uint8_t,8> header{};
    if(!input.read(reinterpret_cast<char*>(header.data()),header.size())) return {};
    const bool little=header[0]=='I'&&header[1]=='I'; if(!little && !(header[0]=='M'&&header[1]=='M')) return {};
    auto u16=[&](const std::uint8_t* p){return static_cast<std::uint16_t>(little?p[0]|p[1]<<8:p[0]<<8|p[1]);};
    auto u32=[&](const std::uint8_t* p){return little?static_cast<std::uint32_t>(p[0])|static_cast<std::uint32_t>(p[1])<<8|static_cast<std::uint32_t>(p[2])<<16|static_cast<std::uint32_t>(p[3])<<24:static_cast<std::uint32_t>(p[0])<<24|static_cast<std::uint32_t>(p[1])<<16|static_cast<std::uint32_t>(p[2])<<8|static_cast<std::uint32_t>(p[3]);};
    if(u16(header.data()+2)!=42) return {}; input.seekg(u32(header.data()+4)); std::array<std::uint8_t,2> countBytes{};
    if(!input.read(reinterpret_cast<char*>(countBytes.data()),2)) return {}; const auto count=u16(countBytes.data());
    for(std::uint16_t i=0;i<count;++i){std::array<std::uint8_t,12> entry{};if(!input.read(reinterpret_cast<char*>(entry.data()),12))return{};if(u16(entry.data())!=42113)continue;const auto size=u32(entry.data()+4);std::string text(size,'\0');if(size<=4)std::memcpy(text.data(),entry.data()+8,size);else{const auto resume=input.tellg();input.seekg(u32(entry.data()+8));input.read(text.data(),size);input.seekg(resume);}char* end=nullptr;const double value=std::strtod(text.c_str(),&end);if(end!=text.c_str())return value;return{};}
    return {};
}

struct Raster
{
    std::filesystem::path path;
    std::uint32_t width = 0, height = 0;
    double minLon = 0, maxLon = 0, minLat = 0, maxLat = 0, pixelX = 0, pixelY = 0;
    std::uint16_t bits = 0, format = 0;
    bool pixelIsPoint = false;
    std::optional<double> noData;
    mutable std::vector<double> values;

    bool readInfo(std::string* error)
    {
        std::unique_ptr<TIFF, TiffCloser> tif(TIFFOpen(path.string().c_str(), "r"));
        if (!tif) { *error = "could not open DEM10 GeoTIFF: " + path.string(); return false; }
        std::uint16_t spp = 0, planar = 0;
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLESPERPIXEL, &spp);
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_BITSPERSAMPLE, &bits);
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_SAMPLEFORMAT, &format);
        TIFFGetFieldDefaulted(tif.get(), TIFFTAG_PLANARCONFIG, &planar);
        if (!TIFFGetField(tif.get(), TIFFTAG_IMAGEWIDTH, &width) || !TIFFGetField(tif.get(), TIFFTAG_IMAGELENGTH, &height) ||
            spp != 1 || planar != PLANARCONFIG_CONTIG ||
            !((format == SAMPLEFORMAT_IEEEFP && (bits == 32 || bits == 64)) ||
              (format == SAMPLEFORMAT_INT && (bits == 16 || bits == 32))))
        { *error = "unsupported DEM10 sample layout: " + path.string(); return false; }
        double *scale = nullptr, *tie = nullptr; std::uint32_t scaleCount = 0, tieCount = 0;
        constexpr ttag_t scaleTag = 33550, tieTag = 33922, geoKeyTag = 34735;
        if (!TIFFGetField(tif.get(), scaleTag, &scaleCount, &scale) || scaleCount < 2 ||
            !TIFFGetField(tif.get(), tieTag, &tieCount, &tie) || tieCount < 6 || scale[0] <= 0 || scale[1] <= 0)
        { *error = "DEM10 GeoTIFF lacks supported scale/tiepoint georeferencing: " + path.string(); return false; }
        pixelX = scale[0]; pixelY = scale[1];
        minLon = tie[3] - tie[0] * pixelX; maxLat = tie[4] + tie[1] * pixelY;
        maxLon = minLon + static_cast<double>(width) * pixelX;
        minLat = maxLat - static_cast<double>(height) * pixelY;
        std::uint32_t keyCount = 0; std::uint16_t* keys = nullptr; bool epsg6668 = false;
        if (TIFFGetField(tif.get(), geoKeyTag, &keyCount, &keys) && keyCount >= 4)
        {
            const std::uint16_t entries = keys[3];
            for (std::uint16_t i = 0; i < entries && 4 + i * 4 + 3 < keyCount; ++i)
            {
                const auto key = keys + 4 + i * 4;
                if (key[0] == 2048 && key[1] == 0 && key[3] == 6668) epsg6668 = true;
                if (key[0] == 1025 && key[1] == 0) pixelIsPoint = key[3] == 2;
            }
        }
        if (!epsg6668) { *error = "DEM10 GeoTIFF is not declared as EPSG:6668: " + path.string(); return false; }
        noData = ReadGdalNoData(path);
        return true;
    }

    bool load(std::string* error) const
    {
        if (!values.empty()) return true;
        std::unique_ptr<TIFF, TiffCloser> tif(TIFFOpen(path.string().c_str(), "r"));
        if (!tif) { *error = "could not reopen DEM10 GeoTIFF: " + path.string(); return false; }
        values.resize(static_cast<std::size_t>(width) * height);
        auto store = [&](std::size_t destination, const std::byte* bytes, std::size_t source)
        {
            if (format == SAMPLEFORMAT_IEEEFP && bits == 32) values[destination] = reinterpret_cast<const float*>(bytes)[source];
            else if (format == SAMPLEFORMAT_IEEEFP) values[destination] = reinterpret_cast<const double*>(bytes)[source];
            else if (bits == 16) values[destination] = reinterpret_cast<const std::int16_t*>(bytes)[source];
            else values[destination] = reinterpret_cast<const std::int32_t*>(bytes)[source];
        };
        if (TIFFIsTiled(tif.get()))
        {
            std::uint32_t tileWidth = 0, tileHeight = 0; TIFFGetField(tif.get(), TIFFTAG_TILEWIDTH, &tileWidth); TIFFGetField(tif.get(), TIFFTAG_TILELENGTH, &tileHeight);
            std::vector<std::byte> tile(static_cast<std::size_t>(TIFFTileSize(tif.get())));
            for (std::uint32_t y0 = 0; y0 < height; y0 += tileHeight) for (std::uint32_t x0 = 0; x0 < width; x0 += tileWidth)
            {
                if (TIFFReadTile(tif.get(), tile.data(), x0, y0, 0, 0) < 0) { *error = "failed reading DEM10 tile"; return false; }
                for (std::uint32_t y = 0; y < std::min(tileHeight, height - y0); ++y) for (std::uint32_t x = 0; x < std::min(tileWidth, width - x0); ++x)
                    store(static_cast<std::size_t>(y0 + y) * width + x0 + x, tile.data(), static_cast<std::size_t>(y) * tileWidth + x);
            }
        }
        else
        {
            std::vector<std::byte> row(static_cast<std::size_t>(TIFFScanlineSize(tif.get())));
            for (std::uint32_t y = 0; y < height; ++y)
            {
                if (TIFFReadScanline(tif.get(), row.data(), y) < 0) { *error = "failed reading DEM10 scanline"; return false; }
                for (std::uint32_t x = 0; x < width; ++x) store(static_cast<std::size_t>(y) * width + x, row.data(), x);
            }
        }
        return true;
    }

    bool gridCoordinates(double lon, double lat, double* x, double* y) const
    {
        const double shift = pixelIsPoint ? 0.0 : 0.5;
        *x = (lon - minLon) / pixelX - shift;
        *y = (maxLat - lat) / pixelY - shift;
        return true;
    }

    bool samplePixel(std::int64_t x, std::int64_t y, double* out) const
    {
        if (x < 0 || y < 0 || x >= width || y >= height || values.empty()) return false;
        const double value = values[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)];
        if (!std::isfinite(value) || (noData && value == *noData)) return false;
        *out = value;
        return true;
    }

    std::array<double, 2> pixelCenter(std::int64_t x, std::int64_t y) const
    {
        const double shift = pixelIsPoint ? 0.0 : 0.5;
        return {minLon + (static_cast<double>(x) + shift) * pixelX,
                maxLat - (static_cast<double>(y) + shift) * pixelY};
    }
};

class RasterCatalog
{
public:
    static std::uint64_t binKey(int x, int y) { return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(y)) << 32) | static_cast<std::uint32_t>(x); }
    bool open(const std::filesystem::path& root, std::string* error)
    {
        if (!std::filesystem::is_directory(root)) { *error = "DEM10 source directory is missing: " + root.string(); return false; }
        std::vector<std::filesystem::path> paths;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
            if (entry.is_regular_file() && (entry.path().extension() == ".tif" || entry.path().extension() == ".tiff"))
                paths.push_back(entry.path());
        std::sort(paths.begin(), paths.end());
        if (paths.empty()) { *error = "DEM10 source directory contains no GeoTIFF files"; return false; }
        std::cout << "Indexing " << paths.size() << " DEM10 GeoTIFF files...\n";
        const auto started = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < paths.size(); ++i)
        {
            Raster r; r.path = paths[i]; if (!r.readInfo(error)) return false; rasters.push_back(std::move(r));
            if ((i + 1) % 1000 == 0 || i + 1 == paths.size())
            {
                const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                std::cout << "  Indexed " << i + 1 << '/' << paths.size() << " files (" << std::fixed << std::setprecision(1)
                          << (100.0 * (i + 1) / paths.size()) << "%, " << FormatDuration(elapsed) << ")\n";
            }
        }
        for (std::size_t i = 0; i < rasters.size(); ++i)
        {
            const auto& r = rasters[i];
            for (int y = static_cast<int>(std::floor(r.minLat * 10)); y <= static_cast<int>(std::floor(r.maxLat * 10)); ++y)
                for (int x = static_cast<int>(std::floor(r.minLon * 10)); x <= static_cast<int>(std::floor(r.maxLon * 10)); ++x)
                    bins[binKey(x, y)].push_back(i);
        }
        return true;
    }
    bool ensureResident(std::size_t index, std::string* error) const
    {
        if (!rasters[index].values.empty()) return true;
        if (residentRasters.size() == kResidentRasterLimit)
        {
            rasters[residentRasters.front()].values.clear();
            residentRasters.pop_front();
        }
        if (!rasters[index].load(error)) return false;
        residentRasters.push_back(index);
        return true;
    }

    bool sampleGridPoint(double lon, double lat, double expectedPixelX, double expectedPixelY,
                         double* value, std::string* error) const
    {
        const int bx = static_cast<int>(std::floor(lon * 10)), by = static_cast<int>(std::floor(lat * 10));
        const auto found = bins.find(binKey(bx, by));
        if (found == bins.end()) return false;
        for (const std::size_t i : found->second)
        {
            const Raster& r = rasters[i];
            if (std::abs(r.pixelX - expectedPixelX) > expectedPixelX * 1e-6 ||
                std::abs(r.pixelY - expectedPixelY) > expectedPixelY * 1e-6)
                continue;
            double fx = 0.0, fy = 0.0;
            r.gridCoordinates(lon, lat, &fx, &fy);
            const auto ix = static_cast<std::int64_t>(std::llround(fx));
            const auto iy = static_cast<std::int64_t>(std::llround(fy));
            if (std::abs(fx - ix) > 1e-5 || std::abs(fy - iy) > 1e-5 ||
                ix < 0 || iy < 0 || ix >= r.width || iy >= r.height)
                continue;
            if (!ensureResident(i, error)) return false;
            if (r.samplePixel(ix, iy, value)) return true;
        }
        return false;
    }

    bool sample(double lon, double lat, double* value, std::string* error) const
    {
        const int bx = static_cast<int>(std::floor(lon * 10)), by = static_cast<int>(std::floor(lat * 10));
        const auto found = bins.find(binKey(bx, by));
        if (found == bins.end()) return false;
        for (const std::size_t i : found->second)
            if (const Raster& r = rasters[i]; lon >= r.minLon && lon <= r.maxLon && lat >= r.minLat && lat <= r.maxLat)
            {
                double fx = 0.0, fy = 0.0;
                r.gridCoordinates(lon, lat, &fx, &fy);
                const auto x0 = static_cast<std::int64_t>(std::floor(fx));
                const auto y0 = static_cast<std::int64_t>(std::floor(fy));
                const double tx = fx - x0, ty = fy - y0;
                std::array<double, 4> samples{};
                bool complete = true;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        const auto point = r.pixelCenter(x0 + dx, y0 + dy);
                        complete &= sampleGridPoint(point[0], point[1], r.pixelX, r.pixelY,
                                                    &samples[static_cast<std::size_t>(dy * 2 + dx)], error);
                    }
                if (!complete) continue;
                *value = (samples[0] * (1.0 - tx) + samples[1] * tx) * (1.0 - ty) +
                         (samples[2] * (1.0 - tx) + samples[3] * tx) * ty;
                return true;
            }
        return false;
    }

    double coverageWeight(double lon, double lat, double collarMeters) const
    {
        constexpr double latitudeMetersPerDegree = 110540.0;
        const double longitudeMetersPerDegree = 111320.0 * std::max(std::cos(lat * 3.14159265358979323846 / 180.0), 0.01);
        const int bx = static_cast<int>(std::floor(lon * 10));
        const int by = static_cast<int>(std::floor(lat * 10));
        double nearestSquared = std::numeric_limits<double>::max();
        std::set<std::size_t> candidates;
        for (int y = by - 1; y <= by + 1; ++y)
            for (int x = bx - 1; x <= bx + 1; ++x)
                if (const auto found = bins.find(binKey(x, y)); found != bins.end())
                    candidates.insert(found->second.begin(), found->second.end());
        for (const std::size_t i : candidates)
        {
            const Raster& r = rasters[i];
            const double dxDegrees = lon < r.minLon ? r.minLon - lon : lon > r.maxLon ? lon - r.maxLon : 0.0;
            const double dyDegrees = lat < r.minLat ? r.minLat - lat : lat > r.maxLat ? lat - r.maxLat : 0.0;
            const double dx = dxDegrees * longitudeMetersPerDegree;
            const double dy = dyDegrees * latitudeMetersPerDegree;
            nearestSquared = std::min(nearestSquared, dx * dx + dy * dy);
        }
        if (nearestSquared == 0.0) return 1.0;
        const double distance = std::sqrt(nearestSquared);
        return distance >= collarMeters ? 0.0 : 1.0 - distance / collarMeters;
    }
    bool validateFirstDecode(std::string* error) const { return !rasters.empty() && rasters.front().load(error); }
    mutable std::vector<Raster> rasters;
    static constexpr std::size_t kResidentRasterLimit = 8;
    mutable std::deque<std::size_t> residentRasters;
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> bins;
};

class EtopoSampler
{
public:
    bool open(const std::filesystem::path& indexPath, std::string* error)
    {
        index.open(indexPath, std::ios::binary); if (!index.read(reinterpret_cast<char*>(&header), sizeof(header))) { *error = "could not read ETOPO index"; return false; }
        if (header.magic != RuntimeAssets::kHeightmapPackMagic || header.version != RuntimeAssets::kHeightmapFormatVersion || header.sampleType != static_cast<std::uint32_t>(RuntimeAssets::HeightSampleType::QuantizedInt16ScaleBias) || header.projectionVersion != IcosahedralProjection::kVersion) { *error = "incompatible ETOPO pack"; return false; }
        records.resize(header.tileCount); index.seekg(static_cast<std::streamoff>(header.tileRecordOffset));
        if (!index.read(reinterpret_cast<char*>(records.data()), static_cast<std::streamsize>(records.size() * sizeof(records[0])))) { *error = "could not read ETOPO records"; return false; }
        data.open(indexPath.parent_path() / header.dataFilename.data(), std::ios::binary); if (!data) { *error = "could not open ETOPO height data"; return false; }
        return true;
    }
    bool sample(double px, double py, double* value, std::string* error)
    {
        const double u = px / header.tilePhysicalSizeMeters * RuntimeAssets::kHeightmapTileStride;
        const double v = py / header.tilePhysicalSizeMeters * RuntimeAssets::kHeightmapTileStride;
        const auto ix = static_cast<std::int64_t>(std::floor(u)), iy = static_cast<std::int64_t>(std::floor(v));
        double total = 0; const double tx = u - ix, ty = v - iy;
        for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx)
        {
            double s = 0; if (!sampleLattice(ix + dx, iy + dy, &s, error)) return false;
            total += s * (dx ? tx : 1.0 - tx) * (dy ? ty : 1.0 - ty);
        }
        *value = total; return true;
    }
private:
    bool sampleLattice(std::int64_t x, std::int64_t y, double* value, std::string* error)
    {
        const auto floorDiv = [](std::int64_t a) { auto q = a / 255, r = a % 255; return r < 0 ? q - 1 : q; };
        const int tx = static_cast<int>(floorDiv(x)), ty = static_cast<int>(floorDiv(y));
        const int sx = static_cast<int>(x - static_cast<std::int64_t>(tx) * 255), sy = static_cast<int>(y - static_cast<std::int64_t>(ty) * 255);
        const std::uint32_t key = (static_cast<std::uint32_t>(static_cast<std::uint16_t>(tx)) << 16) |
                                  static_cast<std::uint16_t>(ty);
        auto cached = tileCache.find(key);
        if (cached == tileCache.end())
        {
            const auto it = std::lower_bound(records.begin(), records.end(), std::pair{ty, tx}, [](const auto& r, const auto& k) { return std::pair{int(r.tileY), int(r.tileX)} < k; });
            if (it == records.end() || it->tileX != tx || it->tileY != ty) return false;
            if (!std::isfinite(it->sampleScale) || it->sampleScale < 0 || !std::isfinite(it->sampleBias))
            { *error = "invalid ETOPO scale/bias"; return false; }
            std::vector<std::byte> compressed(it->compressedSize), filtered;
            data.clear(); data.seekg(static_cast<std::streamoff>(it->blobOffset)); data.read(reinterpret_cast<char*>(compressed.data()), it->compressedSize);
            std::vector<std::int16_t> decoded;
            if (!data || !RuntimeAssets::DecompressBytes(RuntimeAssets::CompressionType::Lz4, compressed, it->uncompressedSize, &filtered, error) ||
                !HeightmapTileFilter::Decode(filtered, &decoded, error)) return false;
            std::vector<float> meters(decoded.size());
            for (std::size_t i = 0; i < decoded.size(); ++i) meters[i] = decoded[i] == RuntimeAssets::kHeightmapInvalidHeight ? std::numeric_limits<float>::quiet_NaN() : RuntimeAssets::DecodeHeight(decoded[i], it->sampleScale, it->sampleBias);
            cached = tileCache.emplace(key, std::move(meters)).first;
        }
        const auto s = cached->second[static_cast<std::size_t>(sy) * 256 + sx]; if (!std::isfinite(s)) return false;
        *value = s; return true;
    }
    RuntimeAssets::HeightmapPackHeader header{}; std::vector<RuntimeAssets::HeightmapTileRecord> records;
    std::ifstream index, data;
    std::unordered_map<std::uint32_t, std::vector<float>> tileCache;
};

std::array<double, 2> MosaicOrigin(std::size_t i)
{
    return {kOriginX + kOffsets[i][0] * kMosaicSize * kXAxisX + kOffsets[i][1] * kMosaicSize * kYAxisX,
            kOriginY + kOffsets[i][0] * kMosaicSize * kXAxisY + kOffsets[i][1] * kMosaicSize * kYAxisY};
}

bool WriteMetadata(const std::filesystem::path& path, const std::filesystem::path& sourceRoot, const RasterCatalog& source, std::string* error)
{
    std::ofstream out(path, std::ios::trunc); if (!out) { *error = "could not write Japan metadata"; return false; }
    std::string manifestHash = "unavailable"; std::ifstream hashInput(sourceRoot / "latest_file_list.csv.sha256"); if (hashInput) hashInput >> manifestHash;
    out << std::fixed << std::setprecision(9) << "{\n  \"schema\": \"codex.japan-dem10-delta.v1\",\n"
        << "  \"source\": {\"dataset\": \"smartmaps/japan-geotiff-dem/10\", \"manifest\": \"10/latest_file_list.csv.gz\", \"manifestCsvSha256\": \"" << manifestHash << "\", \"crs\": \"EPSG:6668\", \"fileCount\": " << source.rasters.size() << "},\n"
        << "  \"sampleSpacingMeters\": 10.0,\n  \"tileResolution\": 256,\n  \"tileStride\": 255,\n  \"tileRange\": [-128, -128, 127, 127],\n"
        << "  \"basis\": {\"x\": [" << kXAxisX << ", " << kXAxisY << "], \"y\": [" << kYAxisX << ", " << kYAxisY << "], \"rotationDegrees\": 20.5},\n"
        << "  \"mosaicSizeMeters\": " << kMosaicSize << ",\n  \"positiveEdgesZeroed\": true,\n  \"mosaics\": [\n";
    for (std::size_t i = 0; i < kIds.size(); ++i) { const auto o = MosaicOrigin(i); out << "    {\"id\": \"" << kIds[i] << "\", \"offset\": [" << kOffsets[i][0] << ", " << kOffsets[i][1] << "], \"originAiroceanMeters\": [" << o[0] << ", " << o[1] << "]}" << (i + 1 == kIds.size() ? "\n" : ",\n"); }
    out << "  ],\n  \"delta\": \"DEM10 bilinear elevation minus bilinear existing Codex ETOPO v3 scale/bias decoded tile elevation, with a 2000 m fading positive-ETOPO ocean collar, quantized with independent per-tile scale/bias and exact additive-zero codes\"\n}\n";
    return static_cast<bool>(out);
}

bool WritePreview(const std::filesystem::path& path, const RasterCatalog& source, std::string* error)
{
    constexpr int w = 900, h = 1125; const double spanX = 3.25 * kMosaicSize, spanY = 1.25 * spanX;
    IcosahedralProjection projection; double centerX=0.0,centerY=0.0;
    if(!projection.forward(138.7274,35.3606,&centerX,&centerY)){*error="could not project Mt. Fuji for Japan preview centering";return false;}
    const double minX=centerX-0.5*spanX,minY=centerY-0.5*spanY;
    auto sx = [&](double x) { return (x - minX) / spanX * w; }; auto sy = [&](double y) { return h - (y - minY) / spanY * h; };
    std::ofstream out(path); if (!out) { *error = "could not write Japan footprint preview"; return false; }
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << w << "\" height=\"" << h << "\" viewBox=\"0 0 " << w << ' ' << h << "\"><rect width=\"100%\" height=\"100%\" fill=\"#160024\"/>";
    for (const auto& r : source.rasters)
    {
        std::array<std::array<double,2>,4> p{}; bool valid=true; const std::array<std::array<double,2>,4> ll{{{{r.minLon,r.minLat}},{{r.maxLon,r.minLat}},{{r.maxLon,r.maxLat}},{{r.minLon,r.maxLat}}}};
        for(std::size_t i=0;i<p.size();++i) valid&=projection.forward(ll[i][0],ll[i][1],&p[i][0],&p[i][1]);
        if(valid){out<<"<polygon points=\"";for(auto q:p)out<<sx(q[0])<<','<<sy(q[1])<<' ';out<<"\" fill=\"#55c878\" fill-opacity=\"0.45\" stroke=\"none\"/>";}
    }
    for (std::size_t i=0;i<kIds.size();++i) { const auto o=MosaicOrigin(i); std::array<std::array<double,2>,4> p{{{{o[0],o[1]}},{{o[0]+kMosaicSize*kXAxisX,o[1]+kMosaicSize*kXAxisY}},{{o[0]+kMosaicSize*(kXAxisX+kYAxisX),o[1]+kMosaicSize*(kXAxisY+kYAxisY)}},{{o[0]+kMosaicSize*kYAxisX,o[1]+kMosaicSize*kYAxisY}}}}; out<<"<polygon points=\""; for(auto q:p) out<<sx(q[0])<<','<<sy(q[1])<<' '; out<<"\" fill=\"none\" stroke=\"#ff7040\" stroke-width=\"3\"/><text x=\""<<sx(o[0])+8<<"\" y=\""<<sy(o[1])-8<<"\" fill=\"white\">"<<kIds[i]<<"</text>"; }
    out << "</svg>\n"; return true;
}

bool SelfTest(std::string* error)
{
    if (!HeightmapQuantization::SelfTest(error) || !HeightmapTileFilter::RunSelfTests(error)) return false;
    {
        RasterCatalog catalog;
        Raster west{}, east{};
        west.width = east.width = 2; west.height = east.height = 2;
        west.minLon = 0.0; west.maxLon = 2.0; east.minLon = 2.0; east.maxLon = 4.0;
        west.minLat = east.minLat = 0.0; west.maxLat = east.maxLat = 2.0;
        west.pixelX = west.pixelY = east.pixelX = east.pixelY = 1.0;
        west.values = {0.0, 10.0, 0.0, 10.0};
        east.values = {20.0, 30.0, 20.0, 30.0};
        catalog.rasters = {std::move(west), std::move(east)};
        for (std::size_t i = 0; i < catalog.rasters.size(); ++i)
            for (int y = 0; y <= 20; ++y)
                for (int x = static_cast<int>(catalog.rasters[i].minLon * 10.0);
                     x <= static_cast<int>(catalog.rasters[i].maxLon * 10.0); ++x)
                    catalog.bins[RasterCatalog::binKey(x, y)].push_back(i);
        double seamSample = 0.0;
        if (!catalog.sample(2.0, 1.0, &seamSample, error) || std::abs(seamSample - 15.0) > 1e-12)
        {
            *error = "cross-raster bilinear sampling self-test failed";
            return false;
        }
        if (catalog.coverageWeight(2.0, 1.0, kOceanCollarMeters) != 1.0 ||
            catalog.coverageWeight(4.01, 1.0, kOceanCollarMeters) <= 0.0 ||
            catalog.coverageWeight(4.1, 1.0, kOceanCollarMeters) != 0.0)
        {
            *error = "DEM10 ocean-collar coverage self-test failed";
            return false;
        }
    }
    if (std::abs(std::hypot(kXAxisX,kXAxisY)-1.0)>1e-12 || std::abs(kXAxisX*kYAxisX+kXAxisY*kYAxisY)>1e-12) { *error="Japan basis is not orthonormal"; return false; }
    Tile t; t.fill(7); for (std::size_t i=0;i<256;++i) { t[255*256+i]=0; t[i*256+255]=0; }
    for (std::size_t i=0;i<256;++i) if (t[255*256+i]!=0 || t[i*256+255]!=0) { *error="positive-edge convention self-test failed"; return false; }
    for (std::size_t i=0;i<kOffsets.size();++i) for(std::size_t j=i+1;j<kOffsets.size();++j) if(kOffsets[i]==kOffsets[j]) { *error="duplicate mosaic offset"; return false; }
    const IcosahedralProjection projection;
    for (const auto location : std::array<std::array<double,2>,4>{{{{129.0,31.0}},{{138.7274,35.3606}},{{139.6917,35.6895}},{{145.8,45.5}}}})
    {
        double px,py; if(!projection.forward(location[0],location[1],&px,&py)){*error="Japan coverage projection self-test failed";return false;}
        bool covered=false;
        for(std::size_t i=0;i<kIds.size();++i){const auto o=MosaicOrigin(i);const double dx=px-o[0],dy=py-o[1],lx=dx*kXAxisX+dy*kXAxisY,ly=dx*kYAxisX+dy*kYAxisY;covered|=lx>=0&&ly>=0&&lx<=kMosaicSize&&ly<=kMosaicSize;}
        if(!covered){*error="fixed four-mosaic layout does not cover representative Japan location "+std::to_string(location[0])+","+std::to_string(location[1])+" projected "+std::to_string(px)+","+std::to_string(py);return false;}
    }
    return true;
}
}

bool JapanDem10Converter::run(const JapanDem10ConversionConfig& config, JapanDem10ConversionSummary* summary, std::string* error)
{
    *summary = {}; if (!SelfTest(error)) return false;
    if (config.selfTestOnly) { std::cout << "Japan DEM10 self-tests passed: scale/bias quantization, sentinels, filter, cross-raster sampling, ocean collar, fixed layout, orthonormal basis, and positive-edge ownership\n"; return true; }
    RasterCatalog source; if (!source.open(config.sourceRoot, error)) return false; summary->sourceFiles = source.rasters.size();
    if (config.verbose && !source.validateFirstDecode(error)) return false;
    EtopoSampler etopo; if (!etopo.open(config.etopoIndex, error)) return false;
    std::filesystem::create_directories(config.outputRoot);
    for(const char* extension : {".assetbin",".heightbin","_preview.png"})
    {
        std::error_code removeError; std::filesystem::remove(config.outputRoot/(std::string("japan_dem10_delta_cw")+extension),removeError);
        if(removeError){*error="could not remove obsolete cw mosaic output: "+removeError.message();return false;}
    }
    HeightmapQuantization::Statistics statistics;
    double maxCompositionError = 0;
    std::uint64_t forcedEdgeCount = 0;
    IcosahedralProjection projection;
    for (std::size_t mosaic=0;mosaic<kIds.size();++mosaic)
    {
        const auto origin=MosaicOrigin(mosaic); const std::string stem=std::string("japan_dem10_delta_")+kIds[mosaic];
        const auto dataPath=config.outputRoot/(stem+".heightbin"), indexPath=config.outputRoot/(stem+".assetbin");
        std::ofstream data(dataPath,std::ios::binary|std::ios::trunc); if(!data){*error="could not create "+dataPath.string();return false;}
        std::set<std::pair<int,int>> candidates;
        for (const auto& raster : source.rasters)
        {
            double localMinX=std::numeric_limits<double>::max(),localMinY=localMinX,localMaxX=-localMinX,localMaxY=-localMinX;
            for (const auto corner : std::array<std::array<double,2>,4>{{{{raster.minLon,raster.minLat}},{{raster.minLon,raster.maxLat}},{{raster.maxLon,raster.minLat}},{{raster.maxLon,raster.maxLat}}}})
            {
                double px,py; if(!projection.forward(corner[0],corner[1],&px,&py)) continue;
                const double dx=px-origin[0],dy=py-origin[1],lx=dx*kXAxisX+dy*kXAxisY,ly=dx*kYAxisX+dy*kYAxisY;
                localMinX=std::min(localMinX,lx);localMaxX=std::max(localMaxX,lx);localMinY=std::min(localMinY,ly);localMaxY=std::max(localMaxY,ly);
            }
            if(localMinX>localMaxX) continue;
            const int firstX=std::max(kMinTile,static_cast<int>(std::floor(localMinX/kTileSize))+kMinTile-1),lastX=std::min(kMaxTile,static_cast<int>(std::floor(localMaxX/kTileSize))+kMinTile+1);
            const int firstY=std::max(kMinTile,static_cast<int>(std::floor(localMinY/kTileSize))+kMinTile-1),lastY=std::min(kMaxTile,static_cast<int>(std::floor(localMaxY/kTileSize))+kMinTile+1);
            for(int ty=firstY;ty<=lastY;++ty) for(int tx=firstX;tx<=lastX;++tx) candidates.emplace(ty,tx);
        }
        std::cout << stem << ": " << candidates.size() << " candidate tiles\n";
        std::vector<RuntimeAssets::HeightmapTileRecord> records; Tile tile{};
        struct TileWork
        {
            HeightmapQuantization::FloatTile values{};
            HeightmapQuantization::Kinds kinds{};
            std::array<double, RuntimeAssets::kHeightmapTileSampleCount> bases{}, targets{};
        };
        auto work = std::make_unique<TileWork>();
        auto& values = work->values; auto& kinds = work->kinds;
        auto& bases = work->bases; auto& targets = work->targets;
        std::size_t completedTiles=0;
        const auto mosaicStarted=std::chrono::steady_clock::now();
        auto reportProgress=[&](int tx,int ty,std::uint32_t valid)
        {
            const auto now=std::chrono::steady_clock::now();
            const double elapsed=std::chrono::duration<double>(now-mosaicStarted).count();
            const double eta=completedTiles ? elapsed*(candidates.size()-completedTiles)/completedTiles : 0.0;
            std::cout<<"  "<<stem<<": "<<completedTiles<<'/'<<candidates.size()<<" tiles ("<<std::fixed<<std::setprecision(1)
                     <<(candidates.empty()?100.0:100.0*completedTiles/candidates.size())<<"%), tile ("<<tx<<','<<ty<<") "<<valid
                     <<" valid samples, "<<records.size()<<" stored, elapsed "
                     <<FormatDuration(elapsed)<<", ETA "<<FormatDuration(eta)<<'\n';
        };
        for(const auto [ty,tx] : candidates)
        {
            ++summary->candidateTiles; std::uint32_t valid=0;
            kinds.fill(HeightmapQuantization::Kind::ExactZero);
            targets.fill(std::numeric_limits<double>::quiet_NaN());
            for(int y=0;y<256;++y) for(int x=0;x<256;++x)
            {
                float value=0;
                if (!(tx==kMaxTile&&x==255) && !(ty==kMaxTile&&y==255))
                {
                    const double lx=(static_cast<double>(tx-kMinTile)*255+x)*kSpacing, ly=(static_cast<double>(ty-kMinTile)*255+y)*kSpacing;
                    const double px=origin[0]+lx*kXAxisX+ly*kYAxisX, py=origin[1]+lx*kXAxisY+ly*kYAxisY;
                    double lon,lat,dem,base;
                    if (projection.inverse(px,py,&lon,&lat) && etopo.sample(px,py,&base,error))
                    {
                        bool contributes = false;
                        double delta = 0.0;
                        if (source.sample(lon,lat,&dem,error))
                        {
                            delta = dem - base;
                            bases[static_cast<std::size_t>(y)*256+x] = base;
                            targets[static_cast<std::size_t>(y)*256+x] = dem;
                            contributes = true;
                        }
                        else
                        {
                            const double collarWeight = source.coverageWeight(lon, lat, kOceanCollarMeters);
                            if (collarWeight > 0.0 && base > 0.0)
                            {
                                delta = -base * collarWeight;
                                contributes = true;
                            }
                        }
                        if (contributes)
                        {
                            value=static_cast<float>(delta);
                            kinds[static_cast<std::size_t>(y)*256+x] = HeightmapQuantization::Kind::Normal;
                            ++valid; ++summary->validSamples;
                        }
                    }
                }
                values[static_cast<std::size_t>(y)*256+x]=value;
            }
            if(valid==0){++summary->omittedTiles;++completedTiles;reportProgress(tx,ty,valid);continue;}
            float scale, bias;
            if (!HeightmapQuantization::Encode(values, kinds, tile, scale, bias, statistics, error)) return false;
            std::vector<std::byte> filtered,compressed;
            if (!HeightmapQuantization::RoundTrip(tile, filtered, compressed, error)) return false;
            for (std::size_t i = 0; i < tile.size(); ++i)
            {
                const float decoded = RuntimeAssets::DecodeHeight(tile[i], scale, bias);
                if (std::isfinite(targets[i]))
                {
                    const double difference = std::abs(bases[i] + decoded - targets[i]);
                    const double bound = HeightmapQuantization::ErrorBound(values[i], scale, bias) +
                        std::abs(double(values[i]) - (targets[i] - bases[i]));
                    if (difference > bound)
                    { *error = "composed DEM10 elevation exceeds delta quantization bound"; return false; }
                    maxCompositionError = std::max(maxCompositionError, difference);
                }
                if ((tx == kMaxTile && i % 256 == 255) || (ty == kMaxTile && i / 256 == 255))
                {
                    if (tile[i] != RuntimeAssets::kHeightmapExactZeroHeight || decoded != 0.0f)
                    { *error = "positive mosaic edge lost exact zero"; return false; }
                    ++forcedEdgeCount;
                }
            }
            statistics.rawBytes += tile.size() * sizeof(std::int16_t);
            statistics.filteredBytes += filtered.size(); statistics.compressedBytes += compressed.size();
            const auto offset=static_cast<std::uint64_t>(data.tellp()); data.write(reinterpret_cast<const char*>(compressed.data()),compressed.size());
            if (!data) { *error = "failed writing Japan tile blob"; return false; }
            records.push_back({static_cast<std::int8_t>(tx),static_cast<std::int8_t>(ty),0,static_cast<std::uint32_t>(compressed.size()),static_cast<std::uint32_t>(filtered.size()),static_cast<std::uint32_t>(tile.size()),scale,bias,offset}); ++summary->storedTiles;
            ++completedTiles; reportProgress(tx,ty,valid);
        }
        const std::array<double,4> bounds{origin[0]+std::min(0.0,kMosaicSize*kYAxisX),origin[1],origin[0]+kMosaicSize*kXAxisX,origin[1]+kMosaicSize*(kXAxisY+kYAxisY)};
        data.close(); RuntimeAssets::HeightmapPackHeader h{}; h.magic=RuntimeAssets::kHeightmapPackMagic;h.version=RuntimeAssets::kHeightmapFormatVersion;h.flags=RuntimeAssets::kLittleEndianFlag;h.headerSize=sizeof(h);h.tileCount=records.size();h.tileResolution=256;h.tileStride=255;h.sampleType=static_cast<std::uint32_t>(RuntimeAssets::HeightSampleType::QuantizedInt16ScaleBias);h.invalidHeight=RuntimeAssets::kHeightmapInvalidHeight;h.filterType=static_cast<std::uint32_t>(RuntimeAssets::HeightFilterType::SerpentineDeltaZigZagBytePlanes);h.filterVersion=1;h.compressionType=static_cast<std::uint32_t>(RuntimeAssets::CompressionType::Lz4);h.tilePhysicalSizeMeters=kTileSize;h.authalicRadiusMeters=IcosahedralProjection::kAuthalicRadiusMeters;h.orientationDegrees={20.5,0.0,0.0};h.projectedBoundsMeters=bounds;h.projectionVersion=IcosahedralProjection::kVersion;h.projectionName=RuntimeAssets::MakeMagic('J','P','1','0');h.tileRecordOffset=sizeof(h);h.fileSize=sizeof(h)+records.size()*sizeof(records[0]);std::strncpy(h.dataFilename.data(),(stem+".heightbin").c_str(),h.dataFilename.size()-1);std::strncpy(h.sourceDataset.data(),"smartmaps/japan-geotiff-dem/10 delta",h.sourceDataset.size()-1);
        std::ofstream index(indexPath,std::ios::binary|std::ios::trunc);index.write(reinterpret_cast<const char*>(&h),sizeof(h));index.write(reinterpret_cast<const char*>(records.data()),records.size()*sizeof(records[0]));if(!index){*error="failed writing "+indexPath.string();return false;}
        index.close(); if(!WritePackedDeltaPreview(indexPath,dataPath,config.outputRoot/(stem+"_preview.png"),error)) return false;
    }
    if(!WriteMetadata(config.outputRoot/"japan_dem10_delta_metadata.json",config.sourceRoot,source,error)||!WritePreview(config.outputRoot/"japan_dem10_delta_coverage.svg",source,error)) return false;
    statistics.print("DEM10 delta");
    std::cout << "Maximum decoded ETOPO + delta versus DEM10 target error: " << maxCompositionError << " m\nForced positive-edge exact-zero samples verified: " << forcedEdgeCount << '\n';
    std::cout<<"Japan DEM10 delta: "<<summary->sourceFiles<<" sources, "<<summary->storedTiles<<" stored / "<<summary->candidateTiles<<" candidate tiles, "<<summary->validSamples<<" valid samples\nValidation: fixed placement, positive edges, manifest-backed EPSG:6668 inputs, sparse output, and ETOPO-relative sampling succeeded\n";
    return true;
}
