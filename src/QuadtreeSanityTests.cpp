#include "AppConfig.hpp"
#include "HeightmapNoiseGenerator.hpp"
#include "WorldGridQuadtreeTypes.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace
{
constexpr std::size_t kResolution = AppConfig::Terrain::kHeightmapResolution;
constexpr std::size_t kLeafSampleStart = AppConfig::Terrain::kHeightmapLeafHalo;
constexpr std::size_t kLeafSampleStop = AppConfig::Terrain::kHeightmapResolution - AppConfig::Terrain::kHeightmapLeafHalo;
constexpr std::size_t kRenderedSampleStart = kLeafSampleStart + AppConfig::Terrain::kRenderedPatchInset;
constexpr std::size_t kRenderedSampleStop = kLeafSampleStop - AppConfig::Terrain::kRenderedPatchInset;

std::vector<float> buildHeightmap(const WorldGridQuadtreeLeafId& leafId)
{
    HeightmapNoiseGenerator generator;
    std::vector<float> heightmap(kResolution * kResolution);
    const auto [a, b] = worldGridQuadtreeLeafBounds(leafId);
    generator.fillNoise(a, b, heightmap.data());
    return heightmap;
}

std::uint64_t appendChildren(std::initializer_list<std::uint32_t> quadrants)
{
    std::uint64_t path = 0;
    for (const std::uint32_t quadrant : quadrants)
    {
        path = WorldGridQuadtreeLeafId::appendChild(path, quadrant);
    }
    return path;
}

float maxSameLevelVerticalEdgeError(const WorldGridQuadtreeLeafId& leftLeaf, const WorldGridQuadtreeLeafId& rightLeaf)
{
    const std::vector<float> left = buildHeightmap(leftLeaf);
    const std::vector<float> right = buildHeightmap(rightLeaf);

    float maxError = 0.0f;
    for (std::size_t row = 0; row < kResolution; ++row)
    {
        const float leftValue = left[(row * kResolution) + (kLeafSampleStop - 1)];
        const float rightValue = right[(row * kResolution) + kLeafSampleStart];
        maxError = std::max(maxError, std::fabs(leftValue - rightValue));
    }
    return maxError;
}

float maxRenderedVerticalEdgeError(const WorldGridQuadtreeLeafId& leftLeaf, const WorldGridQuadtreeLeafId& rightLeaf)
{
    const std::vector<float> left = buildHeightmap(leftLeaf);
    const std::vector<float> right = buildHeightmap(rightLeaf);

    float maxError = 0.0f;
    for (std::size_t row = kRenderedSampleStart; row < kRenderedSampleStop; ++row)
    {
        const float leftValue = left[(row * kResolution) + (kRenderedSampleStop - 1)];
        const float rightValue = right[(row * kResolution) + kRenderedSampleStart];
        maxError = std::max(maxError, std::fabs(leftValue - rightValue));
    }
    return maxError;
}

float maxSameLevelHorizontalEdgeError(const WorldGridQuadtreeLeafId& bottomLeaf, const WorldGridQuadtreeLeafId& topLeaf)
{
    const std::vector<float> bottom = buildHeightmap(bottomLeaf);
    const std::vector<float> top = buildHeightmap(topLeaf);

    float maxError = 0.0f;
    for (std::size_t column = 0; column < kResolution; ++column)
    {
        const float bottomValue = bottom[((kLeafSampleStop - 1) * kResolution) + column];
        const float topValue = top[(kLeafSampleStart * kResolution) + column];
        maxError = std::max(maxError, std::fabs(bottomValue - topValue));
    }
    return maxError;
}

float maxRenderedHorizontalEdgeError(const WorldGridQuadtreeLeafId& bottomLeaf, const WorldGridQuadtreeLeafId& topLeaf)
{
    const std::vector<float> bottom = buildHeightmap(bottomLeaf);
    const std::vector<float> top = buildHeightmap(topLeaf);

    float maxError = 0.0f;
    for (std::size_t column = kRenderedSampleStart; column < kRenderedSampleStop; ++column)
    {
        const float bottomValue = bottom[((kRenderedSampleStop - 1) * kResolution) + column];
        const float topValue = top[(kRenderedSampleStart * kResolution) + column];
        maxError = std::max(maxError, std::fabs(bottomValue - topValue));
    }
    return maxError;
}

float maxParentChildError(const WorldGridQuadtreeLeafId& parentLeaf, const WorldGridQuadtreeLeafId& childLeaf)
{
    const std::vector<float> parent = buildHeightmap(parentLeaf);
    const std::vector<float> child = buildHeightmap(childLeaf);

    double parentMinX = 0.0;
    double parentMinZ = 0.0;
    double parentSize = 0.0;
    worldGridQuadtreeLeafExtents(parentLeaf, parentMinX, parentMinZ, parentSize);

    double childMinX = 0.0;
    double childMinZ = 0.0;
    double childSize = 0.0;
    worldGridQuadtreeLeafExtents(childLeaf, childMinX, childMinZ, childSize);

    const std::size_t xOffset = static_cast<std::size_t>(std::llround(
        ((childMinX - parentMinX) / parentSize) * static_cast<double>(AppConfig::Terrain::kHeightmapLeafIntervalCount)));
    const std::size_t zOffset = static_cast<std::size_t>(std::llround(
        ((childMinZ - parentMinZ) / parentSize) * static_cast<double>(AppConfig::Terrain::kHeightmapLeafIntervalCount)));

    float maxError = 0.0f;
    for (std::size_t childZ = kLeafSampleStart; childZ < kLeafSampleStop; childZ += 2)
    {
        for (std::size_t childX = kLeafSampleStart; childX < kLeafSampleStop; childX += 2)
        {
            const std::size_t parentX = kLeafSampleStart + xOffset + ((childX - kLeafSampleStart) / 2);
            const std::size_t parentZ = kLeafSampleStart + zOffset + ((childZ - kLeafSampleStart) / 2);
            const float parentValue = parent[(parentZ * kResolution) + parentX];
            const float childValue = child[(childZ * kResolution) + childX];
            maxError = std::max(maxError, std::fabs(parentValue - childValue));
        }
    }
    return maxError;
}
}

