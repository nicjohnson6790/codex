#include "EtopoGeoTiffReader.hpp"

#include <tiffio.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace
{
struct TiffCloser { void operator()(TIFF* value) const { if (value) TIFFClose(value); } };
}

bool EtopoGeoTiffReader::open(const std::filesystem::path& path, std::string* error)
{
    m_samples.clear();
    std::unique_ptr<TIFF, TiffCloser> tiff(TIFFOpen(path.string().c_str(), "r"));
    if (!tiff)
    {
        if (error) *error = "could not open ETOPO GeoTIFF: " + path.string();
        return false;
    }

    std::uint16_t samplesPerPixel = 0, bitsPerSample = 0, sampleFormat = 0, planarConfig = 0;
    TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLESPERPIXEL, &samplesPerPixel);
    TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_BITSPERSAMPLE, &bitsPerSample);
    TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_SAMPLEFORMAT, &sampleFormat);
    TIFFGetFieldDefaulted(tiff.get(), TIFFTAG_PLANARCONFIG, &planarConfig);
    if (!TIFFGetField(tiff.get(), TIFFTAG_IMAGEWIDTH, &m_width) ||
        !TIFFGetField(tiff.get(), TIFFTAG_IMAGELENGTH, &m_height) ||
        m_width < 20000 || m_height < 10000 || samplesPerPixel != 1 || planarConfig != PLANARCONFIG_CONTIG ||
        !((sampleFormat == SAMPLEFORMAT_INT && (bitsPerSample == 16 || bitsPerSample == 32)) ||
          (sampleFormat == SAMPLEFORMAT_IEEEFP && (bitsPerSample == 32 || bitsPerSample == 64))))
    {
        if (error) *error = "GeoTIFF is not a global, single-band signed numeric ETOPO-style raster";
        return false;
    }
    m_sourceSampleDescription = sampleFormat == SAMPLEFORMAT_INT ? "signed int" : "IEEE float";
    m_sourceSampleDescription += std::to_string(bitsPerSample);

    double* scale = nullptr;
    double* tiePoints = nullptr;
    std::uint16_t scaleCount = 0, tieCount = 0;
    constexpr ttag_t modelPixelScaleTag = 33550;
    constexpr ttag_t modelTiepointTag = 33922;
    if (!TIFFGetField(tiff.get(), modelPixelScaleTag, &scaleCount, &scale) || scaleCount < 2 ||
        !TIFFGetField(tiff.get(), modelTiepointTag, &tieCount, &tiePoints) || tieCount < 6)
    {
        if (error) *error = "GeoTIFF lacks ModelPixelScale/ModelTiepoint longitude-latitude georeferencing";
        return false;
    }
    m_pixelWidth = scale[0];
    m_pixelHeight = scale[1];
    m_minLongitude = tiePoints[3] - tiePoints[0] * m_pixelWidth;
    m_maxLatitude = tiePoints[4] + tiePoints[1] * m_pixelHeight;
    m_maxLongitude = m_minLongitude + static_cast<double>(m_width) * m_pixelWidth;
    m_minLatitude = m_maxLatitude - static_cast<double>(m_height) * m_pixelHeight;
    if (std::abs(m_minLongitude + 180.0) > 0.1 || std::abs(m_maxLongitude - 180.0) > 0.1 ||
        std::abs(m_minLatitude + 90.0) > 0.1 || std::abs(m_maxLatitude - 90.0) > 0.1 ||
        std::abs(m_pixelWidth - m_pixelHeight) > 1e-10)
    {
        if (error) *error = "GeoTIFF bounds are not the expected global longitude/latitude coverage";
        return false;
    }

    const std::size_t sampleCount = static_cast<std::size_t>(m_width) * m_height;
    try { m_samples.resize(sampleCount); }
    catch (const std::bad_alloc&)
    {
        if (error) *error = "not enough memory to cache the ETOPO source raster";
        return false;
    }
    m_sourceMin = INT16_MAX;
    m_sourceMax = INT16_MIN;
    const auto convertSample = [sampleFormat, bitsPerSample](const std::byte* bytes, std::size_t index, double* value)
    {
        if (sampleFormat == SAMPLEFORMAT_INT && bitsPerSample == 16)
            *value = reinterpret_cast<const std::int16_t*>(bytes)[index];
        else if (sampleFormat == SAMPLEFORMAT_INT && bitsPerSample == 32)
            *value = reinterpret_cast<const std::int32_t*>(bytes)[index];
        else if (bitsPerSample == 32)
            *value = reinterpret_cast<const float*>(bytes)[index];
        else
            *value = reinterpret_cast<const double*>(bytes)[index];
    };
    const auto storeSample = [this, error](std::uint32_t x, std::uint32_t y, double value)
    {
        if (!std::isfinite(value))
        {
            if (error) *error = "source elevation is non-finite at sample (" + std::to_string(x) + "," + std::to_string(y) + ")";
            m_samples.clear();
            return false;
        }
        const long rounded = std::lround(value);
        if (rounded <= INT16_MIN || rounded > INT16_MAX)
        {
            if (error) *error = "source elevation rounds to the reserved sentinel or outside signed 16-bit meters at sample (" +
                std::to_string(x) + "," + std::to_string(y) + ")";
            m_samples.clear();
            return false;
        }
        m_samples[static_cast<std::size_t>(y) * m_width + x] = static_cast<std::int16_t>(rounded);
        return true;
    };

    if (TIFFIsTiled(tiff.get()))
    {
        std::uint32_t tileWidth = 0, tileHeight = 0;
        TIFFGetField(tiff.get(), TIFFTAG_TILEWIDTH, &tileWidth);
        TIFFGetField(tiff.get(), TIFFTAG_TILELENGTH, &tileHeight);
        const tmsize_t tileBytes = TIFFTileSize(tiff.get());
        if (tileWidth == 0 || tileHeight == 0 || tileBytes <= 0)
        { if (error) *error = "GeoTIFF tile layout is invalid"; return false; }
        std::vector<std::byte> sourceTile(static_cast<std::size_t>(tileBytes));
        for (std::uint32_t tileY = 0; tileY < m_height; tileY += tileHeight)
        for (std::uint32_t tileX = 0; tileX < m_width; tileX += tileWidth)
        {
            if (TIFFReadTile(tiff.get(), sourceTile.data(), tileX, tileY, 0, 0) < 0)
            { if (error) *error = "failed reading GeoTIFF tile at (" + std::to_string(tileX) + "," + std::to_string(tileY) + ")"; return false; }
            const std::uint32_t copyWidth = std::min(tileWidth, m_width - tileX);
            const std::uint32_t copyHeight = std::min(tileHeight, m_height - tileY);
            for (std::uint32_t y = 0; y < copyHeight; ++y)
            for (std::uint32_t x = 0; x < copyWidth; ++x)
            {
                double value = 0.0;
                convertSample(sourceTile.data(), static_cast<std::size_t>(y) * tileWidth + x, &value);
                if (!storeSample(tileX + x, tileY + y, value)) return false;
            }
        }
    }
    else
    {
        const tmsize_t scanlineBytes = TIFFScanlineSize(tiff.get());
        if (scanlineBytes <= 0 || static_cast<std::size_t>(scanlineBytes) < static_cast<std::size_t>(m_width) * bitsPerSample / 8u)
        { if (error) *error = "GeoTIFF scanline size is invalid"; return false; }
        std::vector<std::byte> sourceRow(static_cast<std::size_t>(scanlineBytes));
        for (std::uint32_t y = 0; y < m_height; ++y)
        {
            if (TIFFReadScanline(tiff.get(), sourceRow.data(), y, 0) < 0)
            { if (error) *error = "failed reading GeoTIFF scanline " + std::to_string(y); return false; }
            for (std::uint32_t x = 0; x < m_width; ++x)
            {
                double value = 0.0;
                convertSample(sourceRow.data(), x, &value);
                if (!storeSample(x, y, value)) return false;
            }
        }
    }
    const auto [minimum, maximum] = std::minmax_element(m_samples.begin(), m_samples.end());
    m_sourceMin = *minimum;
    m_sourceMax = *maximum;
    return true;
}

