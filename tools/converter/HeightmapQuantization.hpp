#pragma once

#include "assets/RuntimeHeightmapFormat.hpp"
#include "assets/RuntimeAssetCompression.hpp"
#include "HeightmapTileFilter.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <vector>
#include <lz4hc.h>

namespace HeightmapQuantization
{
enum class Kind : unsigned char { Normal, Invalid, ExactZero };
using FloatTile = std::array<float, RuntimeAssets::kHeightmapTileSampleCount>;
using Kinds = std::array<Kind, RuntimeAssets::kHeightmapTileSampleCount>;
using EncodedTile = std::array<std::int16_t, RuntimeAssets::kHeightmapTileSampleCount>;
constexpr int qMin = -32766, qMax = 32767;

inline double ErrorBound(float value, float scale, float bias)
{
    // Half a step plus float multiply/add rounding in the runtime decoder.
    return 0.5 * scale + 4.0 * std::numeric_limits<float>::epsilon() *
        (std::abs(double(value)) + std::abs(double(bias)) + 32767.0 * scale);
}

struct Statistics
{
    double minimum = std::numeric_limits<double>::infinity(), maximum = -minimum;
    double decodedMinimum = minimum, decodedMaximum = maximum;
    float minStep = std::numeric_limits<float>::infinity(), maxStep = 0;
    double maxError = 0, squaredError = 0;
    std::uint64_t count = 0, exactZeros = 0, rawBytes = 0, filteredBytes = 0, compressedBytes = 0;

    void merge(const Statistics& other)
    {
        minimum = std::min(minimum, other.minimum); maximum = std::max(maximum, other.maximum);
        decodedMinimum = std::min(decodedMinimum, other.decodedMinimum); decodedMaximum = std::max(decodedMaximum, other.decodedMaximum);
        minStep = std::min(minStep, other.minStep); maxStep = std::max(maxStep, other.maxStep);
        maxError = std::max(maxError, other.maxError); squaredError += other.squaredError;
        count += other.count; exactZeros += other.exactZeros;
        rawBytes += other.rawBytes; filteredBytes += other.filteredBytes; compressedBytes += other.compressedBytes;
    }

    void print(const char* label) const
    {
        std::cout << std::setprecision(9) << label << " floating-point range: " << minimum << ".." << maximum
            << " m; decoded range: " << decodedMinimum << ".." << decodedMaximum
            << " m\nQuantization step: " << minStep << ".." << maxStep
            << " m; maximum absolute error: " << maxError << " m; RMS error: "
            << (count ? std::sqrt(squaredError / count) : 0) << " m\nExact-zero samples verified: " << exactZeros
            << "\nRaw/filtered/compressed bytes: " << rawBytes << " / " << filteredBytes << " / " << compressedBytes << '\n';
    }
};

inline bool Encode(const FloatTile& values, const Kinds& kinds, EncodedTile& encoded,
    float& scale, float& bias, Statistics& stats, std::string* error)
{
    auto fail = [&](const char* message) { if (error) *error = message; return false; };
    float low = std::numeric_limits<float>::infinity(), high = -low;
    for (std::size_t i = 0; i < values.size(); ++i)
        if (kinds[i] == Kind::Normal)
        {
            if (!std::isfinite(values[i])) return fail("non-finite normal height sample");
            low = std::min(low, values[i]); high = std::max(high, values[i]);
        }
    scale = 0; bias = std::isfinite(low) ? low : 0;
    if (low < high)
    {
        scale = static_cast<float>((double(high) - low) / (qMax - qMin));
        if (scale == 0) scale = std::numeric_limits<float>::denorm_min();
        // Center using stored float metadata, expanding only when rounding would
        // put an endpoint outside the normal code domain. Never clamp samples.
        for (;;)
        {
            bias = static_cast<float>((double(low) + high - double(scale)) * 0.5);
            if (!std::isfinite(scale) || !std::isfinite(bias)) return fail("unrepresentable height quantization metadata");
            const double a = std::round((double(low) - bias) / scale);
            const double b = std::round((double(high) - bias) / scale);
            if (a >= qMin && b <= qMax) break;
            scale = std::nextafter(scale, std::numeric_limits<float>::infinity());
        }
    }
    stats.minStep = std::min(stats.minStep, scale); stats.maxStep = std::max(stats.maxStep, scale);
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        if (kinds[i] == Kind::Invalid) { encoded[i] = RuntimeAssets::kHeightmapInvalidHeight; continue; }
        if (kinds[i] == Kind::ExactZero) encoded[i] = RuntimeAssets::kHeightmapExactZeroHeight;
        else
        {
            const double q = scale == 0 ? 0 : std::round((double(values[i]) - bias) / scale);
            if (q < qMin || q > qMax) return fail("height sample exceeds quantization domain");
            encoded[i] = static_cast<std::int16_t>(q);
        }
        const float reconstructed = RuntimeAssets::DecodeHeight(encoded[i], scale, bias);
        const float target = kinds[i] == Kind::ExactZero ? 0.0f : values[i];
        const double difference = std::abs(double(reconstructed) - target);
        if (!std::isfinite(reconstructed) || difference > ErrorBound(target, scale, bias))
            return fail("height quantization error exceeds expected bound");
        if (kinds[i] == Kind::ExactZero)
        {
            if (reconstructed != 0.0f) return fail("exact zero did not decode to zero");
            ++stats.exactZeros;
        }
        stats.minimum = std::min(stats.minimum, double(target)); stats.maximum = std::max(stats.maximum, double(target));
        stats.decodedMinimum = std::min(stats.decodedMinimum, double(reconstructed));
        stats.decodedMaximum = std::max(stats.decodedMaximum, double(reconstructed));
        stats.maxError = std::max(stats.maxError, difference); stats.squaredError += difference * difference; ++stats.count;
    }
    return true;
}

