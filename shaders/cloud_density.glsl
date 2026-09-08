struct CloudDensityField
{
    vec4 layer;
    vec4 macro;
    vec4 basePhase;
    vec4 detailPhase;
    vec4 shape;
};
// p is relative to the shared render origin, never to a specific sampling camera.
// Shadow generation can pass its own positions and these identical field uniforms.
float sampleCloudDensity(vec3 p, CloudDensityField field, sampler2D coverageTexture, sampler3D noiseTexture)
{
    float h=(p.y-field.layer.x)/field.layer.y;
    if(h<=0.0 || h>=1.0) return 0.0;
    float profile=smoothstep(0.0,0.12,h)*(1.0-smoothstep(0.65,1.0,h));
    vec2 lattice=(p.xz-field.macro.xy)/field.macro.z;
    if(any(lessThan(lattice,vec2(0.0))) || any(greaterThan(lattice,vec2(80.0)))) return 0.0;
    float macro=textureLod(coverageTexture,(lattice+0.5)/field.macro.w,0.0).r;
    float coverage=clamp(macro+field.layer.z-0.5,0.0,1.0);
    float density=profile*coverage;
    float base=textureLod(noiseTexture,fract(p*field.basePhase.w+field.basePhase.xyz),0.0).r;
    float threshold=(1.0-base)*field.shape.x;
    density=clamp((density-threshold)/max(1.0-threshold,1e-5),0.0,1.0);
    if(density<=0.0) return 0.0;
    float detail=textureLod(noiseTexture,fract(p*field.detailPhase.w+field.detailPhase.xyz),0.0).g;
    threshold=clamp((1.0-detail)*field.shape.y*field.shape.z,0.0,0.99);
    return clamp((density-threshold)/max(1.0-threshold,1e-5),0.0,1.0)*field.layer.w;
}
