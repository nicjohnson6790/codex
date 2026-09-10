// Four cell-centered ground probes per axis, with half-pitch margins.
vec2 skyProbeFraction(int probe)
{
    return (vec2(float(probe%4),float(probe/4))+vec2(0.5f))/4.0f;
}
vec2 skyProbeGridCoordinate(vec2 fraction)
{
    return clamp(fraction*4.0f-vec2(0.5f),vec2(0.0f),vec2(3.0f));
}
// Equal-solid-angle stratification of the upper hemisphere. All 256 rays see sky;
// latitude rings are staggered in azimuth to avoid aligned meridian artifacts.
vec3 skyProbeDirection(int sampleIndex)
{
    int row=sampleIndex/16, column=sampleIndex%16;
    float y=(float(row)+0.5f)/16.0f;
    float phi=6.28318530718f*(float(column)+0.5f+float(row)*0.61803398875f)/16.0f;
    float r=sqrt(max(0.0f,1.0f-y*y));
    return vec3(r*cos(phi),y,r*sin(phi));
}
float skyProbeBasis(int index,vec3 d)
{
    if(index==0) return 0.2820947918f;
    if(index==1) return 0.4886025119f*d.y;
    if(index==2) return 0.4886025119f*d.z;
    if(index==3) return 0.4886025119f*d.x;
    if(index==4) return 1.0925484306f*d.x*d.y;
    if(index==5) return 1.0925484306f*d.y*d.z;
    if(index==6) return 0.3153915653f*(3.0f*d.z*d.z-1.0f);
    if(index==7) return 1.0925484306f*d.x*d.z;
    return 0.5462742153f*(d.x*d.x-d.y*d.y);
}
float skyProbeConvolution(int index)
{
    return index==0 ? 3.14159265359f : (index<4 ? 2.09439510239f : 0.7853981634f);
}
float skyProbeRegionWeight(vec2 p,vec4 domain)
{
    float dx=max(max(domain.x-p.x,p.x-domain.x-domain.z),0.0f);
    float dz=max(max(domain.y-p.y,p.y-domain.y-domain.z),0.0f);
    return (1.0f-smoothstep(0.0f,domain.w,dx))*(1.0f-smoothstep(0.0f,domain.w,dz));
}
