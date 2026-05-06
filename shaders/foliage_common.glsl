float foliageHash01(uint seed)
{
    seed ^= seed >> 16u;
    seed *= 0x7feb352du;
    seed ^= seed >> 15u;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16u;
    return float(seed & 0x00FFFFFFu) / float(0x01000000u);
}

vec2 foliageJitterOffset(uint seed)
{
    float jitterX = (foliageHash01(seed ^ 0x68bc21ebu) * 2.0) - 1.0;
    float jitterZ = (foliageHash01(seed ^ 0x02e5be93u) * 2.0) - 1.0;
    return vec2(jitterX, jitterZ) * 1.2;
}
