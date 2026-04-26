#pragma once

#include "Position.hpp"

class HeightmapNoiseGenerator
{
public:
    void fillNoise(const Position& a, const Position& b, float* buffer) const;
};
