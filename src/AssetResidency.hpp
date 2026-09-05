#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

using CacheIndex = std::uint16_t;
inline constexpr CacheIndex kUnavailableCacheIndex = std::numeric_limits<CacheIndex>::max();

struct GenerationJobHandle
{
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool valid() const
    {
        return index != std::numeric_limits<std::uint32_t>::max();
    }

    friend constexpr bool operator==(const GenerationJobHandle &, const GenerationJobHandle &) = default;
};

namespace AssetResidencyDetail
{
class BitArray
{
  public:
    BitArray() = default;
    explicit BitArray(std::size_t size) : m_size(size), m_words((size + 63u) / 64u, 0)
    {
    }

    [[nodiscard]] bool test(std::size_t index) const
    {
        return (m_words.at(index / 64u) & (std::uint64_t{1} << (index % 64u))) != 0;
    }

    void set(std::size_t index, bool value = true)
    {
        std::uint64_t &word = m_words.at(index / 64u);
        const std::uint64_t mask = std::uint64_t{1} << (index % 64u);
        word = value ? (word | mask) : (word & ~mask);
    }

    void clear()
    {
        std::fill(m_words.begin(), m_words.end(), 0);
    }
    [[nodiscard]] std::size_t size() const
    {
        return m_size;
    }

  private:
    std::size_t m_size = 0;
    std::vector<std::uint64_t> m_words;
};
} // namespace AssetResidencyDetail

template <typename AssetId, typename SlotIndex = CacheIndex, typename Hash = std::hash<AssetId>, typename Equal = std::equal_to<AssetId>, typename Age = std::uint8_t>
class FixedAssetCache
{
  public:
    static constexpr SlotIndex kInvalidSlot = std::numeric_limits<SlotIndex>::max();

    FixedAssetCache(std::size_t capacity, std::size_t bucketCount, std::size_t entriesPerBucket, Hash hash = {}, Equal equal = {})
        : m_assetIds(capacity), m_open(capacity), m_ready(capacity), m_ages(capacity, 0), m_activeJobs(capacity),
          m_lookupBuckets(checkedBucketStorageSize(bucketCount, entriesPerBucket), kInvalidSlot), m_bucketHasOverflow(bucketCount),
          m_overflowEntries(capacity),
          m_bucketCount(bucketCount), m_entriesPerBucket(entriesPerBucket), m_hash(std::move(hash)), m_equal(std::move(equal))
    {
        if (capacity == 0 || capacity > static_cast<std::size_t>(kInvalidSlot))
            throw std::invalid_argument("FixedAssetCache requires non-zero representable capacity, hash "
                                        "bucket count, and bucket entry count.");
    }

    [[nodiscard]] std::size_t capacity() const
    {
        return m_assetIds.size();
    }
    [[nodiscard]] std::size_t hashTableSize() const
    {
        return m_lookupBuckets.size();
    }
    [[nodiscard]] std::size_t lookupDepth() const
    {
        return m_entriesPerBucket;
    }
    [[nodiscard]] std::size_t hashOccupiedCount() const
    {
        return static_cast<std::size_t>(
            std::count_if(m_lookupBuckets.begin(), m_lookupBuckets.end(), [](SlotIndex slot) { return slot != kInvalidSlot; }));
    }
    [[nodiscard]] std::size_t hashCollisionCount() const
    {
        return m_overflowCount;
    }

    [[nodiscard]] std::optional<SlotIndex> find(const AssetId &id, SlotIndex hint = kInvalidSlot) const
    {
        if (hint != kInvalidSlot && validatesHint(hint, id))
            return hint;
        const std::size_t bucketIndex = m_hash(id) % m_bucketCount;
        const std::size_t bucketStart = bucketIndex * m_entriesPerBucket;
        for (std::size_t entryIndex = 0; entryIndex < m_entriesPerBucket; ++entryIndex)
        {
            const SlotIndex slot = m_lookupBuckets[bucketStart + entryIndex];
            if (slot != kInvalidSlot && isOpen(slot) && m_equal(m_assetIds[slot], id))
                return slot;
        }
        if (!m_bucketHasOverflow.test(bucketIndex))
            return std::nullopt;
        for (const OverflowEntry &entry : m_overflowEntries)
        {
            if (entry.used && entry.bucketIndex == bucketIndex && m_equal(entry.assetId, id))
                return entry.slot;
        }
        return std::nullopt;
    }

    // Read-only, ready-only lookup. Never touches age or admits/generates assets.
    [[nodiscard]] SlotIndex isResident(const AssetId &id, SlotIndex hint = kInvalidSlot) const
    {
        const auto slot = find(id, hint);
        return slot && isReady(*slot) ? *slot : kInvalidSlot;
    }

