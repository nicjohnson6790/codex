#include "PineImposterGenerator.hpp"

#include "CompressonatorBcCodec.hpp"
#include "../../src/assets/RuntimeAssetFormat.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{

constexpr std::uint32_t kImposterResolution = 256u;
constexpr std::uint32_t kYawViewCount = 8u;
constexpr std::uint32_t kPitchViewCount = 4u;
constexpr std::uint32_t kLayerCount = kYawViewCount * kPitchViewCount;
constexpr std::uint8_t kAlphaCoverageCutoffByte = 128u;
constexpr std::array<float, kPitchViewCount> kPitchDegrees{
    -5.0f,
    10.0f,
    25.0f,
    40.0f,
};

struct Float3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Float4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct Mat4
{
    std::array<float, 16> elements{};
};

struct VertexUniforms
{
    Mat4 viewProjection{};
};

struct FragmentUniforms
{
    Float4 viewBasisRight{};
    Float4 viewBasisUp{};
    Float4 viewBasisForward{};
    Float4 sunDirectionIntensity{};
    Float4 sunColorAmbient{};
    Float4 shadingParams0{};
    Float4 cameraPositionAlphaCutoff{};
};

struct AggregateDraw
{
    std::uint32_t firstIndex = 0u;
    std::uint32_t indexCount = 0u;
    std::uint32_t materialIndex = 0u;
};

struct AggregateAssetGeometry
{
    std::vector<RuntimeAssets::MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<AggregateDraw> draws;
    std::array<float, 3> boundsMin{};
    std::array<float, 3> boundsMax{};
    std::array<float, 3> boundsCenter{};
    float boundsRadius = 0.0f;
};

struct MaterialTextures
{
    SDL_GPUTexture* baseColor = nullptr;
    SDL_GPUTexture* normal = nullptr;
    SDL_GPUTexture* roughness = nullptr;
    SDL_GPUTexture* specular = nullptr;
    SDL_GPUTexture* ao = nullptr;
    SDL_GPUTexture* subsurface = nullptr;
    float alphaCutoff = 0.0f;
};

struct CaptureView
{
    Mat4 viewProjection{};
    Float3 cameraPosition{};
    Float3 viewBasisRight{};
    Float3 viewBasisUp{};
    Float3 viewBasisForward{};
};

class OffscreenImposterRenderer
{
public:
    bool initialize(std::string* error);
    void shutdown();

    bool captureAsset(
        const ImportedPack& pack,
        const ImportedAsset& asset,
        std::vector<std::vector<std::byte>>* outColorLayers,
        std::vector<std::vector<std::byte>>* outNormalLayers,
        std::string* error);

private:
    bool createDevice(std::string* error);
    bool createShadersAndPipelines(std::string* error);
    bool createRenderTargets(std::string* error);
    bool createSampler(std::string* error);
    bool createDefaultTextures(std::string* error);
    bool buildAggregateGeometry(
        const ImportedPack& pack,
        const ImportedAsset& asset,
        AggregateAssetGeometry* outGeometry,
        std::string* error) const;
    bool createAssetGeometryBuffers(
        const AggregateAssetGeometry& geometry,
        SDL_GPUBuffer** outVertexBuffer,
        SDL_GPUBuffer** outIndexBuffer,
        std::string* error) const;
    bool createMaterialTextureSet(
        const ImportedPack& pack,
        std::uint32_t textureIndex,
        const std::array<std::byte, 4>& defaultPixel,
        SDL_GPUTexture** outTexture,
        std::string* error) const;
    bool createMaterialResources(
        const ImportedPack& pack,
        const ImportedAsset& asset,
        std::unordered_map<std::uint32_t, MaterialTextures>* outMaterials,
        std::string* error) const;
    bool renderLayer(
        const AggregateAssetGeometry& geometry,
        SDL_GPUBuffer* vertexBuffer,
        SDL_GPUBuffer* indexBuffer,
        const std::unordered_map<std::uint32_t, MaterialTextures>& materials,
        const CaptureView& view,
        std::vector<std::byte>* outColorPixels,
        std::vector<std::byte>* outNormalPixels,
        std::string* error) const;
    bool downloadRenderTarget(
        SDL_GPUTexture* texture,
        SDL_GPUTransferBuffer* transferBuffer,
        std::vector<std::byte>* outPixels,
        std::string* error) const;
    SDL_GPUTextureFormat chooseDepthFormat(std::string* error) const;
    SDL_GPUShader* createShader(
        const std::filesystem::path& path,
        SDL_GPUShaderStage stage,
        std::uint32_t uniformBufferCount,
        std::uint32_t samplerCount,
        std::string* error) const;
    std::vector<std::uint8_t> readShaderCode(const std::filesystem::path& path, std::string* error) const;
    void releaseTexture(SDL_GPUTexture** texture) const;
    void releaseBuffer(SDL_GPUBuffer** buffer) const;
    void releaseTransferBuffer(SDL_GPUTransferBuffer** buffer) const;
    [[nodiscard]] const MaterialTextures& materialTexturesFor(
        const std::unordered_map<std::uint32_t, MaterialTextures>& materials,
        std::uint32_t materialIndex) const;

    SDL_GPUDevice* m_device = nullptr;
    SDL_GPUTextureFormat m_depthFormat = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUGraphicsPipeline* m_colorPipeline = nullptr;
    SDL_GPUGraphicsPipeline* m_normalPipeline = nullptr;
    SDL_GPUSampler* m_sampler = nullptr;
    SDL_GPUTexture* m_colorTarget = nullptr;
    SDL_GPUTexture* m_normalTarget = nullptr;
    SDL_GPUTexture* m_depthTarget = nullptr;
    SDL_GPUTransferBuffer* m_colorReadback = nullptr;
    SDL_GPUTransferBuffer* m_normalReadback = nullptr;

    SDL_GPUTexture* m_defaultBaseColor = nullptr;
    SDL_GPUTexture* m_defaultNormal = nullptr;
    SDL_GPUTexture* m_defaultRoughness = nullptr;
    SDL_GPUTexture* m_defaultSpecular = nullptr;
    SDL_GPUTexture* m_defaultAo = nullptr;
    SDL_GPUTexture* m_defaultSubsurface = nullptr;
};

bool containsInsensitive(std::string_view haystack, std::string_view needle)
{
    auto lower = [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    };

    return std::search(
        haystack.begin(),
        haystack.end(),
        needle.begin(),
        needle.end(),
        [&](char lhs, char rhs) {
            return lower(static_cast<unsigned char>(lhs)) == lower(static_cast<unsigned char>(rhs));
        }) != haystack.end();
}

