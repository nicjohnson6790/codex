#ifdef NDEBUG
#undef NDEBUG
#endif
#include "assets/RuntimeAssetReader.hpp"
#include "assets/FoliageNormalEncoding.hpp"
#include <cassert>
#include <cstring>
#include <limits>
#include <iostream>

int main()
{
    using namespace RuntimeAssets;
    // Equal XY with opposite Z must remain distinct through normal mip input.
    // The old positive-sqrt decoder folded both onto the same hemisphere.
    const std::byte front[] = {std::byte{128},std::byte{128},std::byte{255},std::byte{255}};
    const std::byte back[] = {std::byte{128},std::byte{128},std::byte{0},std::byte{255}};
    const auto frontNormal=DecodeFoliageNormal(front), backNormal=DecodeFoliageNormal(back);
    assert(frontNormal[2]==1.0f && backNormal[2]==-1.0f);
    assert(frontNormal[0]==backNormal[0] && frontNormal[1]==backNormal[1]);
    assert((frontNormal[2]+backNormal[2])*0.5f==0.0f);
    AssetBinHeader header{};
    header.magic=kAssetBinMagic; header.version=kFormatVersion; header.flags=kLittleEndianFlag;
    header.assetCount=1; header.textureBlobCount=4;
    header.assetRecordOffset=sizeof(header);
    header.materialRecordOffset=sizeof(header)+sizeof(AssetRecord)+sizeof(FoliageCaptureRecord);
    header.textureBlobRecordOffset=header.materialRecordOffset;
    header.stringTableOffset=header.textureBlobRecordOffset+4*sizeof(TextureBlobRecord);
    header.stringTableSize=1; header.fileSize=header.stringTableOffset+1;
    AssetRecord asset{};
    asset.captureMetadataOffset=sizeof(header)+sizeof(asset);
    asset.imposterColorTextureIndex=0; asset.imposterNormalTextureIndex=1;
    FoliageCaptureRecord capture{};
    capture.canopyColorTextureIndex=2; capture.canopyNormalTextureIndex=3;
    capture.centerAndRadius={-2,12,1,8}; capture.pitchHalfHeights={14,16,15,11};
    capture.canopyCenterAndHalfExtents={-2,1,8,8};
    std::vector<std::byte> bytes(header.fileSize);
    const auto write=[&] {
        std::memcpy(bytes.data(),&header,sizeof(header));
        std::memcpy(bytes.data()+header.assetRecordOffset,&asset,sizeof(asset));
        std::memcpy(bytes.data()+sizeof(header)+sizeof(asset),&capture,sizeof(capture));
    };
    std::string error;
    write(); assert(ValidateAssetBin(bytes.data(),bytes.size(),&error));
    FoliageCaptureRecord restored{};
    std::memcpy(&restored,bytes.data()+asset.captureMetadataOffset,sizeof(restored));
    assert(restored.centerAndRadius==capture.centerAndRadius);
    assert(restored.pitchHalfHeights==capture.pitchHalfHeights);
    assert(restored.canopyCenterAndHalfExtents==capture.canopyCenterAndHalfExtents);
    const auto valid=capture;
    capture.version=1; write(); assert(!ValidateAssetBin(bytes.data(),bytes.size(),&error));
    capture.version=3; write(); assert(!ValidateAssetBin(bytes.data(),bytes.size(),&error));
    capture=valid; capture.canopyNormalTextureIndex=4; write(); assert(!ValidateAssetBin(bytes.data(),bytes.size(),&error));
    capture=valid; capture.pitchHalfHeights[2]=0; write(); assert(!ValidateAssetBin(bytes.data(),bytes.size(),&error));
    capture=valid; capture.centerAndRadius[0]=std::numeric_limits<float>::quiet_NaN();
    write(); assert(!ValidateAssetBin(bytes.data(),bytes.size(),&error));
    capture=valid; capture.canopyCenterAndHalfExtents[2]=300;
    write(); assert(!ValidateAssetBin(bytes.data(),bytes.size(),&error));
    capture=valid; asset.captureMetadataOffset=0; write(); assert(!ValidateAssetBin(bytes.data(),bytes.size(),&error));
    asset.captureMetadataOffset=sizeof(header); write(); assert(!ValidateAssetBin(bytes.data(),bytes.size(),&error));
    asset.captureMetadataOffset=sizeof(header)+sizeof(asset); write();
    assert(!ValidateAssetBin(bytes.data(),bytes.size()-1,&error));
    std::cout << "Foliage capture metadata round trip and rejection tests passed.\n";
}
