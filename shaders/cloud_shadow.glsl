// Requires the shared density and domain helpers. Pure Beer transmission only.
// params: extinction (1/m), termination, enabled/ready, terrain sample count.
float terrainCloudTransmission(vec3 origin, vec3 ray, CloudDensityField field,
    vec4 params, sampler2D coverageTexture, sampler3D noiseTexture)
{
    if(params.z<=0.0f || params.x<=0.0f) return 1.0f;
    vec3 low=vec3(field.macro.x,field.layer.x,field.macro.y);
    float width=(field.macro.w-1.0f)*field.macro.z;
    vec3 high=low+vec3(width,field.layer.y,width);
    // The finite footprint/slab supplies the exit; this is no artistic distance cap.
    vec2 interval=cloudDomainInterval(origin,ray,low,high,3.402823e38f);
    if(interval.y<=interval.x) return 1.0f;
    int count=clamp(int(params.w),1,32);
    float ds=(interval.y-interval.x)/float(count);
    float tau=0.0f, transmission=1.0f;
    for(int i=0;i<count && transmission>params.y;++i)
    {
        vec3 p=origin+ray*(interval.x+(float(i)+0.5f)*ds);
        tau+=sampleCloudDensity(p,field,coverageTexture,noiseTexture)*params.x*ds;
        transmission=exp(-tau);
    }
    return transmission;
}
