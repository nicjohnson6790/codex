float cloudHG(float mu,float g)
{
    return (1.0-g*g)/(12.56637061436*pow(max(1.0+g*g-2.0*g*mu,1e-5),1.5));
}
float cloudDualHG(float mu,float g,float weight) { return mix(cloudHG(mu,-g),cloudHG(mu,g),weight); }
float cloudMultipleScattering(float tau,float mu,float g,float weight,int count,vec3 abc)
{
    float result=0.0, a=1.0,b=1.0,c=1.0;
    for(int i=0;i<count;++i)
    {
        result+=b*cloudDualHG(mu,c*g,weight)*exp(-a*tau);
        a*=abc.x; b*=abc.y; c*=abc.z;
    }
    return result;
}
// cameraDirection points FROM camera INTO scene; sunDirection points TO sun.
// mu=-1 sees the illuminated face; mu=+1 looks toward the sun through the cloud.
float cloudPowder(float tau,float mu,float strength,float angularPower)
{
    float illuminated=pow(clamp(0.5-0.5*mu,0.0,1.0),angularPower);
    return mix(1.0,2.0*(1.0-exp(-2.0*tau)),strength*illuminated);
}
