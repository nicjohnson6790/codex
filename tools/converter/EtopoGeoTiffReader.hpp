#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class EtopoGeoTiffReader
{
public:
    bool open(const std::filesystem::path& path, std::string* error);
    [[nodiscard]] bool sampleBilinear(double longitudeDegrees, double latitudeDegrees, double* elevationMeters) const;
    [[nodiscard]] std::uint32_t width() const { return m_width; }
    [[nodiscard]] std::uint32_t height() const { return m_height; }
    [[nodiscard]] double minLongitude() const { return m_minLongitude; }
    [[nodiscard]] double maxLongitude() const { return m_maxLongitude; }
    [[nodiscard]] double minLatitude() const { return m_minLatitude; }
    [[nodiscard]] double maxLatitude() const { return m_maxLatitude; }
    [[nodiscard]] float sourceMin() const { return m_sourceMin; }
    [[nodiscard]] float sourceMax() const { return m_sourceMax; }
    [[nodiscard]] const std::string& sourceSampleDescription() const { return m_sourceSampleDescription; }

private:
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    double m_minLongitude = 0.0;
    double m_maxLongitude = 0.0;
    double m_minLatitude = 0.0;
    double m_maxLatitude = 0.0;
    double m_pixelWidth = 0.0;
    double m_pixelHeight = 0.0;
    float m_sourceMin = 0;
    float m_sourceMax = 0;
    std::string m_sourceSampleDescription;
    std::vector<float> m_samples;
};
