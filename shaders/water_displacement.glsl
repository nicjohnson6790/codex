// The FFT maps already include amplitude/choppiness. All callers use the same
// render-origin phase and explicit level-zero filtering (the maps have one mip).
vec3 sampleWaterDisplacement(sampler2DArray maps, vec2 relativeXZ,
    vec2 originPhase, float worldSize, uint cascade)
{
    return textureLod(maps, vec3(originPhase + relativeXZ / max(worldSize, 1.0),
        float(cascade)), 0.0).xyz;
}

float waterShallowFade(float localDepth, float shallowDepth, float fadeEnd, float strength)
{
    float fade = smoothstep(max(fadeEnd,0.0),max(shallowDepth,fadeEnd+0.001),localDepth);
    return mix(1.0,fade,clamp(strength,0.0,8.0));
}