float clamp01(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

std::uint32_t fullMipCountForExtent(std::uint32_t width, std::uint32_t height)
{
    std::uint32_t mipCount = 1u;
    while (width > 1u || height > 1u)
    {
        width = std::max(width / 2u, 1u);
        height = std::max(height / 2u, 1u);
        ++mipCount;
    }
    return mipCount;
}

std::size_t pixelOffset(std::uint32_t width, std::uint32_t x, std::uint32_t y)
{
    return (static_cast<std::size_t>(y) * width + x) * 4u;
}

void encodeRgba(
    std::vector<std::byte>* pixels,
    std::uint32_t width,
    std::uint32_t x,
    std::uint32_t y,
    float r,
    float g,
    float b,
    float a)
{
    const std::size_t offset = pixelOffset(width, x, y);
    (*pixels)[offset + 0] = std::byte{ static_cast<std::uint8_t>(clamp01(r) * 255.0f) };
    (*pixels)[offset + 1] = std::byte{ static_cast<std::uint8_t>(clamp01(g) * 255.0f) };
    (*pixels)[offset + 2] = std::byte{ static_cast<std::uint8_t>(clamp01(b) * 255.0f) };
    (*pixels)[offset + 3] = std::byte{ static_cast<std::uint8_t>(clamp01(a) * 255.0f) };
}

Float3 decodeNormal(const std::byte* pixel)
{
    const float nx = (static_cast<float>(std::to_integer<std::uint8_t>(pixel[0])) / 127.5f) - 1.0f;
    const float ny = (static_cast<float>(std::to_integer<std::uint8_t>(pixel[1])) / 127.5f) - 1.0f;
    const float nzSquared = std::max(1.0f - (nx * nx) - (ny * ny), 0.0f);
    return { nx, ny, std::sqrt(nzSquared) };
}

void encodeNormal(
    std::vector<std::byte>* pixels,
    std::uint32_t width,
    std::uint32_t x,
    std::uint32_t y,
    Float3 normal,
    float alpha)
{
    const float length = std::sqrt(
        std::max((normal.x * normal.x) + (normal.y * normal.y) + (normal.z * normal.z), 1.0e-12f));
    normal.x /= length;
    normal.y /= length;
    normal.z /= length;
    encodeRgba(
        pixels,
        width,
        x,
        y,
        (normal.x * 0.5f) + 0.5f,
        (normal.y * 0.5f) + 0.5f,
        (normal.z * 0.5f) + 0.5f,
        alpha);
}

void dilateRgbAroundAlphaEdges(
    std::vector<std::byte>* pixels,
    std::uint32_t width,
    std::uint32_t height)
{
    std::vector<std::byte> working = *pixels;
    for (std::uint32_t iteration = 0; iteration < 6u; ++iteration)
    {
        std::vector<std::byte> next = working;
        for (std::uint32_t y = 0; y < height; ++y)
        {
            for (std::uint32_t x = 0; x < width; ++x)
            {
                const std::size_t centerOffset = pixelOffset(width, x, y);
                if (std::to_integer<std::uint8_t>(working[centerOffset + 3]) != 0u)
                {
                    continue;
                }

                std::uint32_t sampleCount = 0u;
                std::uint32_t rgb[3]{};
                for (int offsetY = -1; offsetY <= 1; ++offsetY)
                {
                    for (int offsetX = -1; offsetX <= 1; ++offsetX)
                    {
                        if (offsetX == 0 && offsetY == 0)
                        {
                            continue;
                        }
                        const int sampleX = static_cast<int>(x) + offsetX;
                        const int sampleY = static_cast<int>(y) + offsetY;
                        if (sampleX < 0 || sampleY < 0 ||
                            sampleX >= static_cast<int>(width) || sampleY >= static_cast<int>(height))
                        {
                            continue;
                        }

                        const std::size_t sampleOffset = pixelOffset(width, sampleX, sampleY);
                        if (std::to_integer<std::uint8_t>(working[sampleOffset + 3]) == 0u)
                        {
                            continue;
                        }

                        rgb[0] += std::to_integer<std::uint8_t>(working[sampleOffset + 0]);
                        rgb[1] += std::to_integer<std::uint8_t>(working[sampleOffset + 1]);
                        rgb[2] += std::to_integer<std::uint8_t>(working[sampleOffset + 2]);
                        ++sampleCount;
                    }
                }

                if (sampleCount == 0u)
                {
                    continue;
                }

                next[centerOffset + 0] = std::byte{ static_cast<std::uint8_t>(rgb[0] / sampleCount) };
                next[centerOffset + 1] = std::byte{ static_cast<std::uint8_t>(rgb[1] / sampleCount) };
                next[centerOffset + 2] = std::byte{ static_cast<std::uint8_t>(rgb[2] / sampleCount) };
            }
        }
        working = std::move(next);
    }
    *pixels = std::move(working);
}

std::vector<std::byte> downsampleColorMip(
    std::span<const std::byte> sourcePixels,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight)
{
    std::vector<std::byte> result(static_cast<std::size_t>(targetWidth) * targetHeight * 4u, std::byte{});
    for (std::uint32_t y = 0; y < targetHeight; ++y)
    {
        for (std::uint32_t x = 0; x < targetWidth; ++x)
        {
            std::uint32_t rgba[4]{};
            std::uint32_t sampleCount = 0u;
            for (std::uint32_t sourceY = 0; sourceY < 2u; ++sourceY)
            {
                for (std::uint32_t sourceX = 0; sourceX < 2u; ++sourceX)
                {
                    const std::uint32_t sx = std::min((x * 2u) + sourceX, sourceWidth - 1u);
                    const std::uint32_t sy = std::min((y * 2u) + sourceY, sourceHeight - 1u);
                    const std::size_t sourceOffset = pixelOffset(sourceWidth, sx, sy);
                    for (std::size_t channel = 0; channel < 4u; ++channel)
                    {
                        rgba[channel] += std::to_integer<std::uint8_t>(sourcePixels[sourceOffset + channel]);
                    }
                    ++sampleCount;
                }
            }

            const std::size_t destinationOffset = pixelOffset(targetWidth, x, y);
            for (std::size_t channel = 0; channel < 4u; ++channel)
            {
                result[destinationOffset + channel] =
                    std::byte{ static_cast<std::uint8_t>(rgba[channel] / sampleCount) };
            }
        }
    }
    return result;
}

float alphaCoverageForPixels(std::span<const std::byte> pixels)
{
    if (pixels.empty())
    {
        return 0.0f;
    }

    std::size_t coveredPixelCount = 0u;
    const std::size_t pixelCount = pixels.size() / 4u;
    for (std::size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
    {
        if (std::to_integer<std::uint8_t>(pixels[(pixelIndex * 4u) + 3u]) >= kAlphaCoverageCutoffByte)
        {
            ++coveredPixelCount;
        }
    }

    return static_cast<float>(coveredPixelCount) / static_cast<float>(pixelCount);
}

void scaleAlphaToMatchCoverage(
    std::vector<std::byte>* pixels,
    float targetCoverage)
{
    if (pixels->empty())
    {
        return;
    }

    targetCoverage = std::clamp(targetCoverage, 0.0f, 1.0f);
    float minScale = 0.0f;
    float maxScale = 8.0f;
    for (std::uint32_t iteration = 0; iteration < 18u; ++iteration)
    {
        const float scale = (minScale + maxScale) * 0.5f;
        std::size_t coveredPixelCount = 0u;
        const std::size_t pixelCount = pixels->size() / 4u;
        for (std::size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
        {
            const float alpha = static_cast<float>(std::to_integer<std::uint8_t>((*pixels)[(pixelIndex * 4u) + 3u])) * scale;
            if (alpha >= static_cast<float>(kAlphaCoverageCutoffByte))
            {
                ++coveredPixelCount;
            }
        }

        const float scaledCoverage = static_cast<float>(coveredPixelCount) / static_cast<float>(pixelCount);
        if (scaledCoverage < targetCoverage)
        {
            minScale = scale;
        }
        else
        {
            maxScale = scale;
        }
    }

    const float chosenScale = maxScale;
    const std::size_t pixelCount = pixels->size() / 4u;
    for (std::size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex)
    {
        const std::size_t alphaIndex = (pixelIndex * 4u) + 3u;
        const float scaledAlpha =
            static_cast<float>(std::to_integer<std::uint8_t>((*pixels)[alphaIndex])) * chosenScale;
        (*pixels)[alphaIndex] = std::byte{
            static_cast<std::uint8_t>(std::clamp(scaledAlpha, 0.0f, 255.0f))
        };
    }
}

std::vector<std::byte> downsampleNormalMip(
    std::span<const std::byte> sourcePixels,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::uint32_t targetWidth,
    std::uint32_t targetHeight)
{
    std::vector<std::byte> result(static_cast<std::size_t>(targetWidth) * targetHeight * 4u, std::byte{});
    for (std::uint32_t y = 0; y < targetHeight; ++y)
    {
        for (std::uint32_t x = 0; x < targetWidth; ++x)
        {
            Float3 accumulated{};
            float alphaSum = 0.0f;
            std::uint32_t sampleCount = 0u;
            for (std::uint32_t sourceY = 0; sourceY < 2u; ++sourceY)
            {
                for (std::uint32_t sourceX = 0; sourceX < 2u; ++sourceX)
                {
                    const std::uint32_t sx = std::min((x * 2u) + sourceX, sourceWidth - 1u);
                    const std::uint32_t sy = std::min((y * 2u) + sourceY, sourceHeight - 1u);
                    const std::size_t sourceOffset = pixelOffset(sourceWidth, sx, sy);
                    const Float3 normal = decodeNormal(sourcePixels.data() + sourceOffset);
                    accumulated.x += normal.x;
                    accumulated.y += normal.y;
                    accumulated.z += normal.z;
                    alphaSum += static_cast<float>(std::to_integer<std::uint8_t>(sourcePixels[sourceOffset + 3])) / 255.0f;
                    ++sampleCount;
                }
            }
            encodeNormal(
                &result,
                targetWidth,
                x,
                y,
                accumulated,
                alphaSum / static_cast<float>(sampleCount));
        }
    }
    return result;
}

std::vector<std::vector<std::byte>> buildColorMipChain(
    std::span<const std::byte> baseLayerPixels)
{
    std::vector<std::vector<std::byte>> mipChain;
    mipChain.reserve(fullMipCountForExtent(kImposterResolution, kImposterResolution));
    mipChain.emplace_back(baseLayerPixels.begin(), baseLayerPixels.end());
    const float targetCoverage = alphaCoverageForPixels(baseLayerPixels);

    std::uint32_t sourceWidth = kImposterResolution;
    std::uint32_t sourceHeight = kImposterResolution;
    while (sourceWidth > 1u || sourceHeight > 1u)
    {
        const std::uint32_t nextWidth = std::max(sourceWidth / 2u, 1u);
        const std::uint32_t nextHeight = std::max(sourceHeight / 2u, 1u);
        std::vector<std::byte> nextMip = downsampleColorMip(
            mipChain.back(),
            sourceWidth,
            sourceHeight,
            nextWidth,
            nextHeight);
        scaleAlphaToMatchCoverage(&nextMip, targetCoverage);
        mipChain.push_back(std::move(nextMip));
        sourceWidth = nextWidth;
        sourceHeight = nextHeight;
    }

    return mipChain;
}

std::vector<std::vector<std::byte>> buildNormalMipChain(
    std::span<const std::byte> baseLayerPixels)
{
    std::vector<std::vector<std::byte>> mipChain;
    mipChain.reserve(fullMipCountForExtent(kImposterResolution, kImposterResolution));
    mipChain.emplace_back(baseLayerPixels.begin(), baseLayerPixels.end());
    const float targetCoverage = alphaCoverageForPixels(baseLayerPixels);

    std::uint32_t sourceWidth = kImposterResolution;
    std::uint32_t sourceHeight = kImposterResolution;
    while (sourceWidth > 1u || sourceHeight > 1u)
    {
        const std::uint32_t nextWidth = std::max(sourceWidth / 2u, 1u);
        const std::uint32_t nextHeight = std::max(sourceHeight / 2u, 1u);
        std::vector<std::byte> nextMip = downsampleNormalMip(
            mipChain.back(),
            sourceWidth,
            sourceHeight,
            nextWidth,
            nextHeight);
        scaleAlphaToMatchCoverage(&nextMip, targetCoverage);
        mipChain.push_back(std::move(nextMip));
        sourceWidth = nextWidth;
        sourceHeight = nextHeight;
    }

    return mipChain;
}

bool appendCompressedMip(
    RuntimeAssets::TextureFormat format,
    std::span<const std::byte> rgbaPixels,
    std::uint32_t width,
    std::uint32_t height,
    std::vector<std::byte>* destinationPayload,
    std::string* error)
{
    const std::uint64_t expectedBytes = RuntimeAssets::CalculateTextureMipByteSize(format, width, height);
    std::vector<std::byte> compressedBlocks;
    const bool compressed =
        format == RuntimeAssets::TextureFormat::BC3_RGBA_UNORM
            ? ConverterCompressBc3Rgba(rgbaPixels, width, height, &compressedBlocks, error)
            : ConverterCompressBc5Rg(rgbaPixels, width, height, &compressedBlocks, error);
    if (!compressed)
    {
        return false;
    }

    if (compressedBlocks.size() != expectedBytes)
    {
        *error = "compressed imposter mip byte count does not match BC block sizing rules";
        return false;
    }

    destinationPayload->insert(destinationPayload->end(), compressedBlocks.begin(), compressedBlocks.end());
    return true;
}

Float3 makeFloat3(float x, float y, float z)
{
    return { x, y, z };
}

Float3 add(Float3 lhs, Float3 rhs)
{
    return { lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z };
}

Float3 subtract(Float3 lhs, Float3 rhs)
{
    return { lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z };
}

Float3 scale(Float3 value, float scalar)
{
    return { value.x * scalar, value.y * scalar, value.z * scalar };
}

float dot(Float3 lhs, Float3 rhs)
{
    return (lhs.x * rhs.x) + (lhs.y * rhs.y) + (lhs.z * rhs.z);
}

Float3 cross(Float3 lhs, Float3 rhs)
{
    return {
        (lhs.y * rhs.z) - (lhs.z * rhs.y),
        (lhs.z * rhs.x) - (lhs.x * rhs.z),
        (lhs.x * rhs.y) - (lhs.y * rhs.x),
    };
}

float length(Float3 value)
{
    return std::sqrt(std::max(dot(value, value), 0.0f));
}

Float3 normalize(Float3 value)
{
    const float valueLength = length(value);
    if (valueLength <= 1.0e-6f)
    {
        return { 0.0f, 1.0f, 0.0f };
    }
    return scale(value, 1.0f / valueLength);
}

Mat4 multiply(const Mat4& lhs, const Mat4& rhs)
{
    Mat4 result{};
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            float value = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                value += lhs.elements[k * 4 + row] * rhs.elements[column * 4 + k];
            }
            result.elements[column * 4 + row] = value;
        }
    }
    return result;
}

