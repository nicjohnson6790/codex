#version 450
#extension GL_GOOGLE_include_directive : require
#define FOLIAGE_CLOUD_BINDING 2
#define FOLIAGE_NOISE_BINDING 3
#define SKY_PROBE_COEFFICIENT_BINDING 7
#define SKY_PROBE_REGION_BINDING 8
#include "foliage_lighting.glsl"


#include "foliage_common.glsl"

layout(set=3, binding=0) uniform FoliageCanopyUniforms
{
    mat4 viewProjection;
    vec4 sunDirectionIntensity;
    vec4 canopyShellParams;
} canopy;

struct CanopyDrawData
{
    vec4 patchOriginAndSize;
    vec4 terrainOriginAndSize;
    vec4 terrainSliceData;
    uvec4 patchSeedData;
    uvec4 cellSlots[25];
    uvec4 cellSeeds[25];
};

layout(set=2, binding=4, std430) readonly buffer CanopyDrawMetadataBuffer
{
    CanopyDrawData draws[];
} drawMetadataBuffer;

layout(set=2, binding=5, std430) readonly buffer CanopyBitsetPoolBuffer
{
    uint canopyBitsets[];
} canopyBitsetPoolBuffer;

layout(location = 0) in vec2 fragPatchMeters;
layout(location = 1) in vec2 fragWorldXZ;
layout(location = 2) flat in uint fragDrawIndex;
layout(location = 3) flat in float fragShellY;
layout(location = 4) in vec3 fragPosition;

layout(location = 0) out vec4 outColor;

const float kCellSizeMeters = 256.0;
const float kCandidateCellSizeMeters = 4.0;
const float kEdgeFadeWidthMeters = 384.0;
const float kFadeInFrameCount = 12.0;
const uint kCandidateGridResolution = 64u;
const uint kCanopyBitsetWordCount = 128u;
layout(set=2,binding=0) uniform sampler2DArray canopyColor;
layout(set=2,binding=1) uniform sampler2DArray canopyNormal;
struct TreeClassData { vec4 centerAndHalfWidth; vec4 verticalExtentsAndLayerBase; vec4 pitchHalfHeights; vec4 footprint; };
layout(set=2,binding=6,std430) readonly buffer TreeClasses { TreeClassData classes[]; } trees;

bool findCell(vec2 patchMeters, out uint slot, out uint seed)
{
    ivec2 cell=ivec2(floor(patchMeters/256.0))+ivec2(1);
    int side=int(round(drawMetadataBuffer.draws[fragDrawIndex].patchOriginAndSize.w/256.0));
    if(any(lessThan(cell,ivec2(0))) || any(greaterThan(cell,ivec2(side+1)))) return false;
    uint index=uint(cell.y*10+cell.x);
    slot=drawMetadataBuffer.draws[fragDrawIndex].cellSlots[index>>2u][index&3u];
    seed=drawMetadataBuffer.draws[fragDrawIndex].cellSeeds[index>>2u][index&3u];
    return slot!=0xFFFFFFFFu;
}

uint hashU32(uint seed)
{
    seed ^= seed >> 16u;
    seed *= 0x7feb352du;
    seed ^= seed >> 15u;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16u;
    return seed;
}

float unpackEdgeFadeStrength(uint packedStrengths, uint edgeIndex)
{
    return float((packedStrengths >> (edgeIndex * 8u)) & 0xFFu) / 255.0;
}

