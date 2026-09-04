#include "WorldGridQuadtreeHeightmapManager.hpp"

#include "PerformanceCapture.hpp"
#include "QuadtreeMeshRenderer.hpp"

#include <SDL3/SDL_filesystem.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace
{
constexpr double kJapanDem10TileSizeMeters = 2550.0;
constexpr double kJapanDem10MaxContributionPitchMeters = 32.0;
constexpr double kJapanDem10OriginX = 11709172.9257765;
constexpr double kJapanDem10OriginZ = -903715.758822597;
constexpr double kJapanDem10XAxisX = 0.936672189248398;
constexpr double kJapanDem10XAxisZ = 0.350207381259467;
constexpr double kJapanDem10ZAxisX = -0.350207381259467;
constexpr double kJapanDem10ZAxisZ = 0.936672189248398;
constexpr double kJapanDem10MosaicSizeMeters = 256.0 * kJapanDem10TileSizeMeters;
constexpr std::array<std::array<int, 2>, 4> kJapanDem10MosaicOffsets{{{{0, 0}}, {{1, 0}}, {{1, 1}}, {{1, 2}}}};
constexpr std::array<const char *, 4> kJapanDem10MosaicIds{{"sw", "se", "ce", "ne"}};
constexpr HeightmapDatasetId kJapanDem10DatasetIdBase = 0x4a5044454d313000ULL;

std::uint64_t mix64(std::uint64_t x)
{
    x ^= x >> 30U;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27U;
    x *= 0x94d049bb133111ebULL;
    return x ^ (x >> 31U);
}
} // namespace

std::size_t WorldGridQuadtreeHeightmapManager::LeafIdHash::operator()(const WorldGridQuadtreeLeafId &id) const
{
    return mix64(std::bit_cast<std::uint64_t>(id.gridX)) ^ mix64(std::bit_cast<std::uint64_t>(id.gridY)) ^ mix64(id.subdivisionPath);
}
std::size_t WorldGridQuadtreeHeightmapManager::SourceTileHash::operator()(const SourceTileId &id) const
{
    return mix64(id.datasetId) ^ mix64(std::bit_cast<std::uint32_t>(id.tileX)) ^ mix64(std::bit_cast<std::uint32_t>(id.tileY));
}

WorldGridQuadtreeHeightmapManager::WorldGridQuadtreeHeightmapManager()
    : m_heightmaps(kCapacity, 256, 8), m_sourceTiles(kSourceTileCapacity, AppConfig::Terrain::kSourceHeightmapHashBucketCount,
                                                     AppConfig::Terrain::kSourceHeightmapHashLookupDepth),
      m_generationJobs(kCapacity)
{
    m_sourceReferences.reserve(kCapacity * 8);
    std::string error;
    std::filesystem::path assetDirectory = TERRAIN_SANDBOX_ASSET_DIR;
    if (!std::filesystem::exists(assetDirectory / "etopo2022.assetbin"))
        if (const char *basePath = SDL_GetBasePath())
            assetDirectory = std::filesystem::path(basePath) / TERRAIN_SANDBOX_ASSET_DIR;
    const auto etopo = EtopoHeightmapDataset::open(assetDirectory / "etopo2022.assetbin", error);
    if (etopo)
    {
        addDataset(etopo);
        addSourceHeightmap({1,
                            etopo->datasetId(),
                            Position{},
                            {RuntimeAssets::kHeightmapTilePhysicalSizeMeters, 0.0},
                            1.0,
                            {0.0, -RuntimeAssets::kHeightmapTilePhysicalSizeMeters}});
    }
    else
        throw std::runtime_error("Failed to open ETOPO runtime heightmap dataset: " + error);

    // Converter coordinates are Airocean atlas (X,Y). Runtime terrain uses
    // world (X,Z), with atlas Y mapped to -Z, matching the ETOPO placement.
    const glm::dvec2 xTileAxis{kJapanDem10TileSizeMeters * kJapanDem10XAxisX,
                               -kJapanDem10TileSizeMeters * kJapanDem10XAxisZ};
    const glm::dvec2 zTileAxis{kJapanDem10TileSizeMeters * kJapanDem10ZAxisX,
                               -kJapanDem10TileSizeMeters * kJapanDem10ZAxisZ};
    for (std::size_t mosaic = 0; mosaic < kJapanDem10MosaicIds.size(); ++mosaic)
    {
        const auto offset = kJapanDem10MosaicOffsets[mosaic];
        const double mosaicOriginX = kJapanDem10OriginX + offset[0] * kJapanDem10MosaicSizeMeters * kJapanDem10XAxisX +
                                     offset[1] * kJapanDem10MosaicSizeMeters * kJapanDem10ZAxisX;
        const double mosaicOriginZ = -(kJapanDem10OriginZ + offset[0] * kJapanDem10MosaicSizeMeters * kJapanDem10XAxisZ +
                                       offset[1] * kJapanDem10MosaicSizeMeters * kJapanDem10ZAxisZ);
        const auto indexPath = assetDirectory / (std::string("japan_dem10_delta_") + kJapanDem10MosaicIds[mosaic] + ".assetbin");
        if (!std::filesystem::exists(indexPath))
            continue;
        error.clear();
        const HeightmapDatasetId datasetId = kJapanDem10DatasetIdBase + mosaic;
        const auto dataset = EtopoHeightmapDataset::open(indexPath, error, datasetId);
        if (!dataset)
            throw std::runtime_error("Failed to open Japan DEM10 runtime heightmap dataset: " + error);
        addDataset(dataset);
        const glm::dvec2 tileZero = glm::dvec2{mosaicOriginX, mosaicOriginZ} + 128.0 * (xTileAxis + zTileAxis);
        addSourceHeightmap({static_cast<SourceHeightmapId>(2 + mosaic), datasetId,
                            Position(0, 0, {tileZero.x, 0.0, tileZero.y}), xTileAxis, 1.0, zTileAxis,
                            kJapanDem10MaxContributionPitchMeters});
    }
    clearCache();
}

