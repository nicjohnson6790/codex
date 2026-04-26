#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

struct CapturedScope
{
    std::uint64_t start = 0;
    std::uint64_t stop = 0;
    const char* name = nullptr;
};

struct CapturedFrame
{
    std::uint64_t start = 0;
    std::uint64_t stop = 0;
    std::size_t scopeBeginIndex = 0;
    std::size_t scopeEndIndex = 0;
};

class PerformanceCapture
{
public:
    class ScopedEvent
    {
    public:
        explicit ScopedEvent(const char* name);
        ~ScopedEvent();

        ScopedEvent(const ScopedEvent&) = delete;
        ScopedEvent& operator=(const ScopedEvent&) = delete;

    private:
        std::size_t m_scopeIndex = static_cast<std::size_t>(-1);
    };

    static PerformanceCapture& instance();
    static std::uint64_t readTimestamp();

    void initialize(double historySeconds = 3.0);
    void shutdown();

    void beginFrame();
    void endFrame();

    void setPaused(bool paused);
    [[nodiscard]] bool paused() const { return m_paused; }

    [[nodiscard]] const std::deque<CapturedFrame>& frames() const { return m_frames; }
    [[nodiscard]] const std::vector<CapturedScope>& scopes() const { return m_scopes; }
    [[nodiscard]] bool empty() const { return m_frames.empty(); }
    [[nodiscard]] std::size_t latestFrameIndex() const;

    [[nodiscard]] float cyclesToMilliseconds(std::uint64_t cycles) const;
    [[nodiscard]] std::uint64_t frameCycles(const CapturedFrame& frame) const;
    [[nodiscard]] std::uint64_t cpuCycles(const CapturedFrame& frame) const;

private:
    PerformanceCapture() = default;

    [[nodiscard]] std::size_t beginScope(const char* name);
    void endScope(std::size_t scopeIndex);
    void trimHistory();
    void calibrateCyclesPerSecond();
    [[nodiscard]] bool canCaptureScope() const;

    bool m_initialized = false;
    bool m_paused = false;
    bool m_frameOpen = false;
    double m_historySeconds = 3.0;
    double m_cyclesPerSecond = 3.0e9;
    std::uint64_t m_historyWindowCycles = 0;
    std::uint64_t m_frameStartCycles = 0;
    std::size_t m_frameScopeBeginIndex = 0;
    std::deque<CapturedFrame> m_frames;
    std::vector<CapturedScope> m_scopes;
};

#define HELLO_PROFILE_JOIN_IMPL(left, right) left##right
#define HELLO_PROFILE_JOIN(left, right) HELLO_PROFILE_JOIN_IMPL(left, right)
#define HELLO_PROFILE_SCOPE(name) [[maybe_unused]] PerformanceCapture::ScopedEvent HELLO_PROFILE_JOIN(hello_profile_scope_, __LINE__)(name)
