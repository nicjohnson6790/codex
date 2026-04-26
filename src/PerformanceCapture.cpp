#include "PerformanceCapture.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>
#include <immintrin.h>
#include <intrin.h>
#include <limits>

namespace
{
constexpr std::uint64_t kInvalidScopeIndex = std::numeric_limits<std::size_t>::max();

bool isExcludedCpuScope(const CapturedScope& scope)
{
    return scope.name != nullptr &&
        std::strcmp(scope.name, "SDLRenderer::AcquireSwapchain") == 0;
}

template <typename Predicate>
std::uint64_t mergedScopeCycles(
    const std::vector<CapturedScope>& scopes,
    const CapturedFrame& frame,
    Predicate&& predicate)
{
    if (frame.scopeEndIndex <= frame.scopeBeginIndex || frame.scopeEndIndex > scopes.size())
    {
        return 0;
    }

    std::uint64_t mergedCycles = 0;
    std::uint64_t rangeStart = 0;
    std::uint64_t rangeStop = 0;
    bool hasRange = false;

    for (std::size_t scopeIndex = frame.scopeBeginIndex; scopeIndex < frame.scopeEndIndex; ++scopeIndex)
    {
        const CapturedScope& scope = scopes[scopeIndex];
        if (scope.stop <= scope.start || !predicate(scope))
        {
            continue;
        }

        if (!hasRange)
        {
            rangeStart = scope.start;
            rangeStop = scope.stop;
            hasRange = true;
            continue;
        }

        if (scope.start <= rangeStop)
        {
            rangeStop = std::max(rangeStop, scope.stop);
        }
        else
        {
            mergedCycles += (rangeStop - rangeStart);
            rangeStart = scope.start;
            rangeStop = scope.stop;
        }
    }

    if (hasRange)
    {
        mergedCycles += (rangeStop - rangeStart);
    }

    return mergedCycles;
}
}

PerformanceCapture::ScopedEvent::ScopedEvent(const char* name)
{
    m_scopeIndex = PerformanceCapture::instance().beginScope(name);
}

PerformanceCapture::ScopedEvent::~ScopedEvent()
{
    if (m_scopeIndex != kInvalidScopeIndex)
    {
        PerformanceCapture::instance().endScope(m_scopeIndex);
    }
}

PerformanceCapture& PerformanceCapture::instance()
{
    static PerformanceCapture capture;
    return capture;
}

std::uint64_t PerformanceCapture::readTimestamp()
{
    _mm_lfence();
    const std::uint64_t timestamp = __rdtsc();
    _mm_lfence();
    return timestamp;
}

