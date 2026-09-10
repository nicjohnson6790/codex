// Endpoints are sine elevations supplied from AppConfig::Terrain.
float terrainSolarVisibility(float sunY, float horizonEnd)
{
    return smoothstep(0.0f, horizonEnd, sunY);
}