    [[nodiscard]] bool validatesHint(SlotIndex slot, const AssetId &id) const
    {
        return validSlot(slot) && isOpen(slot) && m_equal(m_assetIds[slot], id);
    }

    [[nodiscard]] std::optional<SlotIndex> findAllocationCandidate() const
    {
        for (std::size_t index = 0; index < capacity(); ++index)
            if (!m_open.test(index))
                return static_cast<SlotIndex>(index);

        std::optional<SlotIndex> oldest;
        for (std::size_t index = 0; index < capacity(); ++index)
        {
            if (m_ages[index] == 0)
                continue;
            if (!oldest || m_ages[index] > m_ages[*oldest])
                oldest = static_cast<SlotIndex>(index);
        }
        return oldest;
    }

    void assign(SlotIndex slot, const AssetId &id)
    {
        requireSlot(slot);
        removeLookup(slot);
        m_assetIds[slot] = id;
        m_open.set(slot);
        m_ready.set(slot, false);
        m_ages[slot] = 0;
        m_activeJobs[slot] = {};
        insertLookup(slot);
    }

    void touch(SlotIndex slot)
    {
        requireOpen(slot);
        m_ages[slot] = 0;
    }

    void age(Age elapsed = 1)
    {
        for (std::size_t index = 0; index < capacity(); ++index)
            if (m_open.test(index))
                m_ages[index] += std::min(elapsed, static_cast<Age>(std::numeric_limits<Age>::max() - m_ages[index]));
    }

    [[nodiscard]] bool isOpen(SlotIndex slot) const
    {
        return validSlot(slot) && m_open.test(slot);
    }
    [[nodiscard]] bool isReady(SlotIndex slot) const
    {
        return validSlot(slot) && m_ready.test(slot);
    }
    [[nodiscard]] Age ageOf(SlotIndex slot) const
    {
        requireOpen(slot);
        return m_ages[slot];
    }
    [[nodiscard]] const AssetId &assetId(SlotIndex slot) const
    {
        requireOpen(slot);
        return m_assetIds[slot];
    }
    [[nodiscard]] GenerationJobHandle activeJob(SlotIndex slot) const
    {
        requireOpen(slot);
        return m_activeJobs[slot];
    }
    void setActiveJob(SlotIndex slot, GenerationJobHandle job)
    {
        requireOpen(slot);
        m_activeJobs[slot] = job;
    }
    void clearActiveJob(SlotIndex slot, GenerationJobHandle expected)
    {
        requireOpen(slot);
        if (m_activeJobs[slot] == expected)
            m_activeJobs[slot] = {};
    }
    void markReady(SlotIndex slot)
    {
        requireOpen(slot);
        m_ready.set(slot);
    }
    void markNotReady(SlotIndex slot)
    {
        requireOpen(slot);
        m_ready.set(slot, false);
    }

    void release(SlotIndex slot)
    {
        requireSlot(slot);
        if (!m_open.test(slot))
            return;
        removeLookup(slot);
        m_open.set(slot, false);
        m_ready.set(slot, false);
        m_ages[slot] = 0;
        m_activeJobs[slot] = {};
    }

    void clear()
    {
        m_open.clear();
        m_ready.clear();
        std::fill(m_ages.begin(), m_ages.end(), 0);
        std::fill(m_activeJobs.begin(), m_activeJobs.end(), GenerationJobHandle{});
        std::fill(m_lookupBuckets.begin(), m_lookupBuckets.end(), kInvalidSlot);
        m_bucketHasOverflow.clear();
        std::fill(m_overflowEntries.begin(), m_overflowEntries.end(), OverflowEntry{});
        m_overflowCount = 0;
    }

  private:
    [[nodiscard]] static std::size_t checkedBucketStorageSize(std::size_t bucketCount, std::size_t entriesPerBucket)
    {
        if (bucketCount == 0 || entriesPerBucket == 0 ||
            bucketCount > std::numeric_limits<std::size_t>::max() / entriesPerBucket)
            throw std::invalid_argument("FixedAssetCache bucket storage size is invalid");
        return bucketCount * entriesPerBucket;
    }

    [[nodiscard]] bool validSlot(SlotIndex slot) const
    {
        return static_cast<std::size_t>(slot) < capacity();
    }
    void requireSlot(SlotIndex slot) const
    {
        if (!validSlot(slot))
            throw std::out_of_range("cache slot");
    }
    void requireOpen(SlotIndex slot) const
    {
        if (!isOpen(slot))
            throw std::out_of_range("open cache slot");
    }