Float3 transformPoint(const Mat4& matrix, Float3 point)
{
    const float x =
        (matrix.elements[0] * point.x) +
        (matrix.elements[4] * point.y) +
        (matrix.elements[8] * point.z) +
        matrix.elements[12];
    const float y =
        (matrix.elements[1] * point.x) +
        (matrix.elements[5] * point.y) +
        (matrix.elements[9] * point.z) +
        matrix.elements[13];
    const float z =
        (matrix.elements[2] * point.x) +
        (matrix.elements[6] * point.y) +
        (matrix.elements[10] * point.z) +
        matrix.elements[14];
    const float w =
        (matrix.elements[3] * point.x) +
        (matrix.elements[7] * point.y) +
        (matrix.elements[11] * point.z) +
        matrix.elements[15];
    const float invW = std::abs(w) > 1.0e-6f ? (1.0f / w) : 1.0f;
    return { x * invW, y * invW, z * invW };
}

Mat4 makeViewMatrix(Float3 right, Float3 up, Float3 forward, Float3 cameraPosition)
{
    Mat4 matrix{};
    matrix.elements = {
        right.x, up.x, forward.x, 0.0f,
        right.y, up.y, forward.y, 0.0f,
        right.z, up.z, forward.z, 0.0f,
        -dot(right, cameraPosition), -dot(up, cameraPosition), -dot(forward, cameraPosition), 1.0f,
    };
    return matrix;
}

Mat4 makeOrthoMatrix(float left, float right, float bottom, float top, float nearPlane, float farPlane)
{
    Mat4 matrix{};
    const float width = std::max(right - left, 1.0e-5f);
    const float height = std::max(top - bottom, 1.0e-5f);
    const float depth = std::max(farPlane - nearPlane, 1.0e-5f);
    matrix.elements = {
        2.0f / width, 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f / height, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f / depth, 0.0f,
        -(right + left) / width, -(top + bottom) / height, -nearPlane / depth, 1.0f,
    };
    return matrix;
}

std::array<Float3, 8> makeBoundsCorners(const AggregateAssetGeometry& geometry)
{
    const auto& min = geometry.boundsMin;
    const auto& max = geometry.boundsMax;
    return {
        makeFloat3(min[0], min[1], min[2]),
        makeFloat3(max[0], min[1], min[2]),
        makeFloat3(min[0], max[1], min[2]),
        makeFloat3(max[0], max[1], min[2]),
        makeFloat3(min[0], min[1], max[2]),
        makeFloat3(max[0], min[1], max[2]),
        makeFloat3(min[0], max[1], max[2]),
        makeFloat3(max[0], max[1], max[2]),
    };
}

CaptureView buildCaptureView(
    const AggregateAssetGeometry& geometry,
    std::uint32_t pitchIndex,
    std::uint32_t yawIndex)
{
    const float yawRadians = (static_cast<float>(yawIndex) / static_cast<float>(kYawViewCount)) * 6.28318530718f;
    const float pitchRadians = kPitchDegrees[pitchIndex] * (3.14159265359f / 180.0f);
    const Float3 target = {
        geometry.boundsCenter[0],
        geometry.boundsCenter[1],
        geometry.boundsCenter[2],
    };

    const Float3 viewDirection = normalize({
        std::cos(pitchRadians) * std::sin(yawRadians),
        std::sin(pitchRadians),
        std::cos(pitchRadians) * std::cos(yawRadians),
    });
    Float3 upHint = { 0.0f, 1.0f, 0.0f };
    if (std::abs(dot(viewDirection, upHint)) > 0.98f)
    {
        upHint = { 1.0f, 0.0f, 0.0f };
    }

    const Float3 right = normalize(cross(viewDirection, upHint));
    const Float3 up = normalize(cross(right, viewDirection));
    const float cameraDistance = std::max(geometry.boundsRadius * 3.0f, 2.0f);
    const Float3 cameraPosition = subtract(target, scale(viewDirection, cameraDistance));
    const Mat4 viewMatrix = makeViewMatrix(right, up, viewDirection, cameraPosition);

    float minX = std::numeric_limits<float>::max();
    float minY = std::numeric_limits<float>::max();
    float minZ = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float maxY = std::numeric_limits<float>::lowest();
    float maxZ = std::numeric_limits<float>::lowest();
    for (const Float3 corner : makeBoundsCorners(geometry))
    {
        const Float3 viewSpace = transformPoint(viewMatrix, corner);
        minX = std::min(minX, viewSpace.x);
        minY = std::min(minY, viewSpace.y);
        minZ = std::min(minZ, viewSpace.z);
        maxX = std::max(maxX, viewSpace.x);
        maxY = std::max(maxY, viewSpace.y);
        maxZ = std::max(maxZ, viewSpace.z);
    }

    const float paddingX = std::max((maxX - minX) * 0.08f, 0.15f);
    const float paddingY = std::max((maxY - minY) * 0.08f, 0.15f);
    const float paddingZ = std::max((maxZ - minZ) * 0.25f, 1.0f);
    const Mat4 projection = makeOrthoMatrix(
        minX - paddingX,
        maxX + paddingX,
        minY - paddingY,
        maxY + paddingY,
        std::max(minZ - paddingZ, 0.01f),
        maxZ + paddingZ);

    CaptureView result{};
    result.viewProjection = multiply(projection, viewMatrix);
    result.cameraPosition = cameraPosition;
    result.viewBasisRight = right;
    result.viewBasisUp = up;
    result.viewBasisForward = viewDirection;
    return result;
}

bool isBillboardMaterial(const ImportedMaterial& material)
{
    return containsInsensitive(material.normalizedName, "billboard");
}

bool isBillboardMesh(const ImportedMesh& mesh)
{
    return containsInsensitive(mesh.meshName, "billboard");
}

