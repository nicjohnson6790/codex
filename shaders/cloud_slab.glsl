// Ray origin is height relative to slab bottom. A finite view limit also handles
// horizontal rays inside the slab; sunlight uses its actual positive slab exit.
vec2 cloudSlabInterval(float height,float dy,float thickness,float limit)
{
    if(abs(dy)<1e-7f)
        return height>0.0f && height<thickness ? vec2(0.0f,limit) : vec2(0.0f);
    float a=-height/dy,b=(thickness-height)/dy;
    return vec2(max(0.0f,min(a,b)),min(limit,max(a,b)));
}

// Reusable axis interval; parallel rays never divide by zero.
vec2 cloudAxisInterval(float origin,float direction,float low,float high,float limit)
{
    if(abs(direction)<1e-7f)
        return origin>=low && origin<=high ? vec2(0.0f,limit) : vec2(limit,0.0f);
    float a=(low-origin)/direction,b=(high-origin)/direction;
    return vec2(max(0.0f,min(a,b)),min(limit,max(a,b)));
}
vec2 cloudDomainInterval(vec3 origin,vec3 ray,vec3 low,vec3 high,float limit)
{
    vec2 x=cloudAxisInterval(origin.x,ray.x,low.x,high.x,limit);
    vec2 y=cloudAxisInterval(origin.y,ray.y,low.y,high.y,limit);
    vec2 z=cloudAxisInterval(origin.z,ray.z,low.z,high.z,limit);
    return vec2(max(x.x,max(y.x,z.x)),min(x.y,min(y.y,z.y)));
}
// Quadratic distance distribution gives near entry finer spacing without losing
// the far endpoint. Count also grows with occupied path length (bounded by budget).
float cloudStepBoundary(float fraction,float length)
{
    return length*(0.25f*fraction+0.75f*fraction*fraction);
}