void PerformanceCapture::initialize(double historySeconds)
{
    m_historySeconds = historySeconds;
    m_frames.clear();
    m_scopes.clear();
    m_scopes.reserve(16'384);
    calibrateCyclesPerSecond();
    m_historyWindowCycles = static_cast<std::uint64_t>(m_cyclesPerSecond * m_historySeconds);
    m_initialized = true;
    m_paused = false;
    m_frameOpen = false;
}

void PerformanceCapture::shutdown()
{
    m_initialized = false;
    m_paused = false;
    m_frameOpen = false;
    m_frames.clear();
    m_scopes.clear();
}

void PerformanceCapture::beginFrame()
{
    if (!m_initialized || m_paused)
    {
        return;
    }

    m_frameOpen = true;
    m_frameStartCycles = readTimestamp();
    m_frameScopeBeginIndex = m_scopes.size();
}

void PerformanceCapture::endFrame()
{
    if (!m_initialized || m_paused || !m_frameOpen)
    {
        return;
    }

    const std::uint64_t frameStopCycles = readTimestamp();
    m_frames.push_back({
        .start = m_frameStartCycles,
        .stop = frameStopCycles,
        .scopeBeginIndex = m_frameScopeBeginIndex,
        .scopeEndIndex = m_scopes.size(),
    });

    m_frameOpen = false;
    trimHistory();
}

void PerformanceCapture::setPaused(bool paused)
{
    m_paused = paused;
    if (m_paused)
    {
        m_frameOpen = false;
    }
}

std::size_t PerformanceCapture::latestFrameIndex() const
{
    return m_frames.empty() ? 0 : (m_frames.size() - 1);
}

float PerformanceCapture::cyclesToMilliseconds(std::uint64_t cycles) const
{
    if (m_cyclesPerSecond <= 0.0)
    {
        return 0.0f;
    }

    return static_cast<float>((static_cast<double>(cycles) * 1000.0) / m_cyclesPerSecond);
}

std::uint64_t PerformanceCapture::frameCycles(const CapturedFrame& frame) const
{
    return frame.stop > frame.start ? (frame.stop - frame.start) : 0;
}

std::uint64_t PerformanceCapture::cpuCycles(const CapturedFrame& frame) const
{
    const std::uint64_t coveredCycles = mergedScopeCycles(
        m_scopes,
        frame,
        [](const CapturedScope&) { return true; });
    const std::uint64_t excludedCycles = mergedScopeCycles(
        m_scopes,
        frame,
        [](const CapturedScope& scope) { return isExcludedCpuScope(scope); });
    return excludedCycles >= coveredCycles ? 0 : (coveredCycles - excludedCycles);
}

std::size_t PerformanceCapture::beginScope(const char* name)
{
    if (!canCaptureScope())
    {
        return kInvalidScopeIndex;
    }

    m_scopes.push_back({
        .start = readTimestamp(),
        .stop = 0,
        .name = name,
    });
    return m_scopes.size() - 1;
}

void PerformanceCapture::endScope(std::size_t scopeIndex)
{
    if (scopeIndex >= m_scopes.size())
    {
        return;
    }

    m_scopes[scopeIndex].stop = readTimestamp();
}

void PerformanceCapture::trimHistory()
{
    if (m_frames.empty())
    {
        return;
    }

    const std::uint64_t newestFrameStop = m_frames.back().stop;
    while (!m_frames.empty() && newestFrameStop > m_frames.front().stop && (newestFrameStop - m_frames.front().stop) > m_historyWindowCycles)
    {
        m_frames.pop_front();
    }

    const std::size_t firstScopeToKeep = m_frames.empty() ? m_scopes.size() : m_frames.front().scopeBeginIndex;
    if (firstScopeToKeep == 0)
    {
        return;
    }

    m_scopes.erase(m_scopes.begin(), m_scopes.begin() + static_cast<std::ptrdiff_t>(firstScopeToKeep));
    for (CapturedFrame& frame : m_frames)
    {
        frame.scopeBeginIndex -= firstScopeToKeep;
        frame.scopeEndIndex -= firstScopeToKeep;
    }
}

void PerformanceCapture::calibrateCyclesPerSecond()
{
    const std::uint64_t qpcFrequency = SDL_GetPerformanceFrequency();
    if (qpcFrequency == 0)
    {
        m_cyclesPerSecond = 3.0e9;
        return;
    }

    const std::uint64_t qpcStart = SDL_GetPerformanceCounter();
    const std::uint64_t tscStart = readTimestamp();

    std::uint64_t qpcStop = qpcStart;
    const std::uint64_t targetTicks = std::max<std::uint64_t>(qpcFrequency / 20, 1);
    while ((qpcStop - qpcStart) < targetTicks)
    {
        qpcStop = SDL_GetPerformanceCounter();
    }

    const std::uint64_t tscStop = readTimestamp();
    const double elapsedSeconds = static_cast<double>(qpcStop - qpcStart) / static_cast<double>(qpcFrequency);
    if (elapsedSeconds <= 0.0)
    {
        m_cyclesPerSecond = 3.0e9;
        return;
    }

    m_cyclesPerSecond = static_cast<double>(tscStop - tscStart) / elapsedSeconds;
}

bool PerformanceCapture::canCaptureScope() const
{
    return m_initialized && !m_paused && m_frameOpen;
}