bool EtopoGeoTiffReader::sampleBilinear(double longitudeDegrees, double latitudeDegrees, double* elevationMeters) const
{
    if (m_samples.empty() || !std::isfinite(longitudeDegrees) || !std::isfinite(latitudeDegrees)) return false;
    longitudeDegrees = std::fmod(longitudeDegrees + 180.0, 360.0);
    if (longitudeDegrees < 0.0) longitudeDegrees += 360.0;
    longitudeDegrees -= 180.0;
    latitudeDegrees = std::clamp(latitudeDegrees, -90.0, 90.0);

    double pixelX = (longitudeDegrees - m_minLongitude) / m_pixelWidth - 0.5;
    double pixelY = (m_maxLatitude - latitudeDegrees) / m_pixelHeight - 0.5;
    const auto x0raw = static_cast<std::int64_t>(std::floor(pixelX));
    const auto y0raw = static_cast<std::int64_t>(std::floor(pixelY));
    const double fx = pixelX - std::floor(pixelX);
    const double fy = pixelY - std::floor(pixelY);
    const auto wrapX = [this](std::int64_t x) { x %= m_width; if (x < 0) x += m_width; return static_cast<std::uint32_t>(x); };
    const std::uint32_t x0 = wrapX(x0raw), x1 = wrapX(x0raw + 1);
    const std::uint32_t y0 = static_cast<std::uint32_t>(std::clamp<std::int64_t>(y0raw, 0, m_height - 1));
    const std::uint32_t y1 = static_cast<std::uint32_t>(std::clamp<std::int64_t>(y0raw + 1, 0, m_height - 1));
    const auto at = [this](std::uint32_t x, std::uint32_t y) { return static_cast<double>(m_samples[static_cast<std::size_t>(y) * m_width + x]); };
    const double top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
    const double bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
    *elevationMeters = top + (bottom - top) * fy;
    return true;
}
