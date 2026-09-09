#ifdef __cplusplus
#define ATM_OUTPUT(type) type&
#else
#define ATM_OUTPUT(type) out type
#endif
#include "atmosphere_math.glsl"

struct AtmosphereOptics
{
    vec4 rayleigh; // RGB scattering, scale height
    vec4 mie; // scattering, extinction, scale height, anisotropy
    vec4 ozone; // RGB absorption, equivalent column height
    vec4 solar; // linear incident RGB radiance, atmosphere/cloud source scale
    vec4 radianceScales; // environment radiance scale, space source scale, disk source scale, reserved
};

vec3 airOpticalDepth(float h, float dy, float distance, float top, AtmosphereOptics a)
{
    // A bounded uniform ozone approximation preserves its configured vertical column.
    return vec3(a.rayleigh) * atmosphereColumn(h, dy, distance, a.rayleigh.w)
        + vec3(a.mie.y * atmosphereColumn(h, dy, distance, a.mie.z))
        + vec3(a.ozone) * (distance * a.ozone.w / top);
}

vec3 airSunTransmission(float h, vec3 sun, float top, AtmosphereOptics a)
{
    if (sun.y <= 0.0f) return vec3(0.0f);
    float path = max(top - h, 0.0f) / max(sun.y, 1.0e-7f);
    return exp(-airOpticalDepth(h, sun.y, path, top, a))
        * smoothstep(0.0f, 1.0e-5f, sun.y);
}

void evaluateAtmosphere(float height, vec3 direction, float distance, float top,
    vec3 sun, AtmosphereOptics a, bool scatter, ATM_OUTPUT(vec3) transmission, ATM_OUTPUT(vec3) scattering)
{
    transmission = vec3(1.0f);
    scattering = vec3(0.0f);
    float begin = atmosphereEntry(height, direction.y, distance, top);
    float end = atmosphereExit(height, direction.y, distance, top);
    float length = max(end - begin, 0.0f);
    if (length <= 0.0f) return;
    float h = min(height + direction.y * begin, top);
    transmission = exp(-airOpticalDepth(h, direction.y, length, top, a));
    if (!scatter || sun.y <= 0.0f) return;
    float mu = clamp(dot(direction, sun), -1.0f, 1.0f);
    float phaseR = 3.0f / (16.0f * 3.14159265359f) * (1.0f + mu * mu);
    float g = a.mie.w;
    float phaseM = (1.0f - g*g) / (4.0f * 3.14159265359f
        * pow(max(1.0f + g*g - 2.0f*g*mu, 1.0e-4f), 1.5f));
    // Eight-point Gauss-Legendre quadrature in analytically inverted column density.
    // Exponential importance sampling also resolves optically thick horizontal rays.
#ifdef __cplusplus
    const float nodes[8] = {
#else
    const float nodes[8] = float[8](
#endif
        0.019855072f,0.101666761f,0.237233795f,0.408282679f,
        0.591717321f,0.762766205f,0.898333239f,0.980144928f
#ifdef __cplusplus
    };
#else
    );
#endif
#ifdef __cplusplus
    const float weights[8] = {
#else
    const float weights[8] = float[8](
#endif
        0.050614268f,0.111190517f,0.156853323f,0.181341892f,
        0.181341892f,0.156853323f,0.111190517f,0.050614268f
#ifdef __cplusplus
    };
#else
    );
#endif
    float referenceCoefficient = max(min(min(a.rayleigh.r,a.rayleigh.g),a.rayleigh.b),1.0e-8f);
    float referenceHeight = a.rayleigh.w;
    if (max(max(a.rayleigh.r,a.rayleigh.g),a.rayleigh.b) == 0.0f)
    {
        referenceCoefficient = max(a.mie.y,1.0e-8f);
        referenceHeight = a.mie.z;
    }
    // In a planar medium, the solar column changes by -dy/sun.y times
    // the view column. Sample from the less-extinguished endpoint, including sunset.
    float solarSlope = 1.0f-direction.y/max(sun.y,1.0e-7f);
    bool reverseColumn = solarSlope < 0.0f;
    referenceCoefficient = max(referenceCoefficient*abs(solarSlope),1.0e-8f);
    float column = atmosphereColumn(h,direction.y,length,referenceHeight);
    float opticalRange = referenceCoefficient * column;
    float integralWeight = opticalRange < 0.001f
        ? opticalRange * (1.0f-opticalRange*0.5f+opticalRange*opticalRange/6.0f)
        : 1.0f-exp(-opticalRange);
    for (int i = 0; i < 8; ++i)
    {
        float opticalSample = -atmosphereLogOnePlus(-nodes[i]*integralWeight);
        float sampleColumn = opticalSample/referenceCoefficient;
        if (reverseColumn) sampleColumn = max(column-sampleColumn,0.0f);
        float s = clamp(atmosphereColumnDistance(h,direction.y,sampleColumn,referenceHeight),0.0f,length);
        float sampleHeight = h + direction.y * s;
        vec3 viewT = exp(-airOpticalDepth(h, direction.y, s, top, a));
        vec3 source = vec3(a.rayleigh) * (exp(-max(sampleHeight,0.0f)/a.rayleigh.w) * phaseR)
            + vec3(a.mie.x * exp(-max(sampleHeight,0.0f)/a.mie.z) * phaseM);
        scattering += weights[i] * (integralWeight * exp(opticalSample) / referenceCoefficient
            / max(exp(-max(sampleHeight,0.0f)/referenceHeight),1.0e-30f)) * viewT
            * airSunTransmission(sampleHeight, sun, top, a) * source;
    }
    scattering *= vec3(a.solar) * a.solar.w * a.radianceScales.x;
}

// Cubemap sampling already decodes authored sRGB; scattering is calibrated once above.
vec3 linearSkyRadiance(vec3 decodedSpace, vec3 transmission, vec3 scattering, AtmosphereOptics a)
{
    return decodedSpace * a.radianceScales.y * transmission + scattering;
}

// Directional background only; diffuse ambient lookups use linearSkyRadiance.
// The artistic solar RGB is not irradiance. radianceScales.z calibrates the disk
// independently of solar.w and radianceScales.x (atmosphere/cloud lighting).
vec3 directionalSkyRadiance(vec3 space, vec3 t, vec3 s, AtmosphereOptics a,
    vec3 ray, vec3 sun, float height, float top, float footprint)
{
    float angle=atan(length(cross(ray,sun)),dot(ray,sun));
    float edge=max(footprint,1.0e-6f);
    float disk=1.0f-smoothstep(0.004642576f-edge,0.004642576f+edge,angle);
    // Planar surface blocks downward celestial rays in the atmosphere. Above
    // the top, only rays entering the medium encounter that surface.
    if(ray.y<0.0f) disk=0.0f;
    vec3 source=min(max(vec3(a.solar)*a.radianceScales.z,vec3(0.0f)),vec3(60000.0f));
    return (space*a.radianceScales.y+disk*source)*t+s;
}

#undef ATM_OUTPUT