    struct OverflowEntry
    {
        AssetId assetId{};
        SlotIndex slot = kInvalidSlot;
        std::size_t bucketIndex = 0;
        bool used = false;
    };

    void insertLookup(SlotIndex slot)
    {
        const std::size_t bucketIndex = m_hash(m_assetIds[slot]) % m_bucketCount;
        const std::size_t bucketStart = bucketIndex * m_entriesPerBucket;
        for (std::size_t entryIndex = 0; entryIndex < m_entriesPerBucket; ++entryIndex)
        {
            SlotIndex &candidate = m_lookupBuckets[bucketStart + entryIndex];
            if (candidate == kInvalidSlot)
            {
                candidate = slot;
                return;
            }
        }
        for (OverflowEntry &entry : m_overflowEntries)
        {
            if (entry.used)
                continue;
            entry = {.assetId = m_assetIds[slot], .slot = slot, .bucketIndex = bucketIndex, .used = true};
            m_bucketHasOverflow.set(bucketIndex);
            ++m_overflowCount;
            return;
        }
        throw std::logic_error("FixedAssetCache lookup overflow exhausted");
    }

    void removeLookup(SlotIndex slot)
    {
        const std::size_t bucketIndex = m_hash(m_assetIds[slot]) % m_bucketCount;
        const std::size_t bucketStart = bucketIndex * m_entriesPerBucket;
        for (std::size_t entryIndex = 0; entryIndex < m_entriesPerBucket; ++entryIndex)
        {
            SlotIndex &candidate = m_lookupBuckets[bucketStart + entryIndex];
            if (candidate != slot)
                continue;
            candidate = kInvalidSlot;
            for (OverflowEntry &entry : m_overflowEntries)
            {
                if (!entry.used || entry.bucketIndex != bucketIndex)
                    continue;
                candidate = entry.slot;
                entry = {};
                --m_overflowCount;
                refreshBucketOverflowFlag(bucketIndex);
                return;
            }
            refreshBucketOverflowFlag(bucketIndex);
            return;
        }
        for (OverflowEntry &entry : m_overflowEntries)
        {
            if (!entry.used || entry.bucketIndex != bucketIndex || entry.slot != slot)
                continue;
            entry = {};
            --m_overflowCount;
            refreshBucketOverflowFlag(bucketIndex);
            return;
        }
    }

    void refreshBucketOverflowFlag(std::size_t bucketIndex)
    {
        const bool hasOverflow = std::any_of(m_overflowEntries.begin(), m_overflowEntries.end(),
                                             [bucketIndex](const OverflowEntry &entry) {
                                                 return entry.used && entry.bucketIndex == bucketIndex;
                                             });
        m_bucketHasOverflow.set(bucketIndex, hasOverflow);
    }

    std::vector<AssetId> m_assetIds;
    AssetResidencyDetail::BitArray m_open;
    AssetResidencyDetail::BitArray m_ready;
    std::vector<Age> m_ages;
    std::vector<GenerationJobHandle> m_activeJobs;
    std::vector<SlotIndex> m_lookupBuckets;
    AssetResidencyDetail::BitArray m_bucketHasOverflow;
    std::vector<OverflowEntry> m_overflowEntries;
    std::size_t m_bucketCount;
    std::size_t m_entriesPerBucket;
    std::size_t m_overflowCount = 0;
    Hash m_hash;
    Equal m_equal;
};

