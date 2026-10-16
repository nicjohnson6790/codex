// Incoming illumination through a flat mean water surface. Camera-side medium
// compositing remains a separate optical path. No refraction or in-scattering.
vec3 waterColumnTransmission(float depth,float upwardCosine,vec3 extinction)
{
    if(depth<=0.0f) return vec3(1.0f);
    if(upwardCosine<=0.0f) return vec3(0.0f);
    float distance=depth/max(upwardCosine,1.0e-6f);
    return exp(-max(extinction,vec3(0.0f))*distance);
}
