// Both color and depth use identical registered UVs, four view weights and
// coverage. Out-of-frame capture samples are transparent, never edge-clamped.
vec4 imposterViewColor(sampler2DArray tex, vec2 uv, uint layer)
{
    vec4 value=texture(tex,vec3(uv,float(layer)));
    bool inside=all(greaterThanEqual(uv,vec2(0))) && all(lessThanEqual(uv,vec2(1)));
    return inside ? value : vec4(0);
}

vec4 imposterBlendedColor(sampler2DArray tex,vec2 lowUv,vec2 highUv,
    uvec2 lowLayers,uvec2 highLayers,float yawBlend,float pitchBlend,out vec4 weights)
{
    vec4 a=imposterViewColor(tex,lowUv,lowLayers.x);
    vec4 b=imposterViewColor(tex,lowUv,lowLayers.y);
    vec4 c=imposterViewColor(tex,highUv,highLayers.x);
    vec4 d=imposterViewColor(tex,highUv,highLayers.y);
    weights=vec4((1-yawBlend)*(1-pitchBlend),yawBlend*(1-pitchBlend),
                 (1-yawBlend)*pitchBlend,yawBlend*pitchBlend)*vec4(a.a,b.a,c.a,d.a);
    float alpha=dot(weights,vec4(1));
    return vec4((a.rgb*weights.x+b.rgb*weights.y+c.rgb*weights.z+d.rgb*weights.w)/max(alpha,1e-6),alpha);
}
