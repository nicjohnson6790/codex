// Only human-authored material colors use this decode; data and radiance do not.
float decodeAuthoredSrgb(float value)
{
    return value <= 0.04045f ? value / 12.92f
        : pow((value + 0.055f) / 1.055f, 2.4f);
}
vec3 authoredColor(vec3 color)
{
    return vec3(decodeAuthoredSrgb(color.r), decodeAuthoredSrgb(color.g), decodeAuthoredSrgb(color.b));
}