inline bool CompressFiltered(std::span<const std::byte> filtered, std::vector<std::byte>& compressed, std::string* error)
{
    if (filtered.size() != RuntimeAssets::kHeightmapFilteredTileBytes) return false;
    // Keep the existing stateless HC API and level. The full output bound lets
    // failures report the actual size, without changing the <=128 KiB policy.
    compressed.resize(LZ4_compressBound(static_cast<int>(filtered.size())));
    const int size = LZ4_compress_HC(reinterpret_cast<const char*>(filtered.data()), reinterpret_cast<char*>(compressed.data()),
        static_cast<int>(filtered.size()), static_cast<int>(compressed.size()), LZ4HC_CLEVEL_MAX);
    if (size <= 0 || static_cast<std::size_t>(size) > filtered.size())
    {
        if (error) *error = "LZ4_HC heightmap tile compressed size " + std::to_string(size) +
            " bytes exceeds the 131072-byte runtime bound or compression failed";
        compressed.clear();
        return false;
    }
    compressed.resize(size);
    return true;
}

inline bool RoundTrip(const EncodedTile& tile, std::vector<std::byte>& filtered,
    std::vector<std::byte>& compressed, std::vector<std::byte>& restored,
    std::vector<std::int16_t>& decoded, std::string* error)
{
    if (!HeightmapTileFilter::Encode(tile, &filtered, error) ||
        !CompressFiltered(filtered, compressed, error) ||
        !RuntimeAssets::DecompressBytes(RuntimeAssets::CompressionType::Lz4, compressed, filtered.size(), &restored, error) ||
        !HeightmapTileFilter::Decode(restored, &decoded, error)) return false;
    if (!std::equal(tile.begin(), tile.end(), decoded.begin(), decoded.end()))
    { if (error) *error = "quantized tile compression round-trip failed"; return false; }
    return true;
}

inline bool RoundTrip(const EncodedTile& tile, std::vector<std::byte>& filtered,
    std::vector<std::byte>& compressed, std::string* error)
{
    std::vector<std::byte> restored;
    std::vector<std::int16_t> decoded;
    return RoundTrip(tile, filtered, compressed, restored, decoded, error);
}

