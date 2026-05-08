#version 450

layout(set=3, binding=0) uniform ImposterCaptureMaterialUniforms
{
    vec4 viewBasisRight;
    vec4 viewBasisUp;
    vec4 viewBasisForward;
    vec4 sunDirectionIntensity;
    vec4 sunColorAmbient;
    vec4 shadingParams0;
    vec4 cameraPositionAlphaCutoff;
} capture;

layout(set=2, binding=0) uniform sampler2D baseColorTexture;
layout(set=2, binding=1) uniform sampler2D normalTexture;
layout(set=2, binding=2) uniform sampler2D roughnessTexture;
layout(set=2, binding=3) uniform sampler2D specularTexture;
layout(set=2, binding=4) uniform sampler2D aoTexture;
layout(set=2, binding=5) uniform sampler2D subsurfaceTexture;

layout(location = 0) in vec2 fragUv0;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec3 fragTangent;
layout(location = 3) in vec3 fragBitangent;
layout(location = 4) in vec3 fragWorldPosition;

layout(location = 0) out vec4 outColor;

const float kPi = 3.14159265359;
const float kFoliageAmbientBoost = 1.85;
const float kFoliageSkyFillStrength = 0.24;

float saturate(float value)
{
    return clamp(value, 0.0, 1.0);
}

vec3 fresnelSchlick(float cosTheta, vec3 f0)
{
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cosTheta), 5.0);
}

float luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

float distributionGgx(float nDotH, float roughness)
{
    float alpha = roughness * roughness;
    float alphaSquared = alpha * alpha;
    float denominator = (nDotH * nDotH) * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / max(kPi * denominator * denominator, 0.0001);
}

float geometrySchlickGgx(float nDotV, float roughness)
{
    float k = pow(roughness + 1.0, 2.0) / 8.0;
    return nDotV / max(nDotV * (1.0 - k) + k, 0.0001);
}

float geometrySmith(float nDotV, float nDotL, float roughness)
{
    return geometrySchlickGgx(nDotV, roughness) * geometrySchlickGgx(nDotL, roughness);
}

vec3 sampleSkyRadiance(vec3 worldDirection)
{
    float upT = saturate(worldDirection.y * 0.5 + 0.5);
    vec3 horizon = vec3(0.74, 0.82, 0.90);
    vec3 zenith = vec3(0.24, 0.41, 0.67);
    vec3 ground = vec3(0.16, 0.15, 0.13);
    vec3 sky = mix(horizon, zenith, sqrt(upT));
    sky = mix(ground, sky, step(0.0, worldDirection.y));
    float sunGlow = pow(saturate(dot(normalize(capture.sunDirectionIntensity.xyz), worldDirection)), 64.0);
    return sky + (capture.sunColorAmbient.rgb * sunGlow * 0.25);
}

void main()
{
    vec4 albedoSample = texture(baseColorTexture, fragUv0);
    if (albedoSample.a < capture.cameraPositionAlphaCutoff.w)
    {
        discard;
    }

    vec3 tangentNormal = texture(normalTexture, fragUv0).xyz * 2.0 - 1.0;
    if (!gl_FrontFacing)
    {
        tangentNormal.xy *= -1.0;
    }

    mat3 tbn = mat3(
        normalize(fragTangent),
        normalize(fragBitangent),
        normalize(fragNormal));
    vec3 normal = normalize(tbn * tangentNormal);
    if (!gl_FrontFacing)
    {
        normal = -normal;
    }

    vec3 sunDirection = normalize(capture.sunDirectionIntensity.xyz);
    vec3 viewDirection = normalize(capture.cameraPositionAlphaCutoff.xyz - fragWorldPosition);
    vec3 halfVector = normalize(viewDirection + sunDirection);

    float roughness = clamp(texture(roughnessTexture, fragUv0).r, 0.08, 1.0);
    vec3 specularColor = texture(specularTexture, fragUv0).rgb;
    float ao = texture(aoTexture, fragUv0).r;
    float effectiveAo = mix(1.0, ao, 0.35);
    vec3 subsurfaceColor = texture(subsurfaceTexture, fragUv0).rgb;

    float nDotL = saturate(dot(normal, sunDirection));
    float nDotV = saturate(dot(normal, viewDirection));
    float nDotH = saturate(dot(normal, halfVector));
    float vDotH = saturate(dot(viewDirection, halfVector));

    vec3 dielectricF0 = vec3(capture.shadingParams0.x);
    float specularStrength = saturate(luminance(specularColor));
    vec3 f0 = dielectricF0 * mix(0.22, 0.72, specularStrength);
    f0 = clamp(f0, vec3(0.008), vec3(0.045));
    vec3 fresnel = fresnelSchlick(vDotH, f0);
    float distribution = distributionGgx(nDotH, roughness);
    float geometry = geometrySmith(max(nDotV, 0.05), max(nDotL, 0.05), roughness);
    vec3 specular = (distribution * geometry * fresnel) / max(4.0 * max(nDotV, 0.05) * max(nDotL, 0.05), 0.0001);

    vec3 diffuseColor = albedoSample.rgb * (1.0 - fresnel);
    vec3 diffuse = (diffuseColor / kPi) * nDotL;

    float transmissionStrength = capture.shadingParams0.z;
    float backScatter = pow(saturate(dot(-viewDirection, sunDirection)), 2.0) * saturate(dot(-normal, sunDirection));
    vec3 transmission = subsurfaceColor * albedoSample.rgb * backScatter * transmissionStrength;

    vec3 sunRadiance = capture.sunColorAmbient.rgb * capture.sunDirectionIntensity.w;
    vec3 directLighting = diffuse * sunRadiance + (specular * sunRadiance * 0.16) + transmission * sunRadiance;

    vec3 reflectionDirection = reflect(-viewDirection, normal);
    vec3 reflectedSky = sampleSkyRadiance(reflectionDirection);
    vec3 environmentSpecular =
        reflectedSky *
        fresnelSchlick(nDotV, f0) *
        mix(0.16, 0.05, roughness) *
        mix(0.35, 0.7, pow(1.0 - nDotV, 0.35));
    environmentSpecular *= mix(0.8, 1.0, effectiveAo);

    vec3 skyFill = sampleSkyRadiance(normal) * albedoSample.rgb * kFoliageSkyFillStrength;
    vec3 ambient =
        albedoSample.rgb *
        capture.sunColorAmbient.a *
        mix(0.75, 1.0, effectiveAo) *
        kFoliageAmbientBoost;
    vec3 litColor = ambient + skyFill + directLighting + environmentSpecular;
    outColor = vec4(litColor, albedoSample.a);
}
