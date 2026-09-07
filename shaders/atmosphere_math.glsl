// Scalar math shared with the CPU regression test; distances/heights are meters.
// The caller clips to the atmosphere top. There is deliberately no bottom plane.
float atmosphereEntry(float height, float dy, float distance, float top)
{
    if (height <= top) return 0.0f;
    if (dy >= 0.0f) return distance;
    return min(distance, (height - top) / -dy);
}

float atmosphereExit(float height, float dy, float distance, float top)
{
    if (height > top && dy >= 0.0f) return distance;
    if (dy > 0.0f) return min(distance, max(top - height, 0.0f) / dy);
    return distance;
}

// Integral of exp(-max(h + dy*s, 0)/H). Split at sea level and use
// the lower endpoint density, avoiding overflow on descending rays.
float atmosphereColumn(float height, float dy, float distance, float scaleHeight)
{
    float length = max(distance, 0.0f);
    float endHeight = height + dy * length;
    float low = min(height, endHeight);
    float high = max(height, endHeight);
    if (high <= 0.0f) return length;
    float below = low < 0.0f ? length * (-low / (high - low)) : 0.0f;
    float above = max(length - below, 0.0f);
    float x = abs(dy) * above / scaleHeight;
    float average = x < 0.01f
        ? 1.0f - x * 0.5f + x * x / 6.0f - x * x * x / 24.0f
        : (1.0f - exp(-x)) / x;
    return below + above * exp(-max(low, 0.0f) / scaleHeight) * average;
}

float atmosphereLogOnePlus(float x)
{
    return abs(x) < 0.001f ? x * (1.0f - x * 0.5f + x*x/3.0f) : log(1.0f+x);
}

// Inverse of the clamped exponential column. Used to distribute the fixed
// quadrature samples in optical depth, including long descending background rays.
float atmosphereColumnDistance(float height, float dy, float column, float scale)
{
    if (column <= 0.0f) return 0.0f;
    float density = exp(-max(height,0.0f)/scale);
    if (dy == 0.0f) return column / max(density,1.0e-30f);
    if (height <= 0.0f)
    {
        if (dy < 0.0f) return column;
        float below = -height/dy;
        if (column <= below) return column;
        return below - scale/dy * atmosphereLogOnePlus(max(-(column-below)*dy/scale,-0.99999994f));
    }
    if (dy < 0.0f)
    {
        float seaColumn = scale/-dy * (1.0f-density);
        if (column >= seaColumn) return height/-dy + column-seaColumn;
    }
    float x = -column*dy/(scale*max(density,1.0e-30f));
    return -scale/dy * atmosphereLogOnePlus(max(x,-0.99999994f));
}