int main()
{
    bool failed = false;

    const WorldGridQuadtreeLeafId rootLeft{ .gridX = 0, .gridY = 0, .subdivisionPath = appendChildren({ 3 }) };
    const WorldGridQuadtreeLeafId rootRight{ .gridX = 0, .gridY = 0, .subdivisionPath = appendChildren({ 2 }) };
    const WorldGridQuadtreeLeafId rootBottom{ .gridX = 0, .gridY = 0, .subdivisionPath = appendChildren({ 3 }) };
    const WorldGridQuadtreeLeafId rootTop{ .gridX = 0, .gridY = 0, .subdivisionPath = appendChildren({ 1 }) };
    const WorldGridQuadtreeLeafId acrossCellLeft{ .gridX = 0, .gridY = 0, .subdivisionPath = 0 };
    const WorldGridQuadtreeLeafId acrossCellRight{ .gridX = 1, .gridY = 0, .subdivisionPath = 0 };
    const WorldGridQuadtreeLeafId parentLeaf{ .gridX = 0, .gridY = 0, .subdivisionPath = appendChildren({ 3 }) };
    const WorldGridQuadtreeLeafId childLeaf{ .gridX = 0, .gridY = 0, .subdivisionPath = appendChildren({ 3, 2 }) };

    const std::array passFailChecks{
        std::pair{ "same-level vertical sibling", maxSameLevelVerticalEdgeError(rootLeft, rootRight) },
        std::pair{ "same-level horizontal sibling", maxSameLevelHorizontalEdgeError(rootBottom, rootTop) },
        std::pair{ "same-level across root cells", maxSameLevelVerticalEdgeError(acrossCellLeft, acrossCellRight) },
        std::pair{ "parent-child overlap", maxParentChildError(parentLeaf, childLeaf) },
    };

    for (const auto& [label, error] : passFailChecks)
    {
        std::cout << label << ": max error = " << error << '\n';
        if (error > 0.001f)
        {
            failed = true;
        }
    }

    std::cout << "legacy inner-mesh vertical edge mismatch: max error = "
              << maxRenderedVerticalEdgeError(rootLeft, rootRight) << '\n';
    std::cout << "legacy inner-mesh horizontal edge mismatch: max error = "
              << maxRenderedHorizontalEdgeError(rootBottom, rootTop) << '\n';

    if (failed)
    {
        std::cerr << "quadtree sanity checks failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "quadtree sanity checks passed\n";
    return EXIT_SUCCESS;
}
