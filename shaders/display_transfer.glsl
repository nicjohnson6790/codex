// The sole scene-to-display transfer. Working primaries are linear Rec.709.
float encodeDisplaySrgb(float value)
{
    return value <= 0.0031308f ? 12.92f * value
        : 1.055f * pow(value, 1.0f / 2.4f) - 0.055f;
}

float displayTransfer(float radiance, float exposure)
{
    float mapped = 1.0f - exp(-max(radiance, 0.0f) * exposure);
    return encodeDisplaySrgb(mapped);
}
