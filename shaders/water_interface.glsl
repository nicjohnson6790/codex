// Normal points into the viewer's medium; incident points toward the surface.
// The same dielectric supplies the topside Schlick F0 and underside exact Fresnel.
const float kWaterIor = 1.333f;
const float kWaterF0 = ((kWaterIor - 1.0f) / (kWaterIor + 1.0f))
    * ((kWaterIor - 1.0f) / (kWaterIor + 1.0f));

struct WaterInterface
{
    vec3 reflected;
    vec3 transmitted;
    float reflectance;
};

WaterInterface waterToAirInterface(vec3 incident, vec3 normal)
{
    WaterInterface result;
    result.reflected = reflect(incident, normal);
    result.transmitted = vec3(0.0f);
    result.reflectance = 1.0f;
    float cosI = clamp(-dot(incident, normal), 0.0f, 1.0f);
    float cosT2 = 1.0f - kWaterIor * kWaterIor * (1.0f - cosI * cosI);
    if (cosT2 <= 0.0f) return result; // Snell's law: no propagating air ray.
    float cosT = sqrt(cosT2);
    result.transmitted = kWaterIor * incident + (kWaterIor * cosI - cosT) * normal;
    float rs = (kWaterIor * cosI - cosT) / (kWaterIor * cosI + cosT);
    float rp = (cosI - kWaterIor * cosT) / (cosI + kWaterIor * cosT);
    result.reflectance = clamp(0.5f * (rs * rs + rp * rp), 0.0f, 1.0f);
    return result;
}
