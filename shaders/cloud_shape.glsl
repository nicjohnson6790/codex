// Pure density shaping math, also compiled by the focused CPU cloud tests.
float cloudMacroCoverage(float macro, float detail, float strength, float offset)
{
    return clamp(macro*mix(1.0,detail,strength)+offset-0.5,0.0,1.0);
}

float cloudTopFalloff(float h, float start, float strength)
{
    float a=clamp(start,0.0,0.95);
    float u=clamp((h-a)/(1.0-a),0.0,1.0);
    return exp(-max(strength,0.0)*u*u)*(1.0-smoothstep(0.0,1.0,u));
}
