#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

class PerfPanel
{
public:
    void draw(bool& viewportPaused);

private:
    std::size_t m_selectedFrame = 0;
    bool m_compactFlameGraph = true;
    bool m_flameGraphZoomActive = false;
    std::size_t m_flameGraphZoomFrame = 0;
    std::uint64_t m_flameGraphZoomStart = 0;
    std::uint64_t m_flameGraphZoomStop = 0;
    std::string m_flameGraphZoomLabel;
};