ImportedTexture buildGeneratedTexture(
    std::string_view assetName,
    std::string_view suffix,
    RuntimeAssets::TextureFormat format,
    std::vector<std::byte>&& payload)
{
    ImportedTexture texture;
    texture.name = std::string(assetName) + "_" + std::string(suffix);
    texture.normalizedBasename = texture.name + ".generated";
    texture.sourcePath = "generated://pine_imposter_capture/" + std::string(assetName) + "/" + std::string(suffix);
    texture.width = kImposterResolution;
    texture.height = kImposterResolution;
    texture.layerCount = kLayerCount;
    texture.mipCount = fullMipCountForExtent(kImposterResolution, kImposterResolution);
    texture.format = format;
    texture.dimension = RuntimeAssets::TextureDimension::Texture2DArray;
    texture.flags = 0u;
    texture.payload = std::move(payload);
    return texture;
}

bool buildCompressedTexturePayload(
    RuntimeAssets::TextureFormat format,
    const std::vector<std::vector<std::byte>>& layers,
    std::vector<std::byte>* outPayload,
    std::string* error)
{
    outPayload->clear();
    const std::uint32_t mipCount = fullMipCountForExtent(kImposterResolution, kImposterResolution);
    const std::uint64_t expectedTotalBytes = RuntimeAssets::CalculateTextureDataSize(
        format,
        kImposterResolution,
        kImposterResolution,
        kLayerCount,
        mipCount);
    outPayload->reserve(static_cast<std::size_t>(expectedTotalBytes));

    if (layers.size() != kLayerCount)
    {
        *error = "imposter layer count does not match expected yaw/pitch view count";
        return false;
    }

    for (const std::vector<std::byte>& layerPixels : layers)
    {
        const std::vector<std::vector<std::byte>> mipChain =
            format == RuntimeAssets::TextureFormat::BC3_RGBA_UNORM
                ? buildColorMipChain(layerPixels)
                : buildNormalMipChain(layerPixels);
        std::uint32_t mipWidth = kImposterResolution;
        std::uint32_t mipHeight = kImposterResolution;

        for (std::uint32_t mipIndex = 0; mipIndex < mipCount; ++mipIndex)
        {
            if (!appendCompressedMip(
                    format,
                    std::span<const std::byte>(mipChain[mipIndex]),
                    mipWidth,
                    mipHeight,
                    outPayload,
                    error))
            {
                return false;
            }

            mipWidth = std::max(mipWidth / 2u, 1u);
            mipHeight = std::max(mipHeight / 2u, 1u);
        }
    }

    if (outPayload->size() != expectedTotalBytes)
    {
        *error = "packed imposter payload size does not match the expected layer-by-mip BC byte size";
        outPayload->clear();
        return false;
    }

    return true;
}

bool OffscreenImposterRenderer::initialize(std::string* error)
{
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
    {
        *error = std::string("SDL_InitSubSystem(SDL_INIT_VIDEO) failed: ") + SDL_GetError();
        return false;
    }
    return createDevice(error) &&
        createSampler(error) &&
        createDefaultTextures(error) &&
        createRenderTargets(error) &&
        createShadersAndPipelines(error);
}

void OffscreenImposterRenderer::shutdown()
{
    releaseTexture(&m_defaultSubsurface);
    releaseTexture(&m_defaultAo);
    releaseTexture(&m_defaultSpecular);
    releaseTexture(&m_defaultRoughness);
    releaseTexture(&m_defaultNormal);
    releaseTexture(&m_defaultBaseColor);
    releaseTransferBuffer(&m_normalReadback);
    releaseTransferBuffer(&m_colorReadback);
    releaseTexture(&m_depthTarget);
    releaseTexture(&m_normalTarget);
    releaseTexture(&m_colorTarget);
    if (m_sampler != nullptr)
    {
        SDL_ReleaseGPUSampler(m_device, m_sampler);
        m_sampler = nullptr;
    }
    if (m_normalPipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_normalPipeline);
        m_normalPipeline = nullptr;
    }
    if (m_colorPipeline != nullptr)
    {
        SDL_ReleaseGPUGraphicsPipeline(m_device, m_colorPipeline);
        m_colorPipeline = nullptr;
    }
    if (m_device != nullptr)
    {
        SDL_WaitForGPUIdle(m_device);
        SDL_DestroyGPUDevice(m_device);
        m_device = nullptr;
    }
}

bool OffscreenImposterRenderer::createDevice(std::string* error)
{
    SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0)
    {
        *error = std::string("SDL_CreateProperties failed: ") + SDL_GetError();
        return false;
    }

    SDL_SetStringProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "vulkan");
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
    SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN, true);

    m_device = SDL_CreateGPUDeviceWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (m_device == nullptr)
    {
        *error = std::string("SDL_CreateGPUDeviceWithProperties failed: ") + SDL_GetError();
        return false;
    }

    m_depthFormat = chooseDepthFormat(error);
    return m_depthFormat != SDL_GPU_TEXTUREFORMAT_INVALID;
}

SDL_GPUTextureFormat OffscreenImposterRenderer::chooseDepthFormat(std::string* error) const
{
    const std::array<SDL_GPUTextureFormat, 3> candidates{
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT,
        SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
        SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT,
    };

    for (const SDL_GPUTextureFormat candidate : candidates)
    {
        if (SDL_GPUTextureSupportsFormat(
                m_device,
                candidate,
                SDL_GPU_TEXTURETYPE_2D,
                SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
        {
            return candidate;
        }
    }

    *error = "Failed to find a supported offscreen depth format for imposter capture";
    return SDL_GPU_TEXTUREFORMAT_INVALID;
}

bool OffscreenImposterRenderer::createSampler(std::string* error)
{
    SDL_GPUSamplerCreateInfo samplerInfo{};
    samplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    samplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    samplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
    samplerInfo.min_lod = 0.0f;
    samplerInfo.max_lod = 0.0f;
    m_sampler = SDL_CreateGPUSampler(m_device, &samplerInfo);
    if (m_sampler == nullptr)
    {
        *error = std::string("Failed to create imposter sampler: ") + SDL_GetError();
        return false;
    }
    return true;
}

bool OffscreenImposterRenderer::createRenderTargets(std::string* error)
{
    auto createTarget = [&](SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage, SDL_GPUTexture** outTexture) {
        SDL_GPUTextureCreateInfo textureInfo{};
        textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
        textureInfo.format = format;
        textureInfo.usage = usage;
        textureInfo.width = kImposterResolution;
        textureInfo.height = kImposterResolution;
        textureInfo.layer_count_or_depth = 1;
        textureInfo.num_levels = 1;
        textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
        *outTexture = SDL_CreateGPUTexture(m_device, &textureInfo);
        return *outTexture != nullptr;
    };

    if (!createTarget(
            SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
            &m_colorTarget) ||
        !createTarget(
            SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
            SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
            &m_normalTarget) ||
        !createTarget(
            m_depthFormat,
            SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET,
            &m_depthTarget))
    {
        *error = std::string("Failed to create imposter render targets: ") + SDL_GetError();
        return false;
    }

    SDL_GPUTransferBufferCreateInfo readbackInfo{};
    readbackInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    readbackInfo.size = kImposterResolution * kImposterResolution * 4u;
    m_colorReadback = SDL_CreateGPUTransferBuffer(m_device, &readbackInfo);
    m_normalReadback = SDL_CreateGPUTransferBuffer(m_device, &readbackInfo);
    if (m_colorReadback == nullptr || m_normalReadback == nullptr)
    {
        *error = std::string("Failed to create imposter readback buffers: ") + SDL_GetError();
        return false;
    }

    return true;
}

std::vector<std::uint8_t> OffscreenImposterRenderer::readShaderCode(
    const std::filesystem::path& path,
    std::string* error) const
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        *error = "Failed to open shader: " + path.string();
        return {};
    }

    const std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file)
    {
        *error = "Failed to read shader: " + path.string();
        return {};
    }
    return bytes;
}

SDL_GPUShader* OffscreenImposterRenderer::createShader(
    const std::filesystem::path& path,
    SDL_GPUShaderStage stage,
    std::uint32_t uniformBufferCount,
    std::uint32_t samplerCount,
    std::string* error) const
{
    const std::vector<std::uint8_t> code = readShaderCode(path, error);
    if (code.empty())
    {
        return nullptr;
    }

    SDL_GPUShaderCreateInfo shaderInfo{};
    shaderInfo.code_size = code.size();
    shaderInfo.code = code.data();
    shaderInfo.entrypoint = "main";
    shaderInfo.format = SDL_GPU_SHADERFORMAT_SPIRV;
    shaderInfo.stage = stage;
    shaderInfo.num_uniform_buffers = uniformBufferCount;
    shaderInfo.num_samplers = samplerCount;

    SDL_GPUShader* shader = SDL_CreateGPUShader(m_device, &shaderInfo);
    if (shader == nullptr)
    {
        *error = std::string("Failed to create imposter shader ") + path.string() + ": " + SDL_GetError();
    }
    return shader;
}

