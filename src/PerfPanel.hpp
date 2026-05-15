#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class PerfPanel
{
public:
    void draw(bool& viewportPaused);

private:
    std::size_t m_selectedFrame = 0;
    bool m_compactFlameGraph = true;
    bool m_compactImguiRelatedBlocks = true;
    bool m_flameGraphZoomActive = false;
    bool m_flameGraphFollowLive = false;
    std::size_t m_flameGraphZoomFrame = 0;
    std::uint64_t m_flameGraphZoomStart = 0;
    std::uint64_t m_flameGraphZoomStop = 0;
    std::string m_flameGraphZoomLabel;
    std::size_t m_childScopeRootFrame = 0;
    std::uint64_t m_childScopeRootStart = 0;
    std::uint64_t m_childScopeRootStop = 0;
    std::string m_childScopeRootLabel;
    std::vector<std::string> m_childScopePath;
};
