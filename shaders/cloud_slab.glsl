// Ray origin is height relative to slab bottom. A finite view limit also handles
// horizontal rays inside the slab; sunlight uses its actual positive slab exit.
vec2 cloudSlabInterval(float height,float dy,float thickness,float limit)
{
    if(abs(dy)<1e-7f)
        return height>0.0f && height<thickness ? vec2(0.0f,limit) : vec2(0.0f);
    float a=-height/dy,b=(thickness-height)/dy;
    return vec2(max(0.0f,min(a,b)),min(limit,max(a,b)));
}
