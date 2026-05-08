#include "RuntimeAssetCompression.hpp"

#include <lz4.h>

#include <limits>

namespace RuntimeAssets
{
namespace
{

bool SizeFitsInt(std::size_t value)
{
    return value <= static_cast<std::size_t>(std::numeric_limits<int>::max());
}

} // namespace

bool CompressBytes(
    CompressionType compressionType,
    std::span<const std::byte> source,
    std::vector<std::byte>* out,
    std::string* error)
{
    out->clear();

    switch (compressionType)
    {
    case CompressionType::None:
        out->assign(source.begin(), source.end());
        return true;

    case CompressionType::Lz4:
    {
        if (!SizeFitsInt(source.size()))
        {
            if (error != nullptr)
            {
                *error = "source buffer is too large for LZ4 compression";
            }
            return false;
        }

        const int sourceSize = static_cast<int>(source.size());
        const int maxCompressedSize = LZ4_compressBound(sourceSize);
        out->assign(static_cast<std::size_t>(maxCompressedSize), std::byte{});
        const int compressedSize = LZ4_compress_default(
            reinterpret_cast<const char*>(source.data()),
            reinterpret_cast<char*>(out->data()),
            sourceSize,
            maxCompressedSize);
        if (compressedSize <= 0)
        {
            if (error != nullptr)
            {
                *error = "LZ4 compression failed";
            }
            out->clear();
            return false;
        }

        out->resize(static_cast<std::size_t>(compressedSize));
        return true;
    }
    }

    if (error != nullptr)
    {
        *error = "unsupported compression type";
    }
    return false;
}

bool DecompressBytes(
    CompressionType compressionType,
    std::span<const std::byte> source,
    std::size_t uncompressedSize,
    std::vector<std::byte>* out,
    std::string* error)
{
    out->clear();

    switch (compressionType)
    {
    case CompressionType::None:
        if (source.size() != uncompressedSize)
        {
            if (error != nullptr)
            {
                *error = "uncompressed blob size mismatch";
            }
            return false;
        }
        out->assign(source.begin(), source.end());
        return true;

    case CompressionType::Lz4:
    {
        if (!SizeFitsInt(source.size()) || !SizeFitsInt(uncompressedSize))
        {
            if (error != nullptr)
            {
                *error = "buffer is too large for LZ4 decompression";
            }
            return false;
        }

        out->assign(uncompressedSize, std::byte{});
        const int decompressedSize = LZ4_decompress_safe(
            reinterpret_cast<const char*>(source.data()),
            reinterpret_cast<char*>(out->data()),
            static_cast<int>(source.size()),
            static_cast<int>(uncompressedSize));
        if (decompressedSize < 0 || static_cast<std::size_t>(decompressedSize) != uncompressedSize)
        {
            if (error != nullptr)
            {
                *error = "LZ4 decompression failed";
            }
            out->clear();
            return false;
        }
        return true;
    }
    }

    if (error != nullptr)
    {
        *error = "unsupported compression type";
    }
    return false;
}

} // namespace RuntimeAssets
