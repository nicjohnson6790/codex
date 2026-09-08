#pragma once
#include <algorithm>
#include <cmath>

inline int waterCloudSampleCount(int primary, float multiplier)
{
    primary=std::max(primary,1);
    return std::clamp(int(std::round(primary*std::clamp(multiplier,0.1f,1.0f))),1,primary);
}
