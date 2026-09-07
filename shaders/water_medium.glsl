// Stable homogeneous single-scattering integral, including zero extinction.
// Returns integral sigmaS * exp(-sigmaT*s) ds, per RGB channel.
float waterScatteringIntegral(float sigmaS, float sigmaT, float distance)
{
    float tau = sigmaT * distance;
    float average = tau < 0.001f
        ? 1.0f-tau*0.5f+tau*tau/6.0f
        : (1.0f-exp(-tau))/tau;
    return sigmaS * distance * average;
}

// Existing homogeneous, isotropic underwater approximation. Direction has no
// effect in this model; infinite reflected rays start at the surface (depth 0).
vec3 waterMediumRadiance(float surface, float depth, float distance, bool infiniteRay,
    vec3 sun, float atmosphereHeight, AtmosphereOptics optics,
    vec3 absorption, vec4 scattering)
{
    if (sun.y <= 0.0f) return vec3(0.0f);
    vec3 sigmaS = max(vec3(scattering), vec3(0.0f));
    vec3 sigmaT = max(absorption, vec3(0.0f)) + sigmaS;
    vec3 mediumSun = vec3(optics.solar)
        * airSunTransmission(surface, sun, atmosphereHeight, optics)
        * exp(-sigmaT * (max(depth, 0.0f) / max(sun.y, 1.0e-7f)));
    vec3 integral = infiniteRay
        ? sigmaS / max(sigmaT, vec3(1.0e-30f))
        : vec3(waterScatteringIntegral(sigmaS.r, sigmaT.r, distance),
            waterScatteringIntegral(sigmaS.g, sigmaT.g, distance),
            waterScatteringIntegral(sigmaS.b, sigmaT.b, distance));
    return mediumSun * (scattering.w / (4.0f * 3.14159265359f)) * integral;
}
