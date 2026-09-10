// Endpoints are sine elevations supplied from AppConfig::Terrain.
float terrainSolarVisibility(float sunY, float horizonEnd)
{
    return smoothstep(0.0f, horizonEnd, sunY);
}
float terrainAmbientDayFactor(float sunY, float nightEnd, float dayStart)
{
    return smoothstep(nightEnd, dayStart, sunY);
}
