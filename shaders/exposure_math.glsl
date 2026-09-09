// Shared numerical policy: endpoints include black; 0.18 is pre-shoulder linear gray.
float exposureBinLog(int bin) { return -20.0f + float(bin) * (40.0f / 255.0f); }
float exposureRetained(float start, float count, float total)
{
    return max(0.0f, min(start+count,total*0.98f)-max(start,total*0.02f));
}
float exposureTarget(float meanLog, float compensation, float minimum, float maximum)
{
    return clamp(log2(0.18f)-meanLog+compensation,minimum,maximum);
}
float exposureAdapt(float previous, float target, float dt, float brightTime, float darkTime)
{
    float alpha=1.0f-exp(-clamp(dt,0.0f,0.1f)/(target<previous ? brightTime : darkTime));
    return previous+(target-previous)*alpha;
}
bool exposureFinite(float value) { return !isnan(value) && !isinf(value); }
int exposureBin(float luminance)
{
    if(!exposureFinite(luminance)) return -1;
    return int(round((clamp(log2(max(luminance,exp2(-20.0f))),-20.0f,20.0f)+20.0f)*255.0f/40.0f));
}
float exposureState(float previous,float seed,float total,float meanLog,vec4 range,vec3 timing)
{
    if(seed!=0.0f) previous=seed;
    if(!exposureFinite(previous)||previous<=0.0f) previous=1.0f;
    if(total==0.0f) return previous;
    float target=exposureTarget(meanLog,range.x,range.y,range.z);
    return exp2(seed<0.0f ? target : exposureAdapt(log2(previous),target,timing.x,timing.y,timing.z));
}