bool OffscreenImposterRenderer::createShadersAndPipelines(std::string* error)
{
    const std::filesystem::path shaderDirectory(CONVERTER_SHADER_DIR);
    SDL_GPUShader* vertexShader = createShader(
        shaderDirectory / "pine_imposter_capture.vert.spv",
        SDL_GPU_SHADERSTAGE_VERTEX,
        1,
        0,
        error);
    if (vertexShader == nullptr)
    {
        return false;
    }

    SDL_GPUShader* colorFragmentShader = createShader(
        shaderDirectory / "pine_imposter_capture_color.frag.spv",
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        1,
        6,
        error);
    if (colorFragmentShader == nullptr)
    {
        SDL_ReleaseGPUShader(m_device, vertexShader);
        return false;
    }

    SDL_GPUShader* normalFragmentShader = createShader(
        shaderDirectory / "pine_imposter_capture_normal.frag.spv",
        SDL_GPU_SHADERSTAGE_FRAGMENT,
        1,
        6,
        error);
    if (normalFragmentShader == nullptr)
    {
        SDL_ReleaseGPUShader(m_device, colorFragmentShader);
        SDL_ReleaseGPUShader(m_device, vertexShader);
        return false;
    }

    SDL_GPUVertexBufferDescription vertexBufferDescriptions[1]{};
    vertexBufferDescriptions[0].slot = 0;
    vertexBufferDescriptions[0].pitch = sizeof(RuntimeAssets::MeshVertex);
    vertexBufferDescriptions[0].input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute vertexAttributes[4]{};
    vertexAttributes[0].location = 0;
    vertexAttributes[0].buffer_slot = 0;
    vertexAttributes[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[0].offset = offsetof(RuntimeAssets::MeshVertex, position);
    vertexAttributes[1].location = 1;
    vertexAttributes[1].buffer_slot = 0;
    vertexAttributes[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    vertexAttributes[1].offset = offsetof(RuntimeAssets::MeshVertex, normal);
    vertexAttributes[2].location = 2;
    vertexAttributes[2].buffer_slot = 0;
    vertexAttributes[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    vertexAttributes[2].offset = offsetof(RuntimeAssets::MeshVertex, tangent);
    vertexAttributes[3].location = 3;
    vertexAttributes[3].buffer_slot = 0;
    vertexAttributes[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    vertexAttributes[3].offset = offsetof(RuntimeAssets::MeshVertex, uv0);

    SDL_GPUColorTargetDescription colorTargetDescription{};
    colorTargetDescription.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    colorTargetDescription.blend_state.color_write_mask =
        SDL_GPU_COLORCOMPONENT_R |
        SDL_GPU_COLORCOMPONENT_G |
        SDL_GPU_COLORCOMPONENT_B |
        SDL_GPU_COLORCOMPONENT_A;

    auto createPipeline = [&](SDL_GPUShader* fragmentShader, SDL_GPUGraphicsPipeline** outPipeline) -> bool {
        SDL_GPUGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.vertex_shader = vertexShader;
        pipelineInfo.fragment_shader = fragmentShader;
        pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipelineInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pipelineInfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        pipelineInfo.rasterizer_state.enable_depth_clip = true;
        pipelineInfo.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pipelineInfo.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        pipelineInfo.depth_stencil_state.enable_depth_test = true;
        pipelineInfo.depth_stencil_state.enable_depth_write = true;
        pipelineInfo.target_info.num_color_targets = 1;
        pipelineInfo.target_info.color_target_descriptions = &colorTargetDescription;
        pipelineInfo.target_info.has_depth_stencil_target = true;
        pipelineInfo.target_info.depth_stencil_format = m_depthFormat;
        pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
        pipelineInfo.vertex_input_state.vertex_buffer_descriptions = vertexBufferDescriptions;
        pipelineInfo.vertex_input_state.num_vertex_attributes = 4;
        pipelineInfo.vertex_input_state.vertex_attributes = vertexAttributes;

        *outPipeline = SDL_CreateGPUGraphicsPipeline(m_device, &pipelineInfo);
        if (*outPipeline == nullptr)
        {
            *error = std::string("Failed to create imposter pipeline: ") + SDL_GetError();
            return false;
        }
        return true;
    };

    const bool ok = createPipeline(colorFragmentShader, &m_colorPipeline) &&
        createPipeline(normalFragmentShader, &m_normalPipeline);
    SDL_ReleaseGPUShader(m_device, normalFragmentShader);
    SDL_ReleaseGPUShader(m_device, colorFragmentShader);
    SDL_ReleaseGPUShader(m_device, vertexShader);
    return ok;
}

void OffscreenImposterRenderer::releaseTexture(SDL_GPUTexture** texture) const
{
    if (*texture != nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, *texture);
        *texture = nullptr;
    }
}

void OffscreenImposterRenderer::releaseBuffer(SDL_GPUBuffer** buffer) const
{
    if (*buffer != nullptr)
    {
        SDL_ReleaseGPUBuffer(m_device, *buffer);
        *buffer = nullptr;
    }
}

void OffscreenImposterRenderer::releaseTransferBuffer(SDL_GPUTransferBuffer** buffer) const
{
    if (*buffer != nullptr)
    {
        SDL_ReleaseGPUTransferBuffer(m_device, *buffer);
        *buffer = nullptr;
    }
}

bool OffscreenImposterRenderer::createMaterialTextureSet(
    const ImportedPack& pack,
    std::uint32_t textureIndex,
    const std::array<std::byte, 4>& defaultPixel,
    SDL_GPUTexture** outTexture,
    std::string* error) const
{
    if (textureIndex == std::numeric_limits<std::uint32_t>::max())
    {
        const std::array<std::byte, 4> pixel = defaultPixel;
        SDL_GPUTextureCreateInfo textureInfo{};
        textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
        textureInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        textureInfo.width = 1;
        textureInfo.height = 1;
        textureInfo.layer_count_or_depth = 1;
        textureInfo.num_levels = 1;
        textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
        SDL_GPUTexture* texture = SDL_CreateGPUTexture(m_device, &textureInfo);
        if (texture == nullptr)
        {
            *error = std::string("Failed to create default imposter material texture: ") + SDL_GetError();
            return false;
        }

        SDL_GPUTransferBufferCreateInfo transferInfo{};
        transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        transferInfo.size = 4u;
        SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
        if (transferBuffer == nullptr)
        {
            SDL_ReleaseGPUTexture(m_device, texture);
            *error = std::string("Failed to create default texture upload buffer: ") + SDL_GetError();
            return false;
        }

        void* mapped = SDL_MapGPUTransferBuffer(m_device, transferBuffer, false);
        std::memcpy(mapped, pixel.data(), pixel.size());
        SDL_UnmapGPUTransferBuffer(m_device, transferBuffer);

        SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
        SDL_GPUCopyPass* copyPass = commandBuffer != nullptr ? SDL_BeginGPUCopyPass(commandBuffer) : nullptr;
        if (copyPass == nullptr)
        {
            if (commandBuffer != nullptr)
            {
                SDL_CancelGPUCommandBuffer(commandBuffer);
            }
            SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
            SDL_ReleaseGPUTexture(m_device, texture);
            *error = std::string("Failed to begin default texture upload: ") + SDL_GetError();
            return false;
        }

        SDL_GPUTextureTransferInfo source{};
        source.transfer_buffer = transferBuffer;
        source.pixels_per_row = 1u;
        source.rows_per_layer = 1u;

        SDL_GPUTextureRegion destination{};
        destination.texture = texture;
        destination.w = 1u;
        destination.h = 1u;
        destination.d = 1u;
        SDL_UploadToGPUTexture(copyPass, &source, &destination, false);
        SDL_EndGPUCopyPass(copyPass);
        if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
        {
            SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
            SDL_ReleaseGPUTexture(m_device, texture);
            *error = std::string("Failed to submit default texture upload: ") + SDL_GetError();
            return false;
        }

        SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
        *outTexture = texture;
        return true;
    }

    if (textureIndex >= pack.textures.size())
    {
        *error = "material texture index points past imported texture count";
        return false;
    }

    const ImportedTexture& importedTexture = pack.textures[textureIndex];
    if (importedTexture.payload.size() != static_cast<std::size_t>(importedTexture.width) * importedTexture.height * 4u)
    {
        *error = "imported source texture payload is not tightly packed RGBA8";
        return false;
    }

    SDL_GPUTextureCreateInfo textureInfo{};
    textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
    textureInfo.format =
        importedTexture.format == RuntimeAssets::TextureFormat::RGBA8_SRGB
            ? SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB
            : SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    textureInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    textureInfo.width = importedTexture.width;
    textureInfo.height = importedTexture.height;
    textureInfo.layer_count_or_depth = 1;
    textureInfo.num_levels = 1;
    textureInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(m_device, &textureInfo);
    if (texture == nullptr)
    {
        *error = std::string("Failed to create imposter material texture: ") + SDL_GetError();
        return false;
    }

    SDL_GPUTransferBufferCreateInfo transferInfo{};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = static_cast<Uint32>(importedTexture.payload.size());
    SDL_GPUTransferBuffer* transferBuffer = SDL_CreateGPUTransferBuffer(m_device, &transferInfo);
    if (transferBuffer == nullptr)
    {
        SDL_ReleaseGPUTexture(m_device, texture);
        *error = std::string("Failed to create imposter texture upload buffer: ") + SDL_GetError();
        return false;
    }

    void* mapped = SDL_MapGPUTransferBuffer(m_device, transferBuffer, false);
    std::memcpy(mapped, importedTexture.payload.data(), importedTexture.payload.size());
    SDL_UnmapGPUTransferBuffer(m_device, transferBuffer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass* copyPass = commandBuffer != nullptr ? SDL_BeginGPUCopyPass(commandBuffer) : nullptr;
    if (copyPass == nullptr)
    {
        if (commandBuffer != nullptr)
        {
            SDL_CancelGPUCommandBuffer(commandBuffer);
        }
        SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
        SDL_ReleaseGPUTexture(m_device, texture);
        *error = std::string("Failed to begin imposter texture upload: ") + SDL_GetError();
        return false;
    }

    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = transferBuffer;
    source.pixels_per_row = importedTexture.width;
    source.rows_per_layer = importedTexture.height;

    SDL_GPUTextureRegion destination{};
    destination.texture = texture;
    destination.w = importedTexture.width;
    destination.h = importedTexture.height;
    destination.d = 1u;
    SDL_UploadToGPUTexture(copyPass, &source, &destination, false);
    SDL_EndGPUCopyPass(copyPass);

    if (!SDL_SubmitGPUCommandBuffer(commandBuffer))
    {
        SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
        SDL_ReleaseGPUTexture(m_device, texture);
        *error = std::string("Failed to submit imposter texture upload: ") + SDL_GetError();
        return false;
    }

    SDL_ReleaseGPUTransferBuffer(m_device, transferBuffer);
    *outTexture = texture;
    return true;
}

bool OffscreenImposterRenderer::createDefaultTextures(std::string* error)
{
    return createMaterialTextureSet({}, std::numeric_limits<std::uint32_t>::max(), { std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0xFF } }, &m_defaultBaseColor, error) &&
        createMaterialTextureSet({}, std::numeric_limits<std::uint32_t>::max(), { std::byte{ 0x80 }, std::byte{ 0x80 }, std::byte{ 0xFF }, std::byte{ 0xFF } }, &m_defaultNormal, error) &&
        createMaterialTextureSet({}, std::numeric_limits<std::uint32_t>::max(), { std::byte{ 0xFF }, std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0xFF } }, &m_defaultRoughness, error) &&
        createMaterialTextureSet({}, std::numeric_limits<std::uint32_t>::max(), { std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0xFF } }, &m_defaultSpecular, error) &&
        createMaterialTextureSet({}, std::numeric_limits<std::uint32_t>::max(), { std::byte{ 0xFF }, std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0xFF } }, &m_defaultAo, error) &&
        createMaterialTextureSet({}, std::numeric_limits<std::uint32_t>::max(), { std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0xFF } }, &m_defaultSubsurface, error);
}