inline bool SelfTest(std::string* error)
{
    FloatTile values{};
    Kinds kinds{};
    EncodedTile encoded{};
    float scale, bias;
    for (const auto range : std::array<std::array<float, 2>, 9>{{
        {1.125f, 789.625f}, {-901.25f, -2.125f}, {-11000.125f, 8800.75f},
        {7.25f, 7.25f}, {-7.25f, -7.25f}, {0, 0},
        {12345.125f, 12345.126f}, {-0.00001f, 0.00003f}, {1000000.0f, 1000000.125f}}})
    {
        Statistics stats;
        kinds.fill(Kind::Normal);
        for (std::size_t i = 0; i < values.size(); ++i)
            values[i] = static_cast<float>(double(range[0]) + (double(range[1]) - range[0]) * i / (values.size() - 1));
        if (!Encode(values, kinds, encoded, scale, bias, stats, error)) return false;
        for (auto q : encoded) if (q < qMin) { if (error) *error = "normal sample encoded as sentinel"; return false; }
        if (range[0] == range[1] && (scale != 0.0f || bias != range[0] || encoded[0] != 0))
        { if (error) *error = "flat tile did not retain its exact height"; return false; }
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            // Check the ideal decoded value in double independently of the
            // float runtime decode and its additional arithmetic roundoff.
            const double affine = double(bias) + double(encoded[i]) * scale;
            if (std::abs(affine - values[i]) > 0.5 * scale)
            { if (error) *error = "stored float metadata violates half-step quantization bound"; return false; }
        }
        // Include both sentinels and the complete positive-edge ownership shape.
        kinds[1] = Kind::Invalid;
        for (std::size_t i = 0; i < 256; ++i) kinds[255 * 256 + i] = kinds[i * 256 + 255] = Kind::ExactZero;
        if (!Encode(values, kinds, encoded, scale, bias, stats, error)) return false;
        std::vector<std::byte> filtered, compressed;
        if (!RoundTrip(encoded, filtered, compressed, error)) return false;
        if (encoded[1] != RuntimeAssets::kHeightmapInvalidHeight) return false;
        for (std::size_t i = 0; i < 256; ++i)
            if (encoded[255 * 256 + i] != RuntimeAssets::kHeightmapExactZeroHeight ||
                encoded[i * 256 + 255] != RuntimeAssets::kHeightmapExactZeroHeight ||
                RuntimeAssets::DecodeHeight(encoded[i * 256 + 255], scale, bias) != 0.0f) return false;
    }
    for (float metadata : {0.0f, 123.0f, -456.0f, std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::quiet_NaN()})
        if (RuntimeAssets::DecodeHeight(RuntimeAssets::kHeightmapExactZeroHeight, metadata, metadata) != 0.0f)
        { if (error) *error = "exact-zero decode depends on metadata"; return false; }
    values.fill(0.0f); kinds.fill(Kind::Normal);
    values[0] = float(qMin); values[1] = float(qMax);
    Statistics endpointStats;
    if (!Encode(values, kinds, encoded, scale, bias, endpointStats, error)) return false;
    if (scale != 1.0f || bias != 0.0f || encoded[0] != qMin || encoded[1] != qMax)
    { if (error) *error = "normal quantization endpoints are not available"; return false; }
    // The same shared sample in independently quantized neighboring ranges.
    values.fill(12.345f); values[0] = -900.25f; values[1] = 700.75f; kinds.fill(Kind::Normal);
    Statistics stats;
    if (!Encode(values, kinds, encoded, scale, bias, stats, error)) return false;
    const float a = RuntimeAssets::DecodeHeight(encoded[2], scale, bias);
    const double boundA = ErrorBound(values[2], scale, bias);
    values[0] = -4.5f; values[1] = 100.25f;
    if (!Encode(values, kinds, encoded, scale, bias, stats, error)) return false;
    if (std::abs(double(a) - RuntimeAssets::DecodeHeight(encoded[2], scale, bias)) >
        boundA + ErrorBound(values[2], scale, bias))
    { if (error) *error = "independent shared-border quantization failed"; return false; }
    kinds.fill(Kind::Invalid);
    return Encode(values, kinds, encoded, scale, bias, stats, error);
}
}
