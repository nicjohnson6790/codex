#include "AssetResidency.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <memory>

namespace
{
struct ConstantHash
{
    std::size_t operator()(int) const
    {
        return 0;
    }
};
struct Fence
{
    bool signaled = false;
    [[nodiscard]] bool isSignaled() const
    {
        return signaled;
    }
};
struct Job
{
    int assetId = 0;
    std::uint16_t targetSlot = 0;
};

struct TestManager
{
    static constexpr std::uint16_t unavailable = 2;
    FixedAssetCache<int> cache{2, 2, 1};
    GenerationQueue<Job, Fence> jobs{1};

    std::uint16_t request(int assetId, std::uint16_t hint = unavailable)
    {
        auto slot = hint != unavailable && cache.validatesHint(hint, assetId) ? std::optional<std::uint16_t>(hint) : cache.find(assetId);
        if (slot)
        {
            cache.touch(*slot);
            return cache.isReady(*slot) ? *slot : unavailable;
        }
        const auto candidate = cache.findAllocationCandidate();
        if (!candidate)
            return unavailable;
        const auto job = jobs.tryPush({assetId, *candidate});
        if (!job)
            return unavailable;
        cache.assign(*candidate, assetId);
        cache.setActiveJob(*candidate, *job);
        return unavailable;
    }
};

struct MultiCacheManager
{
    FixedAssetCache<int> generated{2, 2, 1};
    FixedAssetCache<int> references{3, 4, 2};
    GenerationQueue<Job, Fence> generatedJobs{2};
    GenerationQueue<Job, Fence> referenceJobs{3};
};
} // namespace

