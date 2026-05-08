#include "CompressonatorBcCodec.hpp"

#if defined(CONVERTER_HAS_COMPRESSONATOR)
#include <compressonator.h>
#endif

#include "../../src/assets/RuntimeAssetFormat.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace
{

constexpr const char* kCompressonatorUnavailableReason =
    "Compressonator BC compression support is unavailable. Reconfigure tools/converter with "
    "CONVERTER_ENABLE_COMPRESSONATOR=ON and a valid COMPRESSONATOR_SDK_ROOT.";

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

#if defined(CONVERTER_HAS_COMPRESSONATOR)
bool ConvertToCompressedFormat(
    std::span<const std::byte> rgbaPixels,
    std::uint32_t width,
    std::uint32_t height,
    CMP_FORMAT destinationFormat,
    std::vector<std::byte>* outCompressedBlocks,
    std::string* error)
{
    CMP_Texture sourceTexture{};
    sourceTexture.dwSize = sizeof(sourceTexture);
    sourceTexture.dwWidth = width;
    sourceTexture.dwHeight = height;
    sourceTexture.dwPitch = width * 4u;
    sourceTexture.format = CMP_FORMAT_RGBA_8888;
    sourceTexture.dwDataSize = static_cast<CMP_DWORD>(rgbaPixels.size());
    sourceTexture.pData = const_cast<CMP_BYTE*>(reinterpret_cast<const CMP_BYTE*>(rgbaPixels.data()));
    sourceTexture.nBlockWidth = 4u;
    sourceTexture.nBlockHeight = 4u;
    sourceTexture.nBlockDepth = 1u;

    CMP_Texture destinationTexture{};
    destinationTexture.dwSize = sizeof(destinationTexture);
    destinationTexture.dwWidth = width;
    destinationTexture.dwHeight = height;
    destinationTexture.dwPitch = 0u;
    destinationTexture.format = destinationFormat;
    destinationTexture.nBlockWidth = 4u;
    destinationTexture.nBlockHeight = 4u;
    destinationTexture.nBlockDepth = 1u;
    destinationTexture.dwDataSize = CMP_CalculateBufferSize(&destinationTexture);
    outCompressedBlocks->assign(destinationTexture.dwDataSize, std::byte{});
    destinationTexture.pData = reinterpret_cast<CMP_BYTE*>(outCompressedBlocks->data());

    CMP_CompressOptions options{};
    options.dwSize = sizeof(options);
    options.fquality = 1.0f;
    options.dwnumThreads = 0u;
    options.SourceFormat = sourceTexture.format;
    options.DestFormat = destinationTexture.format;

    const CMP_ERROR convertResult = CMP_ConvertTexture(
        &sourceTexture,
        &destinationTexture,
        &options,
        nullptr);
    if (convertResult != CMP_OK)
    {
        *error = "Compressonator CMP_ConvertTexture failed";
        outCompressedBlocks->clear();
        return false;
    }

    outCompressedBlocks->resize(destinationTexture.dwDataSize);
    return true;
}
#endif

} // namespace

bool ConverterHasCompressonatorSupport()
{
#if defined(CONVERTER_HAS_COMPRESSONATOR)
    return true;
#else
    return false;
#endif
}

const char* ConverterCompressonatorUnavailableReason()
{
    return kCompressonatorUnavailableReason;
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

#if !defined(CONVERTER_HAS_COMPRESSONATOR)
    *error = kCompressonatorUnavailableReason;
    return false;
#else
    const std::size_t expectedBytes = static_cast<std::size_t>(RuntimeAssets::CalculateTextureMipByteSize(
        RuntimeAssets::TextureFormat::BC3_RGBA_UNORM,
        width,
        height));
    if (!ConvertToCompressedFormat(rgbaPixels, width, height, CMP_FORMAT_BC3, outCompressedBlocks, error))
    {
        return false;
    }
    if (outCompressedBlocks->size() != expectedBytes)
    {
        *error = "Compressonator BC3 output size does not match BC block sizing rules";
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

#if !defined(CONVERTER_HAS_COMPRESSONATOR)
    *error = kCompressonatorUnavailableReason;
    return false;
#else
    const std::size_t expectedBytes = static_cast<std::size_t>(RuntimeAssets::CalculateTextureMipByteSize(
        RuntimeAssets::TextureFormat::BC5_RG_UNORM,
        width,
        height));
    if (!ConvertToCompressedFormat(rgbaPixels, width, height, CMP_FORMAT_BC5, outCompressedBlocks, error))
    {
        return false;
    }
    if (outCompressedBlocks->size() != expectedBytes)
    {
        *error = "Compressonator BC5 output size does not match BC block sizing rules";
        outCompressedBlocks->clear();
        return false;
    }
    return true;
#endif
}