void main()
{
    vec2 patchDx = dFdx(fragPatchMeters);
    vec2 patchDy = dFdy(fragPatchMeters);
    // Load scalar fields only; dynamically indexing a local copy of the halo
    // arrays can materialize the entire descriptor in per-fragment storage.
    vec4 patchBounds = drawMetadataBuffer.draws[fragDrawIndex].patchOriginAndSize;
    uvec4 patchSeed = drawMetadataBuffer.draws[fragDrawIndex].patchSeedData;
    vec2 clampedPatchMeters = clamp(fragPatchMeters, vec2(0.0), vec2(patchBounds.w));
    uint cellsPerSide = clamp(uint(round(patchBounds.w / kCellSizeMeters)), 1u, 8u);
    vec2 maxCellCoord = vec2(float(cellsPerSide - 1u));
    vec2 cellCoord = min(floor(clampedPatchMeters / kCellSizeMeters), maxCellCoord);
    uvec2 cellIndex = uvec2(cellCoord);
    uint flatCellIndex = (cellIndex.y * cellsPerSide) + cellIndex.x;

    uint edgeFadeStrengths = patchSeed.y;
    float edgeFade = 1.0;
    float westStrength = unpackEdgeFadeStrength(edgeFadeStrengths, 0u);
    if (westStrength > 0.0)
    {
        float westBand = clamp(clampedPatchMeters.x / kEdgeFadeWidthMeters, 0.0, 1.0);
        edgeFade = min(edgeFade, mix(1.0, westBand, westStrength));
    }
    float southStrength = unpackEdgeFadeStrength(edgeFadeStrengths, 1u);
    if (southStrength > 0.0)
    {
        float southBand = clamp(clampedPatchMeters.y / kEdgeFadeWidthMeters, 0.0, 1.0);
        edgeFade = min(edgeFade, mix(1.0, southBand, southStrength));
    }
    float eastStrength = unpackEdgeFadeStrength(edgeFadeStrengths, 2u);
    if (eastStrength > 0.0)
    {
        float eastBand = clamp((patchBounds.w - clampedPatchMeters.x) / kEdgeFadeWidthMeters, 0.0, 1.0);
        edgeFade = min(edgeFade, mix(1.0, eastBand, eastStrength));
    }
    float northStrength = unpackEdgeFadeStrength(edgeFadeStrengths, 3u);
    if (northStrength > 0.0)
    {
        float northBand = clamp((patchBounds.w - clampedPatchMeters.y) / kEdgeFadeWidthMeters, 0.0, 1.0);
        edgeFade = min(edgeFade, mix(1.0, northBand, northStrength));
    }

    float fadeIn = clamp(float(patchSeed.z) / kFadeInFrameCount, 0.0, 1.0);
    float combinedFade = min(edgeFade, fadeIn);

    ivec2 fadeCell = ivec2(floor(fragPatchMeters * 0.125));
    uint fadeSeed =
        patchSeed.x ^
        (uint(fadeCell.x) * 0x9e3779b9u) ^
        (uint(fadeCell.y) * 0x85ebca6bu) ^
        (flatCellIndex * 0xc2b2ae35u);
    if (foliageHash01(hashU32(fadeSeed)) > combinedFade)
    {
        discard;
    }

    uint shellIndex = uint(clamp(int(round(fragShellY)) - 1, 0, 2));
    bool covered = false;
    vec3 bestUvLayer = vec3(0);
    mat2 bestRotation = mat2(1);
    vec2 bestDx = vec2(0), bestDy = vec2(0);
    vec3 bestColor = vec3(0);
    uint bestSeed=0u;
    // Directly derive the four lattice candidates surrounding this pixel.
    // Do not clamp at cell borders: the halo maps each candidate to its owner.
    ivec2 baseCandidate=ivec2(floor(fragPatchMeters/kCandidateCellSizeMeters-vec2(0.5)));
    for(uint candidateIndex=0u;candidateIndex<4u;++candidateIndex) {
        ivec2 grid=baseCandidate+ivec2(int(candidateIndex&1u),int(candidateIndex>>1u));
        vec2 candidateCenter=(vec2(grid)+0.5)*kCandidateCellSizeMeters;
        uint slot,seed;
        if(!findCell(candidateCenter,slot,seed)) continue;
        uvec2 localGrid=uvec2(grid)&uvec2(kCandidateGridResolution-1u);
        uint candidateSlot=localGrid.y*kCandidateGridResolution+localGrid.x;
        uint word=canopyBitsetPoolBuffer.canopyBitsets[
            slot*kCanopyBitsetWordCount+(candidateSlot>>5u)];
        if((word&(1u<<(candidateSlot&31u)))==0u) continue;
        uint slotSeed=foliageCandidateSeed(seed,candidateSlot);
        if(covered && slotSeed<=bestSeed) continue;
        uint classCount=uint(canopy.canopyShellParams.y);
        uint treeClass=min(uint(floor(foliageHash01(slotSeed^0x68bc21ebu)*float(classCount))),classCount-1u);
        uint rotationBits=uint(floor(foliageHash01(slotSeed^0xa511e9b3u)*4096.0))&0xFFFu;
        float angle=float(rotationBits)*6.28318530718/4096.0;
        float c=cos(angle),sn=sin(angle);
        mat2 rotation=mat2(c,-sn,sn,c);
        vec2 local=transpose(rotation)*(fragPatchMeters-candidateCenter-foliageJitterOffset(slotSeed));
        vec4 footprint=trees.classes[treeClass].footprint;
        vec2 uv=(local-footprint.xy)/(2.0*footprint.zw)+0.5;
        // Capture up is -Z, and raster image rows increase downwards.
        if(any(lessThan(uv,vec2(0)))||any(greaterThan(uv,vec2(1)))) continue;
        float layer=float(treeClass*3u+shellIndex);
        vec2 uvDx = transpose(rotation) * patchDx / (2.0 * footprint.zw);
        vec2 uvDy = transpose(rotation) * patchDy / (2.0 * footprint.zw);
        vec4 sampleColor=textureGrad(canopyColor,vec3(uv,layer),uvDx,uvDy);
        // Coverage is a world-space cutout, not a view-angle LOD decision.
        // Grazing derivatives can otherwise select a coarse alpha mip that
        // rejects a whole crown. Keep color/normal filtering independent.
        if(textureLod(canopyColor,vec3(uv,layer),0.0).a<0.5) continue;
        covered=true; bestSeed=slotSeed; bestColor=sampleColor.rgb;
        bestUvLayer=vec3(uv,layer); bestRotation=rotation;
        bestDx=uvDx; bestDy=uvDy;
    }

    if (!covered)
    {
        discard;
    }

    vec3 bestNormal=textureGrad(canopyNormal,bestUvLayer,bestDx,bestDy).rgb*2.0-1.0;
    bestNormal.xz=bestRotation*bestNormal.xz;
    bestNormal=dot(bestNormal,bestNormal)>1e-8 ? normalize(bestNormal) : vec3(0,1,0);
    vec3 lightDirection = normalize(canopy.sunDirectionIntensity.xyz);
    float nDotL=max(dot(bestNormal,lightDirection),0.0);
    vec3 direct = nDotL > 0.0 ? foliageSun(fragPosition,lightDirection)*nDotL : vec3(0);
    vec3 color = bestColor / 3.14159265359 *
        (foliageSky(fragPosition,bestNormal,uint(drawMetadataBuffer.draws[fragDrawIndex].terrainSliceData.y)) +
         direct);

    outColor = vec4(color, 1.0);
}
