#include "BcTextureCompression.hpp"

#if defined(CONVERTER_HAS_DIRECTXTEX)
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <DirectXTex.h>
#include <dxgiformat.h>
#endif

#include "../../src/assets/RuntimeAssetFormat.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace
{

constexpr const char* kBcCompressionUnavailableReason =
    "BC compression support is unavailable. Reconfigure tools/converter with DirectXTex support enabled.";

bool ValidateSourceImage(
    std::span<const std::byte> rgbaPixels,
    std::uint32_t width,
    std::uint32_t height,
    std::string* error)
{
    const std::uint64_t expectedBytes =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4u;
    if (width == 0u || height == 0u)
    {
        *error = "BC compression source dimensions must be non-zero";
        return false;
    }
    if (rgbaPixels.size() != expectedBytes)
    {
        *error = "BC compression source byte count does not match RGBA dimensions";
        return false;
    }
    return true;
}

#if defined(CONVERTER_HAS_DIRECTXTEX)
bool ConvertToCompressedFormat(
    std::span<const std::byte> rgbaPixels,
    std::uint32_t width,
    std::uint32_t height,
    DXGI_FORMAT destinationFormat,
    DirectX::TEX_COMPRESS_FLAGS compressFlags,
    std::vector<std::byte>* outCompressedBlocks,
    std::string* error)
{
    DirectX::Image sourceImage{};
    sourceImage.width = width;
    sourceImage.height = height;
    sourceImage.format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sourceImage.rowPitch = static_cast<size_t>(width) * 4u;
    sourceImage.slicePitch = sourceImage.rowPitch * static_cast<size_t>(height);
    sourceImage.pixels = const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(rgbaPixels.data()));

    DirectX::ScratchImage compressedImage;
    const HRESULT hr = DirectX::Compress(
        sourceImage,
        destinationFormat,
        compressFlags,
        DirectX::TEX_THRESHOLD_DEFAULT,
        compressedImage);
    if (FAILED(hr))
    {
        *error = "DirectXTex Compress failed";
        outCompressedBlocks->clear();
        return false;
    }

    const DirectX::Image* mipImage = compressedImage.GetImage(0, 0, 0);
    if (mipImage == nullptr || mipImage->pixels == nullptr)
    {
        *error = "DirectXTex Compress did not return a valid BC image";
        outCompressedBlocks->clear();
        return false;
    }

    outCompressedBlocks->resize(mipImage->slicePitch);
    std::memcpy(outCompressedBlocks->data(), mipImage->pixels, mipImage->slicePitch);
    return true;
}
#endif

} // namespace

bool ConverterHasBcCompressionSupport()
{
#if defined(CONVERTER_HAS_DIRECTXTEX)
    return true;
#else
    return false;
#endif
}

const char* ConverterBcCompressionUnavailableReason()
{
    return kBcCompressionUnavailableReason;
}

bool ConverterCompressBc3Rgba(
    std::span<const std::byte> rgbaPixels,
    std::uint32_t width,
    std::uint32_t height,
    std::vector<std::byte>* outCompressedBlocks,
    std::string* error)
{
    outCompressedBlocks->clear();
    if (!ValidateSourceImage(rgbaPixels, width, height, error))
    {
        return false;
    }

#if !defined(CONVERTER_HAS_DIRECTXTEX)
    *error = kBcCompressionUnavailableReason;
    return false;
#else
    const std::size_t expectedBytes = static_cast<std::size_t>(RuntimeAssets::CalculateTextureMipByteSize(
        RuntimeAssets::TextureFormat::BC3_RGBA_UNORM,
        width,
        height));
    if (!ConvertToCompressedFormat(
            rgbaPixels,
            width,
            height,
            DXGI_FORMAT_BC3_UNORM,
            DirectX::TEX_COMPRESS_DEFAULT,
            outCompressedBlocks,
            error))
    {
        return false;
    }
    if (outCompressedBlocks->size() != expectedBytes)
    {
        *error = "DirectXTex BC3 output size does not match BC block sizing rules";
        outCompressedBlocks->clear();
        return false;
    }
    return true;
#endif
}

bool ConverterCompressBc5Rg(
    std::span<const std::byte> rgbaPixels,
    std::uint32_t width,
    std::uint32_t height,
    std::vector<std::byte>* outCompressedBlocks,
    std::string* error)
{
    outCompressedBlocks->clear();
    if (!ValidateSourceImage(rgbaPixels, width, height, error))
    {
        return false;
    }

#if !defined(CONVERTER_HAS_DIRECTXTEX)
    *error = kBcCompressionUnavailableReason;
    return false;
#else
    const std::size_t expectedBytes = static_cast<std::size_t>(RuntimeAssets::CalculateTextureMipByteSize(
        RuntimeAssets::TextureFormat::BC5_RG_UNORM,
        width,
        height));
    if (!ConvertToCompressedFormat(
            rgbaPixels,
            width,
            height,
            DXGI_FORMAT_BC5_UNORM,
            DirectX::TEX_COMPRESS_UNIFORM,
            outCompressedBlocks,
            error))
    {
        return false;
    }
    if (outCompressedBlocks->size() != expectedBytes)
    {
        *error = "DirectXTex BC5 output size does not match BC block sizing rules";
        outCompressedBlocks->clear();
        return false;
    }
    return true;
#endif
}
