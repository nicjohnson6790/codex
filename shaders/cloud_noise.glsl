// Integer lattice periods guarantee repetition in every axis, including FBM octaves.
vec3 cloudHash(vec3 p)
{
    return fract(sin(vec3(dot(p,vec3(127.1f,311.7f,74.7f)),dot(p,vec3(269.5f,183.3f,246.1f)),
        dot(p,vec3(113.5f,271.9f,124.6f))))*43758.5453f);
}
vec3 cloudWrap(vec3 p,float period) { return p-floor(p/period)*period; }
float cloudGradient(vec3 uv,float frequency)
{
    vec3 p=fract(uv)*frequency, cell=floor(p), f=fract(p);
    vec3 u=f*f*f*(f*(f*6.0f-15.0f)+10.0f);
    float result=0.0f;
    for(int z=0;z<2;++z) for(int y=0;y<2;++y) for(int x=0;x<2;++x)
    {
        vec3 corner=vec3(x,y,z);
        vec3 gradient=normalize(cloudHash(cloudWrap(cell+corner,frequency))*2.0f-1.0f+vec3(1e-5f));
        vec3 w=mix(vec3(1.0f)-u,u,corner);
        result+=dot(gradient,f-corner)*w.x*w.y*w.z;
    }
    return result;
}
float cloudWorley(vec3 uv,float frequency)
{
    vec3 p=fract(uv)*frequency, cell=floor(p), f=fract(p);
    float d=3.0f;
    for(int z=-1;z<=1;++z) for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
    {
        vec3 offset=vec3(x,y,z);
        vec3 delta=offset+cloudHash(cloudWrap(cell+offset,frequency))-f;
        d=min(d,dot(delta,delta));
    }
    return 1.0f-clamp(sqrt(d),0.0f,1.0f);
}
float cloudRemap(float value,float lower) { return clamp((value-lower)/max(1.0f-lower,1e-5f),0.0f,1.0f); }
vec2 cloudNoise(vec3 uv)
{
    float gradient=0.0f, amplitude=1.0f, norm=0.0f, frequency=2.0f;
    for(int i=0;i<8;++i)
    {
        gradient+=amplitude*cloudGradient(uv,frequency);
        norm+=amplitude; amplitude*=0.5f; frequency*=2.0f;
    }
    gradient=clamp(0.5f+gradient/norm,0.0f,1.0f);
    float baseW=0.625f*cloudWorley(uv,2.0f)+0.25f*cloudWorley(uv,4.0f)+0.125f*cloudWorley(uv,8.0f);
    float base=cloudRemap(gradient, -(1.0f-baseW));
    float w[6];
    for(int i=0;i<6;++i) w[i]=cloudWorley(uv,4.0f*exp2(float(i)));
    float f0=dot(vec3(w[0],w[1],w[2]),vec3(0.625f,0.25f,0.125f));
    float f1=dot(vec3(w[1],w[2],w[3]),vec3(0.625f,0.25f,0.125f));
    float f2=dot(vec3(w[2],w[3],w[4]),vec3(0.625f,0.25f,0.125f));
    // Sixth field is reserved by the specified construction; the overlapping FBMs end at field 4.f
    float detail=0.625f*f0+0.25f*f1+0.125f*f2;
    return vec2(base,cloudRemap(base,1.0f-detail));
}