bool WorldGridQuadtreeHeightmapManager::addDataset(std::shared_ptr<HeightmapDataset> dataset)
{
    if (!dataset || findDataset(dataset->datasetId()))
        return false;
    m_datasets.push_back(std::move(dataset));
    return true;
}
bool WorldGridQuadtreeHeightmapManager::addSourceHeightmap(SourceHeightmap source)
{
    const double det = source.xTileAxis.x * source.zTileAxis.y - source.xTileAxis.y * source.zTileAxis.x;
    if (source.sourceHeightmapId == 0 || !findDataset(source.datasetId) || findSource(source.sourceHeightmapId) || std::abs(det) < 1e-12)
        return false;
    m_sources.push_back(source);
    return true;
}
bool WorldGridQuadtreeHeightmapManager::updateSourceHeightmap(SourceHeightmap source)
{
    const double det = source.xTileAxis.x * source.zTileAxis.y - source.xTileAxis.y * source.zTileAxis.x;
    const auto it = std::find_if(m_sources.begin(), m_sources.end(),
                                 [&](const auto &value) { return value.sourceHeightmapId == source.sourceHeightmapId; });
    if (it == m_sources.end() || !findDataset(source.datasetId) || std::abs(det) < 1e-12)
        return false;
    for (std::uint16_t slot = 0; slot < kCapacity; ++slot)
        if (m_heightmaps.isOpen(slot))
            return false; // Placement changes require an explicit full final-cache invalidation.
    *it = source;
    return true;
}
bool WorldGridQuadtreeHeightmapManager::removeSourceHeightmap(SourceHeightmapId sourceHeightmapId)
{
    const auto it =
        std::find_if(m_sources.begin(), m_sources.end(), [&](const auto &value) { return value.sourceHeightmapId == sourceHeightmapId; });
    if (it == m_sources.end())
        return false;
    for (std::uint16_t slot = 0; slot < kCapacity; ++slot)
        if (m_heightmaps.isOpen(slot))
            return false; // Preserve every sourceHeightmapId referenced before clearCache().
    m_sources.erase(it);
    return true;
}
const SourceHeightmap *WorldGridQuadtreeHeightmapManager::findSource(SourceHeightmapId id) const
{
    const auto it = std::find_if(m_sources.begin(), m_sources.end(), [&](const auto &s) { return s.sourceHeightmapId == id; });
    return it == m_sources.end() ? nullptr : &*it;
}
const HeightmapDataset *WorldGridQuadtreeHeightmapManager::findDataset(HeightmapDatasetId id) const
{
    const auto it = std::find_if(m_datasets.begin(), m_datasets.end(), [&](const auto &d) { return d->datasetId() == id; });
    return it == m_datasets.end() ? nullptr : it->get();
}
std::optional<std::uint16_t> WorldGridQuadtreeHeightmapManager::findSlot(const WorldGridQuadtreeLeafId &id) const
{
    return m_heightmaps.find(id);
}