bool OffscreenImposterRenderer::buildAggregateGeometry(
    const ImportedPack& pack,
    const ImportedAsset& asset,
    AggregateAssetGeometry* outGeometry,
    std::string* error) const
{
    outGeometry->vertices.clear();
    outGeometry->indices.clear();
    outGeometry->draws.clear();

    std::uint32_t selectedLod = std::numeric_limits<std::uint32_t>::max();
    for (const std::uint32_t meshIndex : asset.meshIndices)
    {
        if (meshIndex >= pack.meshes.size())
        {
            *error = "asset references a mesh index that does not exist";
            return false;
        }

        const ImportedMesh& mesh = pack.meshes[meshIndex];
        const ImportedMaterial& material = pack.materials[mesh.materialIndex];
        if (isBillboardMesh(mesh) || isBillboardMaterial(material))
        {
            continue;
        }
        selectedLod = std::min(selectedLod, mesh.lodIndex);
    }

    if (selectedLod == std::numeric_limits<std::uint32_t>::max())
    {
        *error = "no non-billboard source meshes were found for imposter generation";
        return false;
    }

    outGeometry->boundsMin = {
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max(),
    };
    outGeometry->boundsMax = {
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
    };

    for (const std::uint32_t meshIndex : asset.meshIndices)
    {
        const ImportedMesh& mesh = pack.meshes[meshIndex];
        const ImportedMaterial& material = pack.materials[mesh.materialIndex];
        if (mesh.lodIndex != selectedLod || isBillboardMesh(mesh) || isBillboardMaterial(material))
        {
            continue;
        }

        const std::uint32_t baseVertex = static_cast<std::uint32_t>(outGeometry->vertices.size());
        const std::uint32_t firstIndex = static_cast<std::uint32_t>(outGeometry->indices.size());
        outGeometry->vertices.insert(outGeometry->vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        outGeometry->indices.reserve(outGeometry->indices.size() + mesh.indices.size());
        for (const std::uint32_t index : mesh.indices)
        {
            outGeometry->indices.push_back(baseVertex + index);
        }

        AggregateDraw draw{};
        draw.firstIndex = firstIndex;
        draw.indexCount = static_cast<std::uint32_t>(mesh.indices.size());
        draw.materialIndex = mesh.materialIndex;
        outGeometry->draws.push_back(draw);

        for (int axis = 0; axis < 3; ++axis)
        {
            outGeometry->boundsMin[axis] = std::min(outGeometry->boundsMin[axis], mesh.boundsMin[axis]);
            outGeometry->boundsMax[axis] = std::max(outGeometry->boundsMax[axis], mesh.boundsMax[axis]);
        }
    }

    if (outGeometry->draws.empty() || outGeometry->vertices.empty() || outGeometry->indices.empty())
    {
        *error = "selected imposter source geometry is empty";
        return false;
    }

    for (int axis = 0; axis < 3; ++axis)
    {
        outGeometry->boundsCenter[axis] = (outGeometry->boundsMin[axis] + outGeometry->boundsMax[axis]) * 0.5f;
    }

    float maxDistanceSquared = 0.0f;
    for (const RuntimeAssets::MeshVertex& vertex : outGeometry->vertices)
    {
        const float dx = vertex.position[0] - outGeometry->boundsCenter[0];
        const float dy = vertex.position[1] - outGeometry->boundsCenter[1];
        const float dz = vertex.position[2] - outGeometry->boundsCenter[2];
        maxDistanceSquared = std::max(maxDistanceSquared, dx * dx + dy * dy + dz * dz);
    }
    outGeometry->boundsRadius = std::sqrt(maxDistanceSquared);
    return true;
}

bool OffscreenImposterRenderer::createAssetGeometryBuffers(
    const AggregateAssetGeometry& geometry,
    SDL_GPUBuffer** outVertexBuffer,
    SDL_GPUBuffer** outIndexBuffer,
    std::string* error) const
{
    SDL_GPUBufferCreateInfo vertexBufferInfo{};
    vertexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vertexBufferInfo.size = static_cast<Uint32>(geometry.vertices.size() * sizeof(RuntimeAssets::MeshVertex));
    *outVertexBuffer = SDL_CreateGPUBuffer(m_device, &vertexBufferInfo);

    SDL_GPUBufferCreateInfo indexBufferInfo{};
    indexBufferInfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    indexBufferInfo.size = static_cast<Uint32>(geometry.indices.size() * sizeof(std::uint32_t));
    *outIndexBuffer = SDL_CreateGPUBuffer(m_device, &indexBufferInfo);

    if (*outVertexBuffer == nullptr || *outIndexBuffer == nullptr)
    {
        *error = std::string("Failed to create imposter mesh buffers: ") + SDL_GetError();
        return false;
    }

    SDL_GPUTransferBufferCreateInfo vertexTransferInfo{};
    vertexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    vertexTransferInfo.size = vertexBufferInfo.size;
    SDL_GPUTransferBuffer* vertexTransfer = SDL_CreateGPUTransferBuffer(m_device, &vertexTransferInfo);

    SDL_GPUTransferBufferCreateInfo indexTransferInfo{};
    indexTransferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    indexTransferInfo.size = indexBufferInfo.size;
    SDL_GPUTransferBuffer* indexTransfer = SDL_CreateGPUTransferBuffer(m_device, &indexTransferInfo);

    if (vertexTransfer == nullptr || indexTransfer == nullptr)
    {
        if (vertexTransfer != nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(m_device, vertexTransfer);
        }
        if (indexTransfer != nullptr)
        {
            SDL_ReleaseGPUTransferBuffer(m_device, indexTransfer);
        }
        *error = std::string("Failed to create imposter mesh upload buffers: ") + SDL_GetError();
        return false;
    }

    void* mappedVertices = SDL_MapGPUTransferBuffer(m_device, vertexTransfer, false);
    std::memcpy(mappedVertices, geometry.vertices.data(), geometry.vertices.size() * sizeof(RuntimeAssets::MeshVertex));
    SDL_UnmapGPUTransferBuffer(m_device, vertexTransfer);

    void* mappedIndices = SDL_MapGPUTransferBuffer(m_device, indexTransfer, false);
    std::memcpy(mappedIndices, geometry.indices.data(), geometry.indices.size() * sizeof(std::uint32_t));
    SDL_UnmapGPUTransferBuffer(m_device, indexTransfer);

    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    SDL_GPUCopyPass* copyPass = commandBuffer != nullptr ? SDL_BeginGPUCopyPass(commandBuffer) : nullptr;
    if (copyPass == nullptr)
    {
        if (commandBuffer != nullptr)
        {
            SDL_CancelGPUCommandBuffer(commandBuffer);
        }
        SDL_ReleaseGPUTransferBuffer(m_device, vertexTransfer);
        SDL_ReleaseGPUTransferBuffer(m_device, indexTransfer);
        *error = std::string("Failed to begin imposter mesh upload copy pass: ") + SDL_GetError();
        return false;
    }

    SDL_GPUTransferBufferLocation vertexSource{};
    vertexSource.transfer_buffer = vertexTransfer;
    SDL_GPUBufferRegion vertexDestination{};
    vertexDestination.buffer = *outVertexBuffer;
    vertexDestination.size = vertexBufferInfo.size;
    SDL_UploadToGPUBuffer(copyPass, &vertexSource, &vertexDestination, false);

    SDL_GPUTransferBufferLocation indexSource{};
    indexSource.transfer_buffer = indexTransfer;
    SDL_GPUBufferRegion indexDestination{};
    indexDestination.buffer = *outIndexBuffer;
    indexDestination.size = indexBufferInfo.size;
    SDL_UploadToGPUBuffer(copyPass, &indexSource, &indexDestination, false);
    SDL_EndGPUCopyPass(copyPass);

    const bool submitted = SDL_SubmitGPUCommandBuffer(commandBuffer);
    SDL_ReleaseGPUTransferBuffer(m_device, vertexTransfer);
    SDL_ReleaseGPUTransferBuffer(m_device, indexTransfer);
    if (!submitted)
    {
        *error = std::string("Failed to submit imposter mesh upload: ") + SDL_GetError();
        return false;
    }

    return true;
}

bool OffscreenImposterRenderer::createMaterialResources(
    const ImportedPack& pack,
    const ImportedAsset& asset,
    std::unordered_map<std::uint32_t, MaterialTextures>* outMaterials,
    std::string* error) const
{
    outMaterials->clear();
    for (const std::uint32_t materialIndex : asset.materialIndices)
    {
        if (materialIndex >= pack.materials.size())
        {
            *error = "asset material index points past imported material count";
            return false;
        }

        const ImportedMaterial& material = pack.materials[materialIndex];
        if (isBillboardMaterial(material))
        {
            continue;
        }

        MaterialTextures textures{};
        auto useImportedOrDefault = [&](std::uint32_t textureIndex,
                                        const std::array<std::byte, 4>& defaultPixel,
                                        SDL_GPUTexture* defaultTexture,
                                        SDL_GPUTexture** outTexture) -> bool {
            if (textureIndex == std::numeric_limits<std::uint32_t>::max())
            {
                *outTexture = defaultTexture;
                return true;
            }
            return createMaterialTextureSet(pack, textureIndex, defaultPixel, outTexture, error);
        };

        if (!useImportedOrDefault(
                material.record.baseColorTextureIndex,
                { std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0xFF }, std::byte{ 0xFF } },
                m_defaultBaseColor,
                &textures.baseColor) ||
            !useImportedOrDefault(
                material.record.normalTextureIndex,
                { std::byte{ 0x80 }, std::byte{ 0x80 }, std::byte{ 0xFF }, std::byte{ 0xFF } },
                m_defaultNormal,
                &textures.normal) ||
            !useImportedOrDefault(
                material.record.roughnessTextureIndex,
                { std::byte{ 0xFF }, std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0xFF } },
                m_defaultRoughness,
                &textures.roughness) ||
            !useImportedOrDefault(
                material.record.specularTextureIndex,
                { std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0xFF } },
                m_defaultSpecular,
                &textures.specular) ||
            !useImportedOrDefault(
                material.record.aoTextureIndex,
                { std::byte{ 0xFF }, std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0xFF } },
                m_defaultAo,
                &textures.ao) ||
            !useImportedOrDefault(
                material.record.subsurfaceTextureIndex,
                { std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0x00 }, std::byte{ 0xFF } },
                m_defaultSubsurface,
                &textures.subsurface))
        {
            return false;
        }
        textures.alphaCutoff = material.record.alphaCutoff;

        outMaterials->emplace(materialIndex, textures);
    }
    return true;
}

