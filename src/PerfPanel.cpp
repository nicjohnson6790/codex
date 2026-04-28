#include "PerfPanel.hpp"

#include "AppConfig.hpp"
#include "PerformanceCapture.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <numeric>
#include <vector>

namespace
{
ImU32 flameColorForDepth(int depth)
{
    const float hue = std::fmod(0.08f + (static_cast<float>(depth) * 0.13f), 1.0f);
    ImVec4 color = ImColor::HSV(hue, 0.62f, 0.86f);
    return ImGui::ColorConvertFloat4ToU32(color);
}

struct FlameGraphEntry
{
    std::size_t scopeIndex = 0;
    int depth = 0;
};

struct CycleRange
{
    std::uint64_t start = 0;
    std::uint64_t stop = 0;
};

bool isCollapsedWaitScope(const CapturedScope& scope)
{
    return (scope.groups & ProfileScopeGroup::Wait) != ProfileScopeGroup::None;
}

bool isImguiRelatedCompactScope(const CapturedScope& scope)
{
    constexpr ProfileScopeGroup kCompactGroups =
        ProfileScopeGroup::ImGui |
        ProfileScopeGroup::Renderer |
        ProfileScopeGroup::TreeUpdate;
    return (scope.groups & kCompactGroups) != ProfileScopeGroup::None;
}

std::vector<CycleRange> mergeRanges(std::vector<CycleRange> ranges)
{
    if (ranges.empty())
    {
        return ranges;
    }

    std::sort(ranges.begin(), ranges.end(), [](const CycleRange& left, const CycleRange& right) {
        return left.start < right.start;
    });

    std::vector<CycleRange> mergedRanges;
    mergedRanges.reserve(ranges.size());
    for (const CycleRange& range : ranges)
    {
        if (mergedRanges.empty() || range.start > mergedRanges.back().stop)
        {
            mergedRanges.push_back(range);
        }
        else
        {
            mergedRanges.back().stop = std::max(mergedRanges.back().stop, range.stop);
        }
    }

    return mergedRanges;
}

std::vector<CycleRange> buildVisibleRanges(
    std::uint64_t rangeStart,
    std::uint64_t rangeStop,
    const std::vector<CycleRange>& collapsedRanges)
{
    if (rangeStop <= rangeStart)
    {
        return {};
    }

    if (collapsedRanges.empty())
    {
        return { { rangeStart, rangeStop } };
    }

    std::vector<CycleRange> visibleRanges;
    visibleRanges.reserve(collapsedRanges.size() + 1);

    std::uint64_t cursor = rangeStart;
    for (const CycleRange& collapsedRange : collapsedRanges)
    {
        if (collapsedRange.start > cursor)
        {
            visibleRanges.push_back({ cursor, collapsedRange.start });
        }
        cursor = std::max(cursor, collapsedRange.stop);
        if (cursor >= rangeStop)
        {
            break;
        }
    }

    if (cursor < rangeStop)
    {
        visibleRanges.push_back({ cursor, rangeStop });
    }

    return visibleRanges;
}

std::uint64_t totalRangeCycles(const std::vector<CycleRange>& ranges)
{
    std::uint64_t total = 0;
    for (const CycleRange& range : ranges)
    {
        total += (range.stop - range.start);
    }
    return total;
}

float compactedFractionForTimestamp(
    std::uint64_t timestamp,
    const std::vector<CycleRange>& mergedRanges,
    std::uint64_t compactedCycles
)
{
    if (mergedRanges.empty() || compactedCycles == 0)
    {
        return 0.0f;
    }

    std::uint64_t elapsedCycles = 0;
    for (const CycleRange& range : mergedRanges)
    {
        if (timestamp <= range.start)
        {
            break;
        }

        if (timestamp >= range.stop)
        {
            elapsedCycles += (range.stop - range.start);
            continue;
        }

        elapsedCycles += (timestamp - range.start);
        break;
    }

    return static_cast<float>(elapsedCycles) / static_cast<float>(compactedCycles);
}

float percentageOrZero(float numerator, float denominator)
{
    if (denominator <= 0.0f)
    {
        return 0.0f;
    }

    return (numerator / denominator) * 100.0f;
}
}