void WorldGridQuadtreeHeightmapManager::allocateReferences(std::uint16_t slot, const WorldGridQuadtreeLeafId &leaf)
{
    FinalMetadata &meta = m_finalMetadata[slot];
    meta.referenceFront = static_cast<std::uint32_t>(m_sourceReferences.size());
    meta.referenceCount = 0;
    for (const auto &source : m_sources)
    {
        const double finalPitch = worldGridQuadtreeLeafSize(leaf) / AppConfig::Terrain::kHeightmapLeafIntervalCount;
        if (finalPitch > source.maxContributionPitch)
            continue;
        const auto *dataset = findDataset(source.datasetId);
        if (!dataset)
            continue;
        for (const SourceTileCoordinate tile : collectOverlappingSourceTiles(source, *dataset, leaf))
        {
            m_sourceReferences.push_back(
                {source.sourceHeightmapId, tile.x, tile.y, dataset->tileRevision(tile.x, tile.y), kSourceUnavailable});
            ++meta.referenceCount;
        }
    }
    m_stats.referenceHighWater = std::max<std::uint32_t>(m_stats.referenceHighWater, static_cast<std::uint32_t>(m_sourceReferences.size()));
}

std::uint16_t WorldGridQuadtreeHeightmapManager::requestAsset(const WorldGridQuadtreeLeafId &leaf, std::uint16_t hint)
{
    auto existing = (hint != kUnavailable && m_heightmaps.validatesHint(hint, leaf)) ? std::optional<std::uint16_t>(hint) : findSlot(leaf);
    if (existing)
    {
        m_heightmaps.touch(*existing);
        if (!m_heightmaps.isReady(*existing))
            makeResident(*existing);
        return m_heightmaps.isReady(*existing) ? *existing : kUnavailable;
    }
    const auto candidate = m_heightmaps.findAllocationCandidate();
    if (!candidate)
        return kUnavailable;
    if (m_heightmaps.isOpen(*candidate))
    {
        discardCurrentJob(*candidate);
        invalidateSlotMetadata(*candidate);
    }
    else
        ++m_residentCount;
    m_heightmaps.assign(*candidate, leaf);
    allocateReferences(*candidate, leaf);
    makeResident(*candidate);
    return kUnavailable;
}

std::uint16_t WorldGridQuadtreeHeightmapManager::requestSourceTile(const SourceTileId &id, std::uint64_t revision, std::uint16_t hint)
{
    auto found =
        (hint != kSourceUnavailable && m_sourceTiles.validatesHint(hint, id)) ? std::optional<std::uint16_t>(hint) : m_sourceTiles.find(id);
    if (found && m_sourceRevisions[*found] == revision)
    {
        m_sourceTiles.touch(*found);
        ++m_stats.sourceHits;
        return m_sourceTiles.isReady(*found) ? *found : kSourceUnavailable;
    }
    ++m_stats.sourceMisses;
    if (found)
    {
        auto &readFences = m_sourceReadFences[*found];
        std::erase_if(readFences, [](const auto &fence) { return !fence || fence->isSignaled(); });
        if (!readFences.empty() || (m_sourceUploads[*found].fence && !m_sourceUploads[*found].fence->isSignaled()))
            return kSourceUnavailable;
    }
    const auto candidate = found ? found : findSourceAllocationCandidate();
    if (!candidate)
        return kSourceUnavailable;
    if (m_sourceTiles.isOpen(*candidate))
        ++m_stats.sourceEvictions;
    const auto datasetIt =
        std::find_if(m_datasets.begin(), m_datasets.end(), [&](const auto &value) { return value->datasetId() == id.datasetId; });
    if (datasetIt == m_datasets.end())
        return kSourceUnavailable;
    const std::shared_ptr<HeightmapDataset> dataset = *datasetIt;
    auto load = std::async(std::launch::async, [dataset, id]() {
        SourceTileLoadResult result;
        dataset->loadTile(id.tileX, id.tileY, result.samples, result.error);
        return result;
    });
    m_sourceTiles.assign(*candidate, id);
    m_sourceRevisions[*candidate] = revision;
    SourceUpload &upload = m_sourceUploads[*candidate];
    upload.samples.clear();
    upload.revision = revision;
    upload.queuedToRenderer = false;
    upload.fence.reset();
    upload.load = std::move(load);
    ++m_stats.sourceLoads;
    return kSourceUnavailable;
}