int main()
{
    FixedAssetCache<int, std::uint16_t, ConstantHash> cache(3, 1, 1);
    cache.assign(0, 10);
    cache.markReady(0);
    cache.assign(1, 20);
    cache.markReady(1);
    assert(cache.find(20) == 1); // Full-cache fallback after the forced collision.
    assert(cache.hashTableSize() == 1 && cache.lookupDepth() == 1);
    assert(cache.hashOccupiedCount() == 1 && cache.hashCollisionCount() == 1);
    assert(cache.validatesHint(0, 10));
    assert(!cache.validatesHint(0, 20));
    for (int i = 0; i < 300; ++i)
        cache.age();
    assert(cache.ageOf(0) == 255);
    cache.touch(0);
    assert(cache.ageOf(0) == 0);
    cache.markNotReady(0);
    assert(!cache.isReady(0));
    cache.markReady(0);

    GenerationQueue<Job, Fence> queue(3);
    auto a = queue.tryPush({10, 0}).value();
    auto b = queue.tryPush({20, 1}).value();
    auto c = queue.tryPush({30, 2}).value();
    assert(!queue.tryPush({40, 0}));
    const auto untouchedCandidate = cache.findAllocationCandidate();
    assert(untouchedCandidate == 2 && !cache.isOpen(2)); // Failed queue admission cannot reserve the cache slot.
    cache.setActiveJob(0, a);
    cache.clearActiveJob(0, b);
    assert(cache.activeJob(0) == a);
    cache.clearActiveJob(0, a);
    assert(!cache.activeJob(0).valid());

    auto fenceA = std::make_shared<Fence>();
    auto fenceB = std::make_shared<Fence>();
    queue.markSubmitted(a, fenceA);
    queue.markSubmitted(b, fenceB);
    queue.markSubmitted(c, fenceB);
    assert(queue.fence(b) == queue.fence(c));
    int completionCalls = 0;
    fenceB->signaled = true;
    queue.processSignaled([&](GenerationJobHandle, const Job &) { ++completionCalls; });
    assert(completionCalls == 2);
    assert(queue.retireCompletedFront() == 0);
    queue.processSignaled([&](GenerationJobHandle, const Job &) { ++completionCalls; });
    assert(completionCalls == 2); // Completed jobs are not processed twice.
    fenceA->signaled = true;
    queue.processSignaled([&](GenerationJobHandle, const Job &) { ++completionCalls; });
    assert(completionCalls == 3 && queue.count() == 0);

    auto d = queue.tryPush({40, 0}).value();
    queue.discard(d);
    assert(queue.isDiscarded(d) && queue.isCompleted(d));
    assert(queue.retireCompletedFront() == 1);

    auto e = queue.tryPush({50, 0}).value();
    queue.markSubmitted(e, fenceA);
    queue.discard(e);
    assert(queue.isDiscarded(e) && !queue.isCompleted(e));
    queue.processSignaled([&](GenerationJobHandle, const Job &) { ++completionCalls; });
    assert(completionCalls == 3 && queue.count() == 0); // Discarded completion never calls back.

    FixedAssetCache<int> secondCache(2, 2, 1);
    secondCache.assign(0, 99);
    assert(secondCache.find(99) == 0 && !cache.find(99));

    FixedAssetCache<int> reassignedCache(1, 1, 1);
    GenerationQueue<Job, Fence> reassignedQueue(2);
    auto oldJob = reassignedQueue.tryPush({1, 0}).value();
    reassignedCache.assign(0, 1);
    reassignedCache.setActiveJob(0, oldJob);
    reassignedQueue.markSubmitted(oldJob, fenceA);
    reassignedCache.age();
    auto newJob = reassignedQueue.tryPush({2, 0}).value();
    reassignedQueue.discard(oldJob);
    reassignedCache.clearActiveJob(0, oldJob);
    reassignedCache.assign(0, 2);
    reassignedCache.setActiveJob(0, newJob);
    int staleCompletionCalls = 0;
    reassignedQueue.processSignaled([&](GenerationJobHandle, const Job &) { ++staleCompletionCalls; });
    assert(staleCompletionCalls == 0);
    assert(reassignedCache.assetId(0) == 2 && reassignedCache.activeJob(0) == newJob);

    TestManager manager;
    assert(manager.request(7) == TestManager::unavailable);
    const std::size_t pendingCount = manager.jobs.count();
    assert(manager.request(7) == TestManager::unavailable);
    assert(manager.jobs.count() == pendingCount); // A duplicate pending request does not enqueue again.
    assert(manager.cache.validatesHint(0, 7));
    assert(manager.request(8, 0) == TestManager::unavailable); // Stale hint falls back to lookup/admission.
    assert(!manager.cache.find(8));                            // Queue-full admission leaves the cache unchanged.

    GenerationQueue<Job, Fence> wrapQueue(2);
    auto wrapA = wrapQueue.tryPush({1, 0}).value();
    auto wrapB = wrapQueue.tryPush({2, 1}).value();
    wrapQueue.markCompleted(wrapA);
    wrapQueue.markCompleted(wrapB);
    assert(wrapQueue.retireCompletedFront() == 2);
    auto wrapped = wrapQueue.tryPush({3, 0}).value();
    assert(wrapped.index == wrapA.index && wrapped.generation != wrapA.generation);

    GenerationQueue<Job, Fence> clearQueue(2);
    auto clearA = clearQueue.tryPush({1, 0}).value();
    auto clearB = clearQueue.tryPush({2, 1}).value();
    auto clearFence = std::make_shared<Fence>();
    clearQueue.markSubmitted(clearA, clearFence);
    clearQueue.discardAll();
    assert(clearQueue.count() == 2); // Submitted front remains; unsubmitted work
                                     // is semantically complete behind it.
    clearFence->signaled = true;
    clearQueue.retireSignaledDiscarded();
    assert(clearQueue.count() == 0);

    MultiCacheManager multi;
    multi.generated.assign(0, 11);
    multi.references.assign(0, 22);
    auto generatedJob = multi.generatedJobs.tryPush({11, 0}).value();
    auto referenceJob = multi.referenceJobs.tryPush({22, 0}).value();
    multi.generated.setActiveJob(0, generatedJob);
    multi.references.setActiveJob(0, referenceJob);
    assert(multi.generated.find(11) == 0 && !multi.generated.find(22));
    assert(multi.references.find(22) == 0 && !multi.references.find(11));
    assert(multi.generatedJobs.count() == 1 && multi.referenceJobs.count() == 1);
}
