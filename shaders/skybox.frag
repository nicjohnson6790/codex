#version 450
#extension GL_GOOGLE_include_directive : require
#include "atmosphere.glsl"
#include "water_displacement.glsl"
#include "water_medium.glsl"

layout(location=0) in vec2 fragNdc;
layout(location=0) out vec4 outColor;
layout(set=2, binding=0) uniform samplerCube skyboxTexture;
layout(set=2, binding=1) uniform sampler2D depthTexture;
layout(set=2, binding=2) uniform sampler2DArray displacementTexture;
layout(set=2, binding=3, std430) readonly buffer HeightmapBuffer { float heights[]; } heightmapBuffer;
layout(set=3, binding=0) uniform SkyboxUniforms
{
    mat4 inverseViewProjection;
    mat4 skyRotation;
    vec4 atmosphereParams;
    vec4 sunDirectionTimeOfDay;
    AtmosphereOptics optics;
    vec4 waterParams;
    vec4 waterSizes;
    vec4 waterPhasesA;
    vec4 waterPhasesB;
    vec4 waterAbsorption;
    vec4 waterScattering;
    vec4 waterDamping;
    vec4 waterShallowDepth;
    vec4 waterCameraLeaf;
    vec4 waterFilter;
    vec4 waterDepthParams;
} uniforms;

vec3 reconstructPosition(float depth)
{
    vec4 p = uniforms.inverseViewProjection * vec4(fragNdc, depth, 1.0);
    return p.xyz / max(p.w, 1.0e-20);
}

vec2 waterPhase(uint i)
{
    if (i == 0u) return uniforms.waterPhasesA.xy;
    if (i == 1u) return uniforms.waterPhasesA.zw;
    if (i == 2u) return uniforms.waterPhasesB.xy;
    return uniforms.waterPhasesB.zw;
}

float cameraSurfaceHeight()
{
    float localDepth = uniforms.waterDepthParams.x;
    bool hasTerrain = uniforms.waterCameraLeaf.w >= 0.0;
    if (hasTerrain)
    {
        vec2 localMeters = -uniforms.waterCameraLeaf.xy;
        ivec2 coord = clamp(ivec2(round(vec2(1.0) + localMeters
            / (uniforms.waterCameraLeaf.z / 256.0))), ivec2(0), ivec2(258));
        uint index = uint(uniforms.waterCameraLeaf.w) * 259u * 259u
            + uint(coord.y) * 259u + uint(coord.x);
        localDepth = max(uniforms.waterParams.x - heightmapBuffer.heights[index], 0.0);
    }
    float viewDistance = abs(uniforms.waterParams.x - uniforms.atmosphereParams.w);
    float pixelSize = max(2.0 * uniforms.waterFilter.y * viewDistance
        / max(uniforms.waterFilter.x,1.0), 1.0e-4);
    vec3 displacement = vec3(0.0);
    for (uint i=0u; i<uint(uniforms.waterParams.y); ++i)
    {
        if ((uint(uniforms.waterDepthParams.z) & (1u << i)) == 0u) continue;
        float detail = smoothstep(uniforms.waterFilter.z, uniforms.waterFilter.w,
            uniforms.waterSizes[i] / (512.0 * pixelSize));
        float fade = hasTerrain ? waterShallowFade(localDepth, uniforms.waterShallowDepth[i], uniforms.waterDepthParams.y,
            uniforms.waterDamping[i]) : 1.0;
        displacement += sampleWaterDisplacement(displacementTexture, vec2(0.0),
            waterPhase(i), uniforms.waterSizes[i], i) * (fade * detail);
    }
    return uniforms.waterParams.x + displacement.y;
}

void main()
{
    ivec2 pixel = clamp(ivec2(gl_FragCoord.xy), ivec2(0), textureSize(depthTexture,0)-1);
    float depth = texelFetch(depthTexture, pixel, 0).r;
    vec3 direction = normalize(reconstructPosition(1.0));
    int pass = int(uniforms.waterParams.w);
    if (pass == 0 && depth > 0.0) discard;
    float height = uniforms.atmosphereParams.w;
    float surface = uniforms.waterParams.z > 0.5 ? cameraSurfaceHeight() : height;
    vec3 sun = uniforms.sunDirectionTimeOfDay.xyz;
    if (pass == 0)
    {
        vec3 space = texture(skyboxTexture, transpose(mat3(uniforms.skyRotation)) * direction).rgb;
        if (height < surface) outColor = vec4(space,1.0);
        else
        {
            float skyDistance = uniforms.atmosphereParams.y;
            if (height > uniforms.atmosphereParams.x && direction.y < 0.0)
                skyDistance += (height-uniforms.atmosphereParams.x)/-direction.y;
            vec3 t, s;
            evaluateAtmosphere(height,direction,skyDistance,uniforms.atmosphereParams.x,
                sun,uniforms.optics,true,t,s);
            outColor = vec4(displaySkyRadiance(space,t,s,uniforms.optics),1.0);
        }
        return;
    }
    float distance = depth > 0.0 ? length(reconstructPosition(depth)) : uniforms.atmosphereParams.y;
    // Air background was composed and tone mapped together in pass zero.
    if (depth <= 0.0 && height >= surface) discard;
    vec3 transmission, scattering = vec3(0.0);
    if (height < surface)
    {
        vec3 sigmaS = max(uniforms.waterScattering.rgb,vec3(0.0));
        vec3 sigmaT = max(uniforms.waterAbsorption.rgb,vec3(0.0)) + sigmaS;
        transmission = exp(-sigmaT * distance);
        if (pass == 2 && sun.y > 0.0)
        {
            float sunPath = (surface-height) / max(sun.y,1.0e-7);
            vec3 mediumSun = uniforms.optics.solar.rgb
                * airSunTransmission(surface,sun,uniforms.atmosphereParams.x,uniforms.optics)
                * exp(-sigmaT * sunPath);
            scattering = mediumSun * (uniforms.waterScattering.w / (4.0 * 3.14159265359))
                * vec3(waterScatteringIntegral(sigmaS.r,sigmaT.r,distance),
                    waterScatteringIntegral(sigmaS.g,sigmaT.g,distance),
                    waterScatteringIntegral(sigmaS.b,sigmaT.b,distance));
        }
    }
    else
    {
        evaluateAtmosphere(height,direction,distance,uniforms.atmosphereParams.x,sun,
            uniforms.optics,pass == 2,transmission,scattering);
    }
    outColor = vec4(pass == 1 ? transmission : scattering,0.0);
}
