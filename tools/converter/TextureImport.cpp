#include "TextureImport.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <span>
#include <string_view>

namespace
{

std::string ToLower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

struct TgaHeader
{
    std::uint8_t idLength = 0;
    std::uint8_t colorMapType = 0;
    std::uint8_t imageType = 0;
    std::uint16_t colorMapOrigin = 0;
    std::uint16_t colorMapLength = 0;
    std::uint8_t colorMapDepth = 0;
    std::uint16_t xOrigin = 0;
    std::uint16_t yOrigin = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t pixelDepth = 0;
    std::uint8_t imageDescriptor = 0;
};

std::uint16_t ReadU16(const std::byte* bytes)
{
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[0])) |
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[1])) << 8u);
}

bool ParseTgaHeader(std::span<const std::byte> bytes, TgaHeader* outHeader)
{
    if (bytes.size() < 18)
    {
        return false;
    }
    outHeader->idLength = std::to_integer<std::uint8_t>(bytes[0]);
    outHeader->colorMapType = std::to_integer<std::uint8_t>(bytes[1]);
    outHeader->imageType = std::to_integer<std::uint8_t>(bytes[2]);
    outHeader->colorMapOrigin = ReadU16(bytes.data() + 3);
    outHeader->colorMapLength = ReadU16(bytes.data() + 5);
    outHeader->colorMapDepth = std::to_integer<std::uint8_t>(bytes[7]);
    outHeader->xOrigin = ReadU16(bytes.data() + 8);
    outHeader->yOrigin = ReadU16(bytes.data() + 10);
    outHeader->width = ReadU16(bytes.data() + 12);
    outHeader->height = ReadU16(bytes.data() + 14);
    outHeader->pixelDepth = std::to_integer<std::uint8_t>(bytes[16]);
    outHeader->imageDescriptor = std::to_integer<std::uint8_t>(bytes[17]);
    return true;
}

bool IsSrgbTexture(const std::string& lowerName)
{
    return lowerName.find("color") != std::string::npos ||
        lowerName.find("_bc") != std::string::npos ||
        lowerName.find("basecolor") != std::string::npos ||
        lowerName.find("diffuse") != std::string::npos;
}