const MaterialTextures& OffscreenImposterRenderer::materialTexturesFor(
    const std::unordered_map<std::uint32_t, MaterialTextures>& materials,
    std::uint32_t materialIndex) const
{
    const auto it = materials.find(materialIndex);
    if (it != materials.end())
    {
        return it->second;
    }

    static MaterialTextures defaults{};
    defaults.baseColor = m_defaultBaseColor;
    defaults.normal = m_defaultNormal;
    defaults.roughness = m_defaultRoughness;
    defaults.specular = m_defaultSpecular;
    defaults.ao = m_defaultAo;
    defaults.subsurface = m_defaultSubsurface;
    return defaults;
}

bool OffscreenImposterRenderer::downloadRenderTarget(
    SDL_GPUTexture* texture,
    SDL_GPUTransferBuffer* transferBuffer,
    std::vector<std::byte>* outPixels,
    std::string* error) const
{
    outPixels->assign(static_cast<std::size_t>(kImposterResolution) * kImposterResolution * 4u, std::byte{});
    void* mapped = SDL_MapGPUTransferBuffer(m_device, transferBuffer, false);
    if (mapped == nullptr)
    {
        *error = std::string("Failed to map imposter readback buffer: ") + SDL_GetError();
        return false;
    }
    std::memcpy(outPixels->data(), mapped, outPixels->size());
    SDL_UnmapGPUTransferBuffer(m_device, transferBuffer);
    (void)texture;
    return true;
}