std::optional<std::uint16_t> WorldGridQuadtreeHeightmapManager::findSourceAllocationCandidate()
{
    std::optional<std::uint16_t> oldest;
    for (std::uint16_t slot = 0; slot < kSourceTileCapacity; ++slot)
    {
        if (!m_sourceTiles.isOpen(slot))
            return slot;
        auto &fences = m_sourceReadFences[slot];
        std::erase_if(fences, [](const auto &fence) { return !fence || fence->isSignaled(); });
        const auto &upload = m_sourceUploads[slot];
        if (m_sourceTiles.ageOf(slot) == 0 || !fences.empty() || (upload.fence && !upload.fence->isSignaled()) || upload.load.valid())
            continue;
        if (!oldest || m_sourceTiles.ageOf(slot) > m_sourceTiles.ageOf(*oldest))
            oldest = slot;
    }
    return oldest;
}

void WorldGridQuadtreeHeightmapManager::discardCurrentJob(std::uint16_t slot)
{
    if (!m_heightmaps.isOpen(slot))
        return;
    const auto job = m_heightmaps.activeJob(slot);
    if (job.valid() && m_generationJobs.contains(job) && !m_generationJobs.isDiscarded(job))
    {
        m_generationJobs.discard(job);
        ++m_stats.discardedJobs;
    }
    m_heightmaps.clearActiveJob(slot, job);
}

bool WorldGridQuadtreeHeightmapManager::makeResident(std::uint16_t slot)
{
    bool available = true;
    auto &meta = m_finalMetadata[slot];
    for (std::uint32_t i = 0; i < meta.referenceCount; ++i)
    {
        auto &ref = m_sourceReferences[meta.referenceFront + i];
        const auto *source = findSource(ref.sourceHeightmapId);
        const auto *dataset = source ? findDataset(source->datasetId) : nullptr;
        if (!dataset)
        {
            available = false;
            continue;
        }
        const auto revision = dataset->tileRevision(ref.tileX, ref.tileY);
        if (ref.revision != revision)
        {
            discardCurrentJob(slot);
            ref.revision = revision;
            ref.lastHint = kSourceUnavailable;
        }
        const auto sourceSlot = requestSourceTile({source->datasetId, ref.tileX, ref.tileY}, ref.revision, ref.lastHint);
        if (sourceSlot == kSourceUnavailable)
            available = false;
        else
        {
            ref.lastHint = sourceSlot;
            m_sourceTiles.touch(sourceSlot);
        }
    }
    const auto active = m_heightmaps.activeJob(slot);
    if (available && (!active.valid() || !m_generationJobs.contains(active)))
    {
        const auto job = m_generationJobs.tryPush({slot});
        if (job)
            m_heightmaps.setActiveJob(slot, *job);
    }
    return available;
}