void PerfPanel::draw(bool& viewportPaused)
{
    HELLO_PROFILE_SCOPE_GROUPS("PerfPanel::Draw", ProfileScopeGroup::ImGui);
    PerformanceCapture& performanceCapture = PerformanceCapture::instance();
    const std::deque<CapturedFrame>& frames = performanceCapture.frames();
    if (frames.empty())
    {
        ImGui::TextUnformatted("Collecting performance samples...");
        return;
    }

    m_selectedFrame = std::min(m_selectedFrame, frames.size() - 1);

    std::vector<float> frameSamples;
    std::vector<float> cpuSamples;
    std::vector<std::uint64_t> frameCycleSamples;
    std::vector<std::uint64_t> cpuCycleSamples;
    frameSamples.reserve(frames.size());
    cpuSamples.reserve(frames.size());
    frameCycleSamples.reserve(frames.size());
    cpuCycleSamples.reserve(frames.size());
    for (const CapturedFrame& frame : frames)
    {
        const std::uint64_t frameCycles = performanceCapture.frameCycles(frame);
        const std::uint64_t cpuCycles = performanceCapture.cpuCycles(frame);
        frameCycleSamples.push_back(frameCycles);
        cpuCycleSamples.push_back(cpuCycles);
        frameSamples.push_back(performanceCapture.cyclesToMilliseconds(frameCycles));
        cpuSamples.push_back(performanceCapture.cyclesToMilliseconds(cpuCycles));
    }

    const float latestMs = frameSamples.back();
    const float latestCpuMs = cpuSamples.back();
    const std::uint64_t latestCpuCycles = cpuCycleSamples.back();
    const float averageMs = std::accumulate(frameSamples.begin(), frameSamples.end(), 0.0f) / static_cast<float>(frameSamples.size());
    const float averageCpuMs = std::accumulate(cpuSamples.begin(), cpuSamples.end(), 0.0f) / static_cast<float>(cpuSamples.size());
    const float maxMs = *std::max_element(frameSamples.begin(), frameSamples.end());
    const float maxCpuMs = *std::max_element(cpuSamples.begin(), cpuSamples.end());
    const std::uint64_t maxCpuCycles = *std::max_element(cpuCycleSamples.begin(), cpuCycleSamples.end());
    const double averageCpuCycles = static_cast<double>(std::accumulate(
        cpuCycleSamples.begin(),
        cpuCycleSamples.end(),
        std::uint64_t{0})) / static_cast<double>(cpuCycleSamples.size());
    const float averageFps = averageMs > 0.0f ? (1000.0f / averageMs) : 0.0f;
    const float graphMaxMs = std::max(33.3f, maxMs * 1.1f);
    const float latestCpuPercent = percentageOrZero(latestCpuMs, latestMs);
    const float averageCpuPercent = percentageOrZero(averageCpuMs, averageMs);
    const float peakCpuPercent = percentageOrZero(maxCpuMs, maxMs);

    ImGui::Text("Window: last %.1f seconds", AppConfig::Perf::kHistorySeconds);
    ImGui::TextUnformatted("Latest CPU used");
    ImGui::Text("%.2f / %.2f ms (%.2f%%, %llu cycles)",
        latestCpuMs,
        latestMs,
        latestCpuPercent,
        static_cast<unsigned long long>(latestCpuCycles));
    ImGui::TextUnformatted("Average CPU used");
    ImGui::Text("%.2f / %.2f ms (%.2f%%, %.0f cycles, %.1f FPS)",
        averageCpuMs,
        averageMs,
        averageCpuPercent,
        averageCpuCycles,
        averageFps);
    ImGui::TextUnformatted("Peak CPU used");
    ImGui::Text("%.2f / %.2f ms (%.2f%%, %llu cycles)",
        maxCpuMs,
        maxMs,
        peakCpuPercent,
        static_cast<unsigned long long>(maxCpuCycles));
    ImGui::Text("Capture: %s", viewportPaused ? "paused" : "live");
    if (ImGui::Button(viewportPaused ? "Resume Viewport + Capture" : "Pause Viewport + Capture"))
    {
        viewportPaused = !viewportPaused;
        performanceCapture.setPaused(viewportPaused);
        if (viewportPaused && !performanceCapture.empty())
        {
            m_selectedFrame = performanceCapture.latestFrameIndex();
        }
        if (!viewportPaused)
        {
            m_flameGraphZoomActive = false;
            m_flameGraphZoomLabel.clear();
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted("Click a bar to freeze that frame and inspect its flame graph.");
    ImGui::Checkbox("Compact CPU timeline", &m_compactFlameGraph);
    ImGui::Checkbox("Compact ImGui-related blocks", &m_compactImguiRelatedBlocks);
    ImGui::Separator();

    const ImVec2 graphSize(-1.0f, AppConfig::Perf::kGraphHeight);
    const ImVec2 graphPos = ImGui::GetCursorScreenPos();
    const ImVec2 graphArea(
        ImGui::GetContentRegionAvail().x > 0.0f ? ImGui::GetContentRegionAvail().x : 1.0f,
        graphSize.y
    );

    ImGui::InvisibleButton("##PerfGraph", graphArea);
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 frameColor = IM_COL32(111, 168, 255, 220);
    const ImU32 cpuColor = IM_COL32(255, 170, 64, 235);
    const ImU32 outlineColor = IM_COL32(110, 120, 140, 255);
    const ImU32 gridColor = IM_COL32(70, 76, 89, 90);
    const ImU32 selectedColor = IM_COL32(255, 255, 255, 200);

    drawList->AddRectFilled(graphPos, ImVec2(graphPos.x + graphArea.x, graphPos.y + graphArea.y), IM_COL32(22, 25, 31, 255));
    drawList->AddRect(graphPos, ImVec2(graphPos.x + graphArea.x, graphPos.y + graphArea.y), outlineColor);

    for (int gridIndex = 1; gridIndex < 4; ++gridIndex)
    {
        const float y = graphPos.y + (graphArea.y * static_cast<float>(gridIndex) / 4.0f);
        drawList->AddLine(ImVec2(graphPos.x, y), ImVec2(graphPos.x + graphArea.x, y), gridColor);
    }

    const std::size_t sampleCount = frameSamples.size();
    const float barWidth = graphArea.x / static_cast<float>(std::max<std::size_t>(sampleCount, 1));
    for (std::size_t index = 0; index < sampleCount; ++index)
    {
        const float frameHeight = graphArea.y * std::clamp(frameSamples[index] / graphMaxMs, 0.0f, 1.0f);
        const float cpuHeight = graphArea.y * std::clamp(std::min(cpuSamples[index], frameSamples[index]) / graphMaxMs, 0.0f, 1.0f);
        const float x0 = graphPos.x + (barWidth * static_cast<float>(index));
        const float x1 = graphPos.x + (barWidth * static_cast<float>(index + 1));
        const float innerPad = barWidth > 2.0f ? 0.5f : 0.0f;

        drawList->AddRectFilled(
            ImVec2(x0 + innerPad, graphPos.y + graphArea.y - frameHeight),
            ImVec2(x1 - innerPad, graphPos.y + graphArea.y),
            frameColor
        );

        drawList->AddRectFilled(
            ImVec2(x0 + innerPad, graphPos.y + graphArea.y - cpuHeight),
            ImVec2(x1 - innerPad, graphPos.y + graphArea.y),
            cpuColor
        );

        if (index == m_selectedFrame)
        {
            drawList->AddRect(
                ImVec2(x0 + innerPad, graphPos.y + 1.0f),
                ImVec2(x1 - innerPad, graphPos.y + graphArea.y - 1.0f),
                selectedColor,
                0.0f,
                0,
                2.0f
            );
        }
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        const float localX = std::clamp(ImGui::GetIO().MousePos.x - graphPos.x, 0.0f, std::max(graphArea.x - 1.0f, 0.0f));
        const float normalized = graphArea.x > 0.0f ? (localX / graphArea.x) : 0.0f;
        const std::size_t clickedIndex = std::min<std::size_t>(
            static_cast<std::size_t>(normalized * static_cast<float>(frameSamples.size())),
            frameSamples.size() - 1
        );
        viewportPaused = true;
        performanceCapture.setPaused(true);
        m_selectedFrame = clickedIndex;
        m_flameGraphZoomActive = false;
        m_flameGraphZoomLabel.clear();
    }

    ImGui::Spacing();
    ImGui::ColorButton("##CpuLegend", ImVec4(1.0f, 170.0f / 255.0f, 64.0f / 255.0f, 1.0f), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(12.0f, 12.0f));
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextUnformatted("CPU time");
    ImGui::SameLine(0.0f, 14.0f);
    ImGui::ColorButton("##FrameLegend", ImVec4(111.0f / 255.0f, 168.0f / 255.0f, 1.0f, 1.0f), ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(12.0f, 12.0f));
    ImGui::SameLine(0.0f, 6.0f);
    ImGui::TextUnformatted("Frame time");

    const CapturedFrame& selectedFrame = frames[m_selectedFrame];
    const std::uint64_t selectedCpuCycles = performanceCapture.cpuCycles(selectedFrame);
    const float selectedFrameMs = performanceCapture.cyclesToMilliseconds(performanceCapture.frameCycles(selectedFrame));
    const float selectedCpuMs = performanceCapture.cyclesToMilliseconds(selectedCpuCycles);
    const float selectedCpuPercent = percentageOrZero(selectedCpuMs, selectedFrameMs);
    const bool zoomAppliesToSelectedFrame = m_flameGraphZoomActive && m_flameGraphZoomFrame == m_selectedFrame;
    const std::uint64_t flameRangeStart = zoomAppliesToSelectedFrame ? m_flameGraphZoomStart : selectedFrame.start;
    const std::uint64_t flameRangeStop = zoomAppliesToSelectedFrame ? m_flameGraphZoomStop : selectedFrame.stop;
    const std::uint64_t flameRangeCycles = std::max<std::uint64_t>(flameRangeStop - flameRangeStart, 1);

    ImGui::Spacing();
    ImGui::SeparatorText("Selected Frame");
    ImGui::Text("Frame %zu CPU used", m_selectedFrame);
    ImGui::Text("%.2f / %.2f ms (%.2f%%, %llu cycles)",
        selectedCpuMs,
        selectedFrameMs,
        selectedCpuPercent,
        static_cast<unsigned long long>(selectedCpuCycles));
    if (zoomAppliesToSelectedFrame)
    {
        ImGui::Text("Zoom: %s", m_flameGraphZoomLabel.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Reset Zoom"))
        {
            m_flameGraphZoomActive = false;
            m_flameGraphZoomLabel.clear();
        }
    }
    else
    {
        ImGui::TextUnformatted("Zoom: frame");
    }

    const std::vector<CapturedScope>& scopes = performanceCapture.scopes();
    std::vector<FlameGraphEntry> flameEntries;
    flameEntries.reserve(selectedFrame.scopeEndIndex - selectedFrame.scopeBeginIndex);
    std::vector<CycleRange> collapsedRanges;
    collapsedRanges.reserve(selectedFrame.scopeEndIndex - selectedFrame.scopeBeginIndex);

    struct ScopeStackEntry
    {
        std::uint64_t stop = 0;
        bool visible = false;
        bool hidesSubtree = false;
    };

    std::vector<ScopeStackEntry> stack;
    int visibleDepth = 0;
    int maxDepth = 0;
    for (std::size_t scopeIndex = selectedFrame.scopeBeginIndex; scopeIndex < selectedFrame.scopeEndIndex; ++scopeIndex)
    {
        const CapturedScope& scope = scopes[scopeIndex];
        if (scope.stop <= scope.start || scope.start < flameRangeStart || scope.stop > flameRangeStop)
        {
            continue;
        }

        while (!stack.empty() && scope.start >= stack.back().stop)
        {
            if (stack.back().visible)
            {
                --visibleDepth;
            }
            stack.pop_back();
        }

        const bool hiddenByAncestor = !stack.empty() && stack.back().hidesSubtree;
        const bool collapseThisScope =
            (m_compactFlameGraph && isCollapsedWaitScope(scope)) ||
            (m_compactImguiRelatedBlocks && isImguiRelatedCompactScope(scope));
        const bool hideThisScope = hiddenByAncestor || collapseThisScope;

        if (collapseThisScope && !hiddenByAncestor)
        {
            collapsedRanges.push_back({
                std::max(scope.start, flameRangeStart),
                std::min(scope.stop, flameRangeStop),
            });
        }

        if (!hideThisScope)
        {
            flameEntries.push_back({
                .scopeIndex = scopeIndex,
                .depth = visibleDepth,
            });
            ++visibleDepth;
            maxDepth = std::max(maxDepth, visibleDepth);
        }

        stack.push_back({
            .stop = scope.stop,
            .visible = !hideThisScope,
            .hidesSubtree = hiddenByAncestor || collapseThisScope,
        });
    }

    const std::vector<CycleRange> visibleRanges = buildVisibleRanges(
        flameRangeStart,
        flameRangeStop,
        mergeRanges(std::move(collapsedRanges)));
    const std::uint64_t compactedCycles = std::max<std::uint64_t>(totalRangeCycles(visibleRanges), 1);

    std::vector<FlameGraphEntry> displayedEntries;
    displayedEntries.reserve(flameEntries.size());
    if (m_compactFlameGraph || m_compactImguiRelatedBlocks)
    {
        for (const FlameGraphEntry& entry : flameEntries)
        {
            const CapturedScope& scope = scopes[entry.scopeIndex];
            const bool scopeHasVisiblePixels =
                compactedFractionForTimestamp(scope.stop, visibleRanges, compactedCycles) >
                compactedFractionForTimestamp(scope.start, visibleRanges, compactedCycles);
            if (scopeHasVisiblePixels)
            {
                displayedEntries.push_back(entry);
            }
        }
    }
    else
    {
        displayedEntries = flameEntries;
    }

    const float flameHeight = std::max(
        AppConfig::Perf::kFlameGraphMinHeight,
        AppConfig::Perf::kFlameGraphRowHeight * static_cast<float>(std::max(maxDepth, 1))
    );
    const ImVec2 flamePos = ImGui::GetCursorScreenPos();
    const ImVec2 flameArea(
        ImGui::GetContentRegionAvail().x > 0.0f ? ImGui::GetContentRegionAvail().x : 1.0f,
        flameHeight
    );

    ImGui::InvisibleButton("##FlameGraph", flameArea);
    drawList->AddRectFilled(flamePos, ImVec2(flamePos.x + flameArea.x, flamePos.y + flameArea.y), IM_COL32(18, 21, 27, 255));
    drawList->AddRect(flamePos, ImVec2(flamePos.x + flameArea.x, flamePos.y + flameArea.y), outlineColor);

    const CapturedScope* hoveredScope = nullptr;
    std::size_t clickedScopeIndex = static_cast<std::size_t>(-1);
    const bool flameGraphClicked = ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const ImVec2 mousePos = ImGui::GetIO().MousePos;

    for (const FlameGraphEntry& entry : displayedEntries)
    {
        const CapturedScope& scope = scopes[entry.scopeIndex];
        const bool useCompactedTimeline = m_compactFlameGraph || m_compactImguiRelatedBlocks;
        const float x0Fraction = useCompactedTimeline
            ? compactedFractionForTimestamp(scope.start, visibleRanges, compactedCycles)
            : (static_cast<float>(scope.start - flameRangeStart) / static_cast<float>(flameRangeCycles));
        const float x1Fraction = useCompactedTimeline
            ? compactedFractionForTimestamp(scope.stop, visibleRanges, compactedCycles)
            : (static_cast<float>(scope.stop - flameRangeStart) / static_cast<float>(flameRangeCycles));
        const float x0 = flamePos.x + (flameArea.x * x0Fraction);
        const float x1 = flamePos.x + (flameArea.x * x1Fraction);
        const float y0 = flamePos.y + (AppConfig::Perf::kFlameGraphRowHeight * static_cast<float>(entry.depth));
        const float y1 = std::min(y0 + AppConfig::Perf::kFlameGraphRowHeight - 3.0f, flamePos.y + flameArea.y - 2.0f);
        const ImVec2 rectMin(x0, y0);
        const ImVec2 rectMax(std::max(x1, x0 + 1.0f), y1);

        drawList->AddRectFilled(rectMin, rectMax, flameColorForDepth(entry.depth), 3.0f);
        drawList->AddRect(rectMin, rectMax, IM_COL32(0, 0, 0, 120), 3.0f);

        const float labelWidth = x1 - x0;
        if (labelWidth > 72.0f && scope.name != nullptr)
        {
            drawList->AddText(ImVec2(x0 + 6.0f, y0 + 4.0f), IM_COL32(15, 16, 18, 255), scope.name);
        }

        if (ImGui::IsItemHovered() &&
            mousePos.x >= rectMin.x && mousePos.x <= rectMax.x &&
            mousePos.y >= rectMin.y && mousePos.y <= rectMax.y)
        {
            hoveredScope = &scope;
            if (flameGraphClicked)
            {
                clickedScopeIndex = entry.scopeIndex;
            }
        }
    }

    if (clickedScopeIndex != static_cast<std::size_t>(-1))
    {
        const CapturedScope& clickedScope = scopes[clickedScopeIndex];
        m_flameGraphZoomActive = true;
        m_flameGraphZoomFrame = m_selectedFrame;
        m_flameGraphZoomStart = clickedScope.start;
        m_flameGraphZoomStop = clickedScope.stop;
        m_flameGraphZoomLabel = clickedScope.name != nullptr ? clickedScope.name : "(unnamed)";
    }

    if (hoveredScope != nullptr)
    {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(hoveredScope->name != nullptr ? hoveredScope->name : "(unnamed)");
        ImGui::Text("%.3f ms", performanceCapture.cyclesToMilliseconds(hoveredScope->stop - hoveredScope->start));
        ImGui::EndTooltip();
    }
}