bool DecodeTga(const std::filesystem::path& path, ImportedTexture* outTexture, std::string* error)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        *error = "failed to open texture: " + path.string();
        return false;
    }

    input.seekg(0, std::ios::end);
    const std::streamoff fileSize = input.tellg();
    input.seekg(0, std::ios::beg);
    if (fileSize <= 0)
    {
        *error = "texture is empty: " + path.string();
        return false;
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(fileSize));
    input.read(reinterpret_cast<char*>(bytes.data()), fileSize);
    if (!input)
    {
        *error = "failed to read texture: " + path.string();
        return false;
    }

    TgaHeader header;
    if (!ParseTgaHeader(bytes, &header))
    {
        *error = "texture header is too small: " + path.string();
        return false;
    }

    if (header.colorMapType != 0 || (header.imageType != 2 && header.imageType != 10))
    {
        *error = "only true-color TGA type 2/10 without color maps is supported: " + path.string();
        return false;
    }

    if (header.pixelDepth != 24 && header.pixelDepth != 32)
    {
        *error = "only 24-bit and 32-bit TGA are supported: " + path.string();
        return false;
    }

    const std::size_t pixelBytes = header.pixelDepth / 8u;
    const std::size_t imagePixelCount = static_cast<std::size_t>(header.width) * header.height;
    const std::size_t rgbaSize = imagePixelCount * 4u;
    outTexture->pixels.assign(rgbaSize, std::byte{});

    const std::size_t dataOffset = 18u + header.idLength;
    if (dataOffset > bytes.size())
    {
        *error = "TGA image data offset is invalid: " + path.string();
        return false;
    }

    std::size_t sourceOffset = dataOffset;
    std::size_t pixelIndex = 0;
    const auto writePixel = [&](std::size_t outIndex, const std::byte* srcPixel) {
        outTexture->pixels[outIndex + 0] = srcPixel[2];
        outTexture->pixels[outIndex + 1] = srcPixel[1];
        outTexture->pixels[outIndex + 2] = srcPixel[0];
        outTexture->pixels[outIndex + 3] = pixelBytes == 4 ? srcPixel[3] : std::byte{ 0xFF };
    };

    const auto emitPixel = [&](const std::byte* srcPixel) -> bool {
        if (pixelIndex >= imagePixelCount)
        {
            return false;
        }

        const std::size_t x = pixelIndex % header.width;
        const std::size_t y = pixelIndex / header.width;
        const bool topOrigin = (header.imageDescriptor & 0x20u) != 0;
        const std::size_t writeY = topOrigin ? y : (header.height - 1u - y);
        const std::size_t outIndex = ((writeY * header.width) + x) * 4u;
        writePixel(outIndex, srcPixel);
        ++pixelIndex;
        return true;
    };

    if (header.imageType == 2)
    {
        const std::size_t expectedBytes = imagePixelCount * pixelBytes;
        if (sourceOffset + expectedBytes > bytes.size())
        {
            *error = "TGA pixel payload is truncated: " + path.string();
            return false;
        }
        while (pixelIndex < imagePixelCount)
        {
            if (!emitPixel(bytes.data() + sourceOffset))
            {
                *error = "failed to emit TGA pixel";
                return false;
            }
            sourceOffset += pixelBytes;
        }
    }
    else
    {
        while (pixelIndex < imagePixelCount)
        {
            if (sourceOffset >= bytes.size())
            {
                *error = "RLE TGA payload is truncated: " + path.string();
                return false;
            }
            const std::uint8_t packetHeader = std::to_integer<std::uint8_t>(bytes[sourceOffset++]);
            const std::size_t runLength = (packetHeader & 0x7Fu) + 1u;
            if ((packetHeader & 0x80u) != 0)
            {
                if (sourceOffset + pixelBytes > bytes.size())
                {
                    *error = "RLE TGA packet is truncated: " + path.string();
                    return false;
                }
                const std::byte* srcPixel = bytes.data() + sourceOffset;
                sourceOffset += pixelBytes;
                for (std::size_t runIndex = 0; runIndex < runLength; ++runIndex)
                {
                    if (!emitPixel(srcPixel))
                    {
                        *error = "RLE TGA expands past the image bounds: " + path.string();
                        return false;
                    }
                }
            }
            else
            {
                const std::size_t blockSize = runLength * pixelBytes;
                if (sourceOffset + blockSize > bytes.size())
                {
                    *error = "raw TGA packet is truncated: " + path.string();
                    return false;
                }
                for (std::size_t runIndex = 0; runIndex < runLength; ++runIndex)
                {
                    if (!emitPixel(bytes.data() + sourceOffset))
                    {
                        *error = "raw TGA packet expands past the image bounds: " + path.string();
                        return false;
                    }
                    sourceOffset += pixelBytes;
                }
            }
        }
    }

    outTexture->width = header.width;
    outTexture->height = header.height;
    return true;
}

} // namespace

bool ImportTextureFolder(
    const std::filesystem::path& textureRoot,
    ImportedPack* outPack,
    std::string* error)
{
    if (!std::filesystem::exists(textureRoot))
    {
        *error = "Texture source folder does not exist: " + textureRoot.string();
        return false;
    }

    std::vector<std::filesystem::path> textureFiles;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(textureRoot))
    {
        if (entry.is_regular_file() && ToLower(entry.path().extension().string()) == ".tga")
        {
            textureFiles.push_back(entry.path());
        }
    }
    std::sort(textureFiles.begin(), textureFiles.end());

    for (const std::filesystem::path& texturePath : textureFiles)
    {
        const std::string basename = ToLower(texturePath.filename().string());
        if (outPack->textureIndexByBasename.contains(basename))
        {
            continue;
        }

        ImportedTexture texture;
        texture.name = ToLower(texturePath.stem().string());
        texture.normalizedBasename = basename;
        texture.sourcePath = texturePath.generic_string();
        if (!DecodeTga(texturePath, &texture, error))
        {
            return false;
        }

        if (IsSrgbTexture(texture.name))
        {
            texture.format = RuntimeAssets::TextureFormat::RGBA8_SRGB;
            texture.flags |= RuntimeAssets::TextureFlagSrgb;
        }
        else
        {
            texture.format = RuntimeAssets::TextureFormat::RGBA8_UNORM;
        }

        const std::uint32_t textureIndex = static_cast<std::uint32_t>(outPack->textures.size());
        outPack->textureIndexByBasename.emplace(basename, textureIndex);
        outPack->textures.push_back(std::move(texture));
    }

    return true;
}