bool WorldGridQuadtreeHeightmapManager::buildDescriptors(std::uint16_t slot, std::vector<HeightmapSourceGpuDescriptor> &out)
{
    const auto leaf = m_heightmaps.assetId(slot);
    double minX, minZ, size;
    worldGridQuadtreeLeafExtents(leaf, minX, minZ, size);
    const double pitch = size / AppConfig::Terrain::kHeightmapLeafIntervalCount;
    minX -= pitch;
    minZ -= pitch;
    const auto &meta = m_finalMetadata[slot];
    for (std::uint32_t i = 0; i < meta.referenceCount; ++i)
    {
        auto &ref = m_sourceReferences[meta.referenceFront + i];
        const auto *s = findSource(ref.sourceHeightmapId);
        if (!s)
            return false;
        SourceTileId id{s->datasetId, ref.tileX, ref.tileY};
        auto sourceSlot = m_sourceTiles.find(id);
        if (!sourceSlot || !m_sourceTiles.isReady(*sourceSlot) || m_sourceRevisions[*sourceSlot] != ref.revision)
            return false;
        const double det = s->xTileAxis.x * s->zTileAxis.y - s->xTileAxis.y * s->zTileAxis.x;
        auto transform = [&](double rx, double rz) {
            return glm::dvec2((rx * s->zTileAxis.y - rz * s->zTileAxis.x) / det * 255.0,
                              (s->xTileAxis.x * rz - s->xTileAxis.y * rx) / det * 255.0);
        };
        const glm::dvec2 relative = positionOffsetXZ(leaf.gridX, leaf.gridY, {minX, minZ}, s->basePosition);
        const double rx = relative.x, rz = relative.y;
        const double tileOriginX = static_cast<double>(ref.tileX) * s->xTileAxis.x + static_cast<double>(ref.tileY) * s->zTileAxis.x;
        const double tileOriginZ = static_cast<double>(ref.tileX) * s->xTileAxis.y + static_cast<double>(ref.tileY) * s->zTileAxis.y;
        const glm::dvec2 origin = transform(rx - tileOriginX, rz - tileOriginZ), sx = transform(pitch, 0.0), sy = transform(0.0, pitch);
        const auto *dataset = findDataset(s->datasetId);
        out.push_back({{static_cast<float>(origin.x), static_cast<float>(origin.y), static_cast<float>(s->yScale), 0},
                       {static_cast<float>(sx.x), static_cast<float>(sx.y), static_cast<float>(sy.x), static_cast<float>(sy.y)},
                       {0.0f, 0.0f, 255.0f, 255.0f},
                       {*sourceSlot, dataset && !dataset->containsTile(ref.tileX - 1, ref.tileY) ? 1u : 0u,
                        dataset && !dataset->containsTile(ref.tileX, ref.tileY - 1) ? 1u : 0u, 0u}});
    }
    return true;
}