template <typename Job, typename Fence> class GenerationQueue
{
  public:
    explicit GenerationQueue(std::size_t capacity)
        : m_jobs(capacity), m_discarded(capacity), m_completed(capacity), m_submitted(capacity), m_fences(capacity),
          m_generations(capacity, 0)
    {
        if (capacity == 0 || capacity > std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument("GenerationQueue requires non-zero representable capacity.");
    }

    [[nodiscard]] std::size_t capacity() const
    {
        return m_jobs.size();
    }
    [[nodiscard]] std::size_t count() const
    {
        return m_count;
    }
    [[nodiscard]] bool full() const
    {
        return m_count == capacity();
    }

    [[nodiscard]] std::optional<GenerationJobHandle> tryPush(Job job)
    {
        if (full())
            return std::nullopt;
        const std::size_t index = (m_front + m_count) % capacity();
        m_jobs[index] = std::move(job);
        m_discarded.set(index, false);
        m_completed.set(index, false);
        m_submitted.set(index, false);
        m_fences[index].reset();
        ++m_generations[index];
        if (m_generations[index] == 0)
            ++m_generations[index];
        ++m_count;
        return GenerationJobHandle{static_cast<std::uint32_t>(index), m_generations[index]};
    }

    [[nodiscard]] bool contains(GenerationJobHandle handle) const
    {
        return handle.valid() && handle.index < capacity() && m_generations[handle.index] == handle.generation && isAllocated(handle.index);
    }

    [[nodiscard]] Job &job(GenerationJobHandle handle)
    {
        require(handle);
        return m_jobs[handle.index];
    }
    [[nodiscard]] const Job &job(GenerationJobHandle handle) const
    {
        require(handle);
        return m_jobs[handle.index];
    }
    [[nodiscard]] bool isDiscarded(GenerationJobHandle handle) const
    {
        require(handle);
        return m_discarded.test(handle.index);
    }
    [[nodiscard]] bool isCompleted(GenerationJobHandle handle) const
    {
        require(handle);
        return m_completed.test(handle.index);
    }
    [[nodiscard]] bool isSubmitted(GenerationJobHandle handle) const
    {
        require(handle);
        return m_submitted.test(handle.index);
    }
    [[nodiscard]] const std::shared_ptr<Fence> &fence(GenerationJobHandle handle) const
    {
        require(handle);
        return m_fences[handle.index];
    }

    void markSubmitted(GenerationJobHandle handle, std::shared_ptr<Fence> fence)
    {
        require(handle);
        m_submitted.set(handle.index);
        m_fences[handle.index] = std::move(fence);
    }

    void discard(GenerationJobHandle handle)
    {
        require(handle);
        m_discarded.set(handle.index);
        if (!m_submitted.test(handle.index))
            m_completed.set(handle.index);
    }

    void markCompleted(GenerationJobHandle handle)
    {
        require(handle);
        m_completed.set(handle.index);
    }

    void discardAll()
    {
        forEachActive([&](GenerationJobHandle handle) { discard(handle); });
        retireCompletedFront();
    }

    void retireSignaledDiscarded()
    {
        forEachActive([&](GenerationJobHandle handle) {
            if (!isDiscarded(handle) || isCompleted(handle) || !isSubmitted(handle))
                return;
            const std::shared_ptr<Fence> &submittedFence = fence(handle);
            if (submittedFence && submittedFence->isSignaled())
                markCompleted(handle);
        });
        retireCompletedFront();
    }

    template <typename CompletionFunction> void processSignaled(CompletionFunction &&processCompletion)
    {
        forEachActive([&](GenerationJobHandle handle) {
            if (isCompleted(handle) || !isSubmitted(handle))
                return;
            const std::shared_ptr<Fence> &submittedFence = fence(handle);
            if (!submittedFence || !submittedFence->isSignaled())
                return;
            if (!isDiscarded(handle))
                std::invoke(processCompletion, handle, job(handle));
            markCompleted(handle);
        });
        retireCompletedFront();
    }

    template <typename Function> void forEachActive(Function &&function)
    {
        for (std::size_t offset = 0; offset < m_count; ++offset)
        {
            const std::size_t index = (m_front + offset) % capacity();
            std::invoke(function, GenerationJobHandle{static_cast<std::uint32_t>(index), m_generations[index]});
        }
    }

    template <typename Function> void forEachActive(Function &&function) const
    {
        for (std::size_t offset = 0; offset < m_count; ++offset)
        {
            const std::size_t index = (m_front + offset) % capacity();
            std::invoke(function, GenerationJobHandle{static_cast<std::uint32_t>(index), m_generations[index]});
        }
    }

    std::size_t retireCompletedFront()
    {
        std::size_t retired = 0;
        while (m_count != 0 && m_completed.test(m_front))
        {
            m_fences[m_front].reset();
            m_front = (m_front + 1) % capacity();
            --m_count;
            ++retired;
        }
        return retired;
    }

    void clear()
    {
        m_front = 0;
        m_count = 0;
        m_discarded.clear();
        m_completed.clear();
        m_submitted.clear();
        std::fill(m_fences.begin(), m_fences.end(), nullptr);
    }

  private:
    [[nodiscard]] bool isAllocated(std::size_t index) const
    {
        for (std::size_t offset = 0; offset < m_count; ++offset)
            if ((m_front + offset) % capacity() == index)
                return true;
        return false;
    }
    void require(GenerationJobHandle handle) const
    {
        if (!contains(handle))
            throw std::out_of_range("generation job handle");
    }

    std::vector<Job> m_jobs;
    AssetResidencyDetail::BitArray m_discarded;
    AssetResidencyDetail::BitArray m_completed;
    AssetResidencyDetail::BitArray m_submitted;
    std::vector<std::shared_ptr<Fence>> m_fences;
    std::vector<std::uint32_t> m_generations;
    std::size_t m_front = 0;
    std::size_t m_count = 0;
};
