#pragma once

#include <algorithm>
#include <atomic>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Offline heightmap tile scheduling only. Contexts live inside the fixed async
// tasks; completed payloads are written by the caller, never queued in memory.
namespace HeightmapTileWorkers
{
using Coordinate = std::pair<int, int>; // y, x: canonical index order

template<class MakeContext, class Convert>
bool Run(std::span<const Coordinate> tiles, const std::string& label,
         MakeContext makeContext, Convert convert, std::string* error)
{
    const auto count = std::min(tiles.size(), std::size_t(std::max(1u, std::thread::hardware_concurrency())));
    std::atomic_size_t next{0}, active{0}, peak{0};
    std::atomic_bool failed{false};
    std::vector<std::future<void>> workers;
    workers.reserve(count);
    std::exception_ptr failure;
    try
    {
        for (std::size_t worker = 0; worker < count; ++worker)
            workers.push_back(std::async(std::launch::async, [&, worker] {
                std::string context = label + " worker " + std::to_string(worker) + " initialization";
                try
                {
                    auto scratch = makeContext();
                    while (!failed.load(std::memory_order_relaxed))
                    {
                        const auto index = next.fetch_add(1, std::memory_order_relaxed);
                        if (index >= tiles.size()) break;
                        const auto [ty, tx] = tiles[index];
                        context = label + " tile (" + std::to_string(tx) + "," + std::to_string(ty) + ")";
                        const auto running = active.fetch_add(1) + 1;
                        auto maximum = peak.load();
                        while (maximum < running && !peak.compare_exchange_weak(maximum, running)) {}
                        convert(index, tx, ty, *scratch);
                        active.fetch_sub(1);
                    }
                }
                catch (const std::exception& ex)
                {
                    failed.store(true);
                    throw std::runtime_error(context + ": " + ex.what());
                }
                catch (...)
                {
                    failed.store(true);
                    throw std::runtime_error(context + ": unknown worker failure");
                }
            }));
    }
    catch (...) { failed.store(true); failure = std::current_exception(); }
    // Always join and get every future, including after launch or tile failure.
    for (auto& worker : workers)
        try { worker.get(); }
        catch (...) { if (!failure) failure = std::current_exception(); }
    if (failure)
    {
        try { std::rethrow_exception(failure); }
        catch (const std::exception& ex) { if (error) *error = label + ": " + ex.what(); }
        catch (...) { if (error) *error = label + ": unknown conversion failure"; }
        return false;
    }
    std::cout << label << ": " << count << " async workers, peak " << peak.load() << " concurrent tile conversions\n";
    return true;
}
}