void WorldGridQuadtreeHeightmapManager::scheduleQueuedGenerations(QuadtreeMeshRenderer &renderer)
{
    HELLO_PROFILE_SCOPE("WorldGridQuadtreeHeightmapManager::ScheduleQueuedGenerations");
    for (std::uint16_t slot = 0; slot < kSourceTileCapacity; ++slot)
        if (m_sourceTiles.isOpen(slot) && !m_sourceTiles.isReady(slot) && !m_sourceUploads[slot].queuedToRenderer)
        {
            auto &upload = m_sourceUploads[slot];
            if (upload.load.valid())
            {
                if (upload.load.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                    continue;
                SourceTileLoadResult loaded = upload.load.get();
                if (!loaded.error.empty() || loaded.samples.size() != RuntimeAssets::kHeightmapTileSampleCount)
                {
                    m_sourceTiles.release(slot);
                    continue;
                }
                upload.samples = std::move(loaded.samples);
            }
            if (!upload.samples.empty() && renderer.queueSourceHeightmapUpload(slot, upload.samples))
            {
                upload.queuedToRenderer = true;
                ++m_stats.sourceUploads;
            }
        }
    std::uint16_t count = 0;
    std::uint32_t descriptorsUsed = 0;
    bool stopBatch = false;
    m_generationJobs.forEachActive([&](GenerationJobHandle handle) {
        if (stopBatch || count >= std::min(m_computeDispatchBudget, kMaxFinalHeightmapsPerDispatch) ||
            m_generationJobs.isCompleted(handle) || m_generationJobs.isDiscarded(handle) || m_generationJobs.isSubmitted(handle))
            return;
        const auto slot = m_generationJobs.job(handle).finalCacheSlot;
        std::vector<HeightmapSourceGpuDescriptor> descriptors;
        if (!buildDescriptors(slot, descriptors))
            return;
        if (descriptors.size() > kSourceDescriptorCapacity)
        {
            ++m_stats.descriptorOverflows;
            ++m_stats.discardedJobs;
            m_generationJobs.discard(handle);
            return;
        }
        if (descriptorsUsed + descriptors.size() > kSourceDescriptorCapacity)
        {
            stopBatch = true;
            return;
        }
        if (renderer.queueHeightmapGeneration(m_heightmaps.assetId(slot), slot, descriptors, handle))
        {
            descriptorsUsed += static_cast<std::uint32_t>(descriptors.size());
            ++count;
        }
    });
    m_stats.lastFinalGenerations = count;
    m_stats.lastSourceDescriptors = descriptorsUsed;
    m_generationJobs.retireCompletedFront();
}

void WorldGridQuadtreeHeightmapManager::markSourceUploadsSubmitted(std::span<const std::uint16_t> slots,
                                                                   const std::shared_ptr<SubmittedGpuFence> &fence)
{
    for (auto slot : slots)
        if (slot < kSourceTileCapacity && m_sourceTiles.isOpen(slot))
            m_sourceUploads[slot].fence = fence;
}
void WorldGridQuadtreeHeightmapManager::markSubmitted(GenerationJobHandle job, const std::shared_ptr<SubmittedGpuFence> &fence)
{
    if (m_generationJobs.contains(job))
    {
        m_generationJobs.markSubmitted(job, fence);
        const auto slot = m_generationJobs.job(job).finalCacheSlot;
        if (slot < kCapacity && m_heightmaps.isOpen(slot))
        {
            const auto &meta = m_finalMetadata[slot];
            for (std::uint32_t i = 0; i < meta.referenceCount; ++i)
            {
                const auto &ref = m_sourceReferences[meta.referenceFront + i];
                const auto *source = findSource(ref.sourceHeightmapId);
                if (!source)
                    continue;
                const auto sourceSlot = m_sourceTiles.find({source->datasetId, ref.tileX, ref.tileY});
                if (sourceSlot && m_sourceRevisions[*sourceSlot] == ref.revision)
                    m_sourceReadFences[*sourceSlot].push_back(fence);
            }
        }
        ++m_stats.submittedJobs;
    }
}
void WorldGridQuadtreeHeightmapManager::ageMap()
{
    m_generationJobs.retireSignaledDiscarded();
    m_heightmaps.age();
    m_sourceTiles.age();
    for (std::uint16_t slot = 0; slot < kSourceTileCapacity; ++slot)
        if (m_sourceTiles.isOpen(slot) && !m_sourceTiles.isReady(slot) && m_sourceUploads[slot].fence &&
            m_sourceUploads[slot].fence->isSignaled())
        {
            m_sourceTiles.markReady(slot);
            m_sourceUploads[slot].samples.clear();
            m_sourceUploads[slot].fence.reset();
        }
    for (auto &fences : m_sourceReadFences)
        std::erase_if(fences, [](const auto &fence) { return !fence || fence->isSignaled(); });
    for (std::uint16_t finalSlot = 0; finalSlot < kCapacity; ++finalSlot)
        if (m_heightmaps.isOpen(finalSlot))
        {
            const auto job = m_heightmaps.activeJob(finalSlot);
            if (!job.valid() || !m_generationJobs.contains(job) || m_generationJobs.isCompleted(job))
                continue;
            const auto &meta = m_finalMetadata[finalSlot];
            for (std::uint32_t i = 0; i < meta.referenceCount; ++i)
            {
                const auto &ref = m_sourceReferences[meta.referenceFront + i];
                const auto *source = findSource(ref.sourceHeightmapId);
                if (!source)
                    continue;
                const auto sourceSlot = m_sourceTiles.find({source->datasetId, ref.tileX, ref.tileY});
                if (sourceSlot && m_sourceRevisions[*sourceSlot] == ref.revision)
                    m_sourceTiles.touch(*sourceSlot);
            }
        }
    compactReferences();
}
void WorldGridQuadtreeHeightmapManager::invalidateSourceTile(const SourceTileId &id)
{
    for (std::uint16_t slot = 0; slot < kCapacity; ++slot)
        if (m_heightmaps.isOpen(slot))
        {
            auto &meta = m_finalMetadata[slot];
            for (std::uint32_t i = 0; i < meta.referenceCount; ++i)
            {
                auto &r = m_sourceReferences[meta.referenceFront + i];
                const auto *s = findSource(r.sourceHeightmapId);
                if (s && SourceTileId{s->datasetId, r.tileX, r.tileY} == id)
                {
                    discardCurrentJob(slot);
                    m_heightmaps.markNotReady(slot);
                    m_knownExtentsValid[slot] = false;
                    m_cpuHeightmapValid[slot] = false;
                    break;
                }
            }
        }
}
void WorldGridQuadtreeHeightmapManager::compactReferences()
{
    std::vector<std::uint16_t> slots;
    for (std::uint16_t i = 0; i < kCapacity; ++i)
        if (m_heightmaps.isOpen(i) && m_finalMetadata[i].referenceCount)
            slots.push_back(i);
    for (std::size_t i = 1; i < slots.size(); ++i)
    {
        auto v = slots[i];
        std::size_t j = i;
        while (j && m_finalMetadata[slots[j - 1]].referenceFront > m_finalMetadata[v].referenceFront)
        {
            slots[j] = slots[j - 1];
            --j;
        }
        slots[j] = v;
    }
    std::uint32_t write = 0;
    for (auto slot : slots)
    {
        auto &m = m_finalMetadata[slot];
        if (m.referenceFront != write)
            std::move(m_sourceReferences.begin() + m.referenceFront, m_sourceReferences.begin() + m.referenceFront + m.referenceCount,
                      m_sourceReferences.begin() + write);
        m.referenceFront = write;
        write += m.referenceCount;
    }
    m_sourceReferences.resize(write);
}

bool WorldGridQuadtreeHeightmapManager::makeCpuResident(const WorldGridQuadtreeLeafId &id, QuadtreeMeshRenderer &renderer)
{
    auto slot = requestAsset(id);
    if (slot == kUnavailable)
        return false;
    if (m_cpuHeightmapValid[slot] && m_cpuHeightmapLeafIds[slot] == id)
        return true;
    if (!m_cpuHeightmapPending[slot])
        m_cpuHeightmapPending[slot] = renderer.requestHeightmapSliceDownload(id, slot);
    return false;
}
void WorldGridQuadtreeHeightmapManager::requestLeaf(const WorldGridQuadtreeLeafId &id, QuadtreeMeshRenderer &renderer)
{
    auto slot = requestAsset(id);
    if (slot != kUnavailable)
        renderer.addLeaf(id, slot);
}
bool WorldGridQuadtreeHeightmapManager::getExtents(const WorldGridQuadtreeLeafId &id, HeightmapExtents &e) const
{
    auto s = findSlot(id);
    if (!s || !m_heightmaps.isReady(*s) || !m_knownExtentsValid[*s])
        return false;
    e = m_knownExtents[*s];
    return true;
}
bool WorldGridQuadtreeHeightmapManager::getResidentSliceIndex(const WorldGridQuadtreeLeafId &id, std::uint16_t &s) const
{
    auto v = findSlot(id);
    if (!v || !m_heightmaps.isReady(*v))
        return false;
    s = *v;
    return true;
}
bool WorldGridQuadtreeHeightmapManager::tryGetCpuResidentHeightmap(const WorldGridQuadtreeLeafId &id, CpuResidentHeightmapView &v) const
{
    std::uint16_t s;
    if (!getResidentSliceIndex(id, s) || !m_cpuHeightmapValid[s] || m_cpuHeightmapLeafIds[s] != id)
        return false;
    v = {id, s, m_cpuHeightmapSamples[s]};
    return true;
}
void WorldGridQuadtreeHeightmapManager::collectCompletedCpuReadbacks(QuadtreeMeshRenderer &r)
{
    std::vector<QuadtreeMeshRenderer::CompletedHeightmapSliceReadback> done;
    r.collectCompletedHeightmapSliceReadbacks(done);
    for (auto &x : done)
    {
        auto s = findSlot(x.leafId);
        if (s && *s == x.sliceIndex && m_heightmaps.isReady(*s))
        {
            m_cpuHeightmapSamples[*s].assign(x.samples.begin(), x.samples.end());
            m_cpuHeightmapLeafIds[*s] = x.leafId;
            m_cpuHeightmapValid[*s] = true;
            m_cpuHeightmapPending[*s] = false;
        }
    }
}
void WorldGridQuadtreeHeightmapManager::applyGeneratedExtents(const WorldGridQuadtreeLeafId &id, std::uint16_t slot,
                                                              const HeightmapExtents &e, GenerationJobHandle job)
{
    if (!m_generationJobs.contains(job) || !m_generationJobs.isSubmitted(job) || !m_generationJobs.fence(job) ||
        !m_generationJobs.fence(job)->isSignaled())
        return;
    auto current = findSlot(id);
    if (!m_generationJobs.isDiscarded(job) && current && *current == slot && m_heightmaps.activeJob(slot) == job)
    {
        m_knownExtents[slot] = e;
        m_knownExtentsValid[slot] = true;
        m_heightmaps.markReady(slot);
        m_heightmaps.clearActiveJob(slot, job);
        ++m_stats.completedFinalGenerations;
        if (m_finalMetadata[slot].referenceCount > 0)
            ++m_stats.completedFinalGenerationsWithSources;
    }
    m_generationJobs.markCompleted(job);
    m_generationJobs.retireCompletedFront();
}
void WorldGridQuadtreeHeightmapManager::invalidateSlotMetadata(std::uint16_t s)
{
    m_finalMetadata[s] = {};
    m_knownExtents[s] = {};
    m_knownExtentsValid[s] = false;
    m_cpuHeightmapSamples[s].clear();
    m_cpuHeightmapValid[s] = m_cpuHeightmapPending[s] = false;
}
void WorldGridQuadtreeHeightmapManager::clearCache()
{
    m_heightmaps.clear();
    m_generationJobs.discardAll();
    m_sourceReferences.clear();
    m_residentCount = 0;
    for (std::uint16_t i = 0; i < kCapacity; ++i)
        invalidateSlotMetadata(i);
}
void WorldGridQuadtreeHeightmapManager::shutdownAfterGpuIdle()
{
    m_heightmaps.clear();
    m_sourceTiles.clear();
    m_generationJobs.clear();
    for (auto &upload : m_sourceUploads)
    {
        if (upload.load.valid())
            upload.load.wait();
        upload.samples.clear();
        upload.fence.reset();
        upload.queuedToRenderer = false;
    }
    for (auto &fences : m_sourceReadFences)
        fences.clear();
}
void WorldGridQuadtreeHeightmapManager::setComputeDispatchBudget(std::uint16_t b)
{
    m_computeDispatchBudget = std::max<std::uint16_t>(1, b);
}
WorldGridQuadtreeHeightmapManager::Diagnostics WorldGridQuadtreeHeightmapManager::diagnostics() const
{
    auto d = m_stats;
    d.submittedJobs = 0;
    d.queuedGenerationJobs = 0;
    d.referenceCount = static_cast<std::uint32_t>(m_sourceReferences.size());
    d.referenceCapacity = static_cast<std::uint32_t>(m_sourceReferences.capacity());
    d.sourceHashOccupied = static_cast<std::uint32_t>(m_sourceTiles.hashOccupiedCount());
    d.sourceHashCapacity = static_cast<std::uint32_t>(m_sourceTiles.hashTableSize());
    d.sourceHashCollisions = static_cast<std::uint32_t>(m_sourceTiles.hashCollisionCount());
    for (std::uint16_t i = 0; i < kSourceTileCapacity; ++i)
        if (m_sourceTiles.isOpen(i))
        {
            ++d.sourceOccupied;
            if (m_sourceTiles.isReady(i))
                ++d.sourceReady;
            else
                ++d.sourceLoading;
            if (m_sourceTiles.ageOf(i) == 0)
                ++d.sourceAgeZero;
        }
    d.waitingFinals = 0;
    d.pendingContributions = 0;
    for (std::uint16_t i = 0; i < kCapacity; ++i)
        if (m_heightmaps.isOpen(i) && !m_heightmaps.isReady(i))
        {
            ++d.waitingFinals;
            d.pendingContributions += m_finalMetadata[i].referenceCount;
        }
    m_generationJobs.forEachActive([&](GenerationJobHandle job) {
        if (m_generationJobs.isSubmitted(job) && !m_generationJobs.isCompleted(job))
            ++d.submittedJobs;
        else if (!m_generationJobs.isDiscarded(job) && !m_generationJobs.isCompleted(job))
            ++d.queuedGenerationJobs;
    });
    return d;
}