bool OffscreenImposterRenderer::renderLayer(
    const AggregateAssetGeometry& geometry,
    SDL_GPUBuffer* vertexBuffer,
    SDL_GPUBuffer* indexBuffer,
    const std::unordered_map<std::uint32_t, MaterialTextures>& materials,
    const CaptureView& view,
    std::vector<std::byte>* outColorPixels,
    std::vector<std::byte>* outNormalPixels,
    std::string* error) const
{
    SDL_GPUCommandBuffer* commandBuffer = SDL_AcquireGPUCommandBuffer(m_device);
    if (commandBuffer == nullptr)
    {
        *error = std::string("Failed to acquire imposter command buffer: ") + SDL_GetError();
        return false;
    }

    const SDL_GPUBufferBinding vertexBinding{ vertexBuffer, 0 };
    const SDL_GPUBufferBinding indexBinding{ indexBuffer, 0 };
    const SDL_GPUViewport viewport{
        0.0f, 0.0f,
        static_cast<float>(kImposterResolution),
        static_cast<float>(kImposterResolution),
        0.0f, 1.0f
    };
    const SDL_Rect scissor{
        0, 0,
        static_cast<int>(kImposterResolution),
        static_cast<int>(kImposterResolution)
    };

    auto renderPassForTarget = [&](SDL_GPUTexture* colorTarget, SDL_GPUGraphicsPipeline* pipeline) -> bool {
        SDL_GPUColorTargetInfo colorTargetInfo{};
        colorTargetInfo.texture = colorTarget;
        colorTargetInfo.clear_color = SDL_FColor{ 0.0f, 0.0f, 0.0f, 0.0f };
        colorTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        colorTargetInfo.store_op = SDL_GPU_STOREOP_STORE;

        SDL_GPUDepthStencilTargetInfo depthTargetInfo{};
        depthTargetInfo.texture = m_depthTarget;
        depthTargetInfo.clear_depth = 1.0f;
        depthTargetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
        depthTargetInfo.store_op = SDL_GPU_STOREOP_STORE;
        depthTargetInfo.stencil_load_op = SDL_GPU_LOADOP_CLEAR;
        depthTargetInfo.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
        depthTargetInfo.clear_stencil = 0u;

        SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(commandBuffer, &colorTargetInfo, 1, &depthTargetInfo);
        if (renderPass == nullptr)
        {
            *error = std::string("Failed to begin imposter render pass: ") + SDL_GetError();
            return false;
        }

        SDL_BindGPUGraphicsPipeline(renderPass, pipeline);
        SDL_SetGPUViewport(renderPass, &viewport);
        SDL_SetGPUScissor(renderPass, &scissor);
        SDL_BindGPUVertexBuffers(renderPass, 0, &vertexBinding, 1);
        SDL_BindGPUIndexBuffer(renderPass, &indexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);

        VertexUniforms vertexUniforms{};
        vertexUniforms.viewProjection = view.viewProjection;
        SDL_PushGPUVertexUniformData(commandBuffer, 0, &vertexUniforms, sizeof(vertexUniforms));

        const Float3 sunDirection = normalize({ 0.45f, 0.72f, 0.53f });
        for (const AggregateDraw& draw : geometry.draws)
        {
            const MaterialTextures& textures = materialTexturesFor(materials, draw.materialIndex);
            SDL_GPUTextureSamplerBinding samplerBindings[6]{
                { textures.baseColor, m_sampler },
                { textures.normal, m_sampler },
                { textures.roughness, m_sampler },
                { textures.specular, m_sampler },
                { textures.ao, m_sampler },
                { textures.subsurface, m_sampler },
            };
            SDL_BindGPUFragmentSamplers(renderPass, 0, samplerBindings, 6);

            FragmentUniforms fragmentUniforms{};
            fragmentUniforms.viewBasisRight = { view.viewBasisRight.x, view.viewBasisRight.y, view.viewBasisRight.z, 0.0f };
            fragmentUniforms.viewBasisUp = { view.viewBasisUp.x, view.viewBasisUp.y, view.viewBasisUp.z, 0.0f };
            fragmentUniforms.viewBasisForward = { view.viewBasisForward.x, view.viewBasisForward.y, view.viewBasisForward.z, 0.0f };
            fragmentUniforms.sunDirectionIntensity = { sunDirection.x, sunDirection.y, sunDirection.z, 1.0f };
            fragmentUniforms.sunColorAmbient = { 1.0f, 0.97f, 0.91f, 0.22f };
            fragmentUniforms.shadingParams0 = { 0.04f, 1.0f, 0.45f, 0.0f };
            fragmentUniforms.cameraPositionAlphaCutoff = {
                view.cameraPosition.x,
                view.cameraPosition.y,
                view.cameraPosition.z,
                textures.alphaCutoff
            };
            SDL_PushGPUFragmentUniformData(commandBuffer, 0, &fragmentUniforms, sizeof(fragmentUniforms));
            SDL_DrawGPUIndexedPrimitives(renderPass, draw.indexCount, 1u, draw.firstIndex, 0, 0u);
        }

        SDL_EndGPURenderPass(renderPass);
        return true;
    };

    if (!renderPassForTarget(m_colorTarget, m_colorPipeline) ||
        !renderPassForTarget(m_normalTarget, m_normalPipeline))
    {
        SDL_CancelGPUCommandBuffer(commandBuffer);
        return false;
    }

    SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(commandBuffer);
    if (copyPass == nullptr)
    {
        SDL_CancelGPUCommandBuffer(commandBuffer);
        *error = std::string("Failed to begin imposter readback copy pass: ") + SDL_GetError();
        return false;
    }

    SDL_GPUTextureRegion colorSource{};
    colorSource.texture = m_colorTarget;
    colorSource.w = kImposterResolution;
    colorSource.h = kImposterResolution;
    colorSource.d = 1u;
    SDL_GPUTextureTransferInfo colorDestination{};
    colorDestination.transfer_buffer = m_colorReadback;
    colorDestination.pixels_per_row = kImposterResolution;
    colorDestination.rows_per_layer = kImposterResolution;
    SDL_DownloadFromGPUTexture(copyPass, &colorSource, &colorDestination);

    SDL_GPUTextureRegion normalSource{};
    normalSource.texture = m_normalTarget;
    normalSource.w = kImposterResolution;
    normalSource.h = kImposterResolution;
    normalSource.d = 1u;
    SDL_GPUTextureTransferInfo normalDestination{};
    normalDestination.transfer_buffer = m_normalReadback;
    normalDestination.pixels_per_row = kImposterResolution;
    normalDestination.rows_per_layer = kImposterResolution;
    SDL_DownloadFromGPUTexture(copyPass, &normalSource, &normalDestination);
    SDL_EndGPUCopyPass(copyPass);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commandBuffer);
    if (fence == nullptr)
    {
        *error = std::string("Failed to submit imposter capture command buffer: ") + SDL_GetError();
        return false;
    }

    SDL_GPUFence* fences[]{ fence };
    const bool waited = SDL_WaitForGPUFences(m_device, true, fences, 1u);
    SDL_ReleaseGPUFence(m_device, fence);
    if (!waited)
    {
        *error = std::string("Failed while waiting for imposter capture fence: ") + SDL_GetError();
        return false;
    }

    return downloadRenderTarget(m_colorTarget, m_colorReadback, outColorPixels, error) &&
        downloadRenderTarget(m_normalTarget, m_normalReadback, outNormalPixels, error);
}

bool OffscreenImposterRenderer::captureAsset(
    const ImportedPack& pack,
    const ImportedAsset& asset,
    std::vector<std::vector<std::byte>>* outColorLayers,
    std::vector<std::vector<std::byte>>* outNormalLayers,
    std::string* error)
{
    AggregateAssetGeometry geometry;
    if (!buildAggregateGeometry(pack, asset, &geometry, error))
    {
        return false;
    }

    SDL_GPUBuffer* vertexBuffer = nullptr;
    SDL_GPUBuffer* indexBuffer = nullptr;
    if (!createAssetGeometryBuffers(geometry, &vertexBuffer, &indexBuffer, error))
    {
        return false;
    }

    std::unordered_map<std::uint32_t, MaterialTextures> materials;
    if (!createMaterialResources(pack, asset, &materials, error))
    {
        releaseBuffer(&indexBuffer);
        releaseBuffer(&vertexBuffer);
        return false;
    }

    outColorLayers->clear();
    outNormalLayers->clear();
    outColorLayers->reserve(kLayerCount);
    outNormalLayers->reserve(kLayerCount);

    bool success = true;
    for (std::uint32_t pitchIndex = 0; pitchIndex < kPitchViewCount && success; ++pitchIndex)
    {
        for (std::uint32_t yawIndex = 0; yawIndex < kYawViewCount && success; ++yawIndex)
        {
            CaptureView view = buildCaptureView(geometry, pitchIndex, yawIndex);
            std::vector<std::byte> colorPixels;
            std::vector<std::byte> normalPixels;
            success = renderLayer(
                geometry,
                vertexBuffer,
                indexBuffer,
                materials,
                view,
                &colorPixels,
                &normalPixels,
                error);
            if (success)
            {
                dilateRgbAroundAlphaEdges(&colorPixels, kImposterResolution, kImposterResolution);
                dilateRgbAroundAlphaEdges(&normalPixels, kImposterResolution, kImposterResolution);
                outColorLayers->push_back(std::move(colorPixels));
                outNormalLayers->push_back(std::move(normalPixels));
            }
        }
    }

    for (auto& [materialIndex, textureSet] : materials)
    {
        (void)materialIndex;
        auto releaseIfOwned = [&](SDL_GPUTexture** texture, SDL_GPUTexture* defaultTexture) {
            if (*texture != nullptr && *texture != defaultTexture)
            {
                SDL_ReleaseGPUTexture(m_device, *texture);
            }
            *texture = nullptr;
        };
        releaseIfOwned(&textureSet.baseColor, m_defaultBaseColor);
        releaseIfOwned(&textureSet.normal, m_defaultNormal);
        releaseIfOwned(&textureSet.roughness, m_defaultRoughness);
        releaseIfOwned(&textureSet.specular, m_defaultSpecular);
        releaseIfOwned(&textureSet.ao, m_defaultAo);
        releaseIfOwned(&textureSet.subsurface, m_defaultSubsurface);
    }

    releaseBuffer(&indexBuffer);
    releaseBuffer(&vertexBuffer);
    return success;
}

} // namespace

bool GeneratePineImposters(
    ImportedPack* pack,
    std::string* error)
{
    if (!ConverterHasCompressonatorSupport())
    {
        *error = ConverterCompressonatorUnavailableReason();
        return false;
    }

    OffscreenImposterRenderer renderer;
    if (!renderer.initialize(error))
    {
        renderer.shutdown();
        return false;
    }

    bool success = true;
    for (ImportedAsset& asset : pack->assets)
    {
        std::vector<std::vector<std::byte>> colorLayers;
        std::vector<std::vector<std::byte>> normalLayers;
        success = renderer.captureAsset(*pack, asset, &colorLayers, &normalLayers, error);
        if (!success)
        {
            break;
        }

        std::vector<std::byte> colorPayload;
        success = buildCompressedTexturePayload(
            RuntimeAssets::TextureFormat::BC3_RGBA_UNORM,
            colorLayers,
            &colorPayload,
            error);
        if (!success)
        {
            break;
        }

        std::vector<std::byte> normalPayload;
        success = buildCompressedTexturePayload(
            RuntimeAssets::TextureFormat::BC5_RG_UNORM,
            normalLayers,
            &normalPayload,
            error);
        if (!success)
        {
            break;
        }

        const std::uint32_t colorTextureIndex = static_cast<std::uint32_t>(pack->textures.size());
        pack->textures.push_back(buildGeneratedTexture(
            asset.name,
            "imposter_coloralpha",
            RuntimeAssets::TextureFormat::BC3_RGBA_UNORM,
            std::move(colorPayload)));
        pack->textureIndexByBasename[pack->textures.back().normalizedBasename] = colorTextureIndex;

        const std::uint32_t normalTextureIndex = static_cast<std::uint32_t>(pack->textures.size());
        pack->textures.push_back(buildGeneratedTexture(
            asset.name,
            "imposter_normal",
            RuntimeAssets::TextureFormat::BC5_RG_UNORM,
            std::move(normalPayload)));
        pack->textureIndexByBasename[pack->textures.back().normalizedBasename] = normalTextureIndex;

        asset.imposterColorTextureIndex = colorTextureIndex;
        asset.imposterNormalTextureIndex = normalTextureIndex;
    }

    renderer.shutdown();
    return success;
}
