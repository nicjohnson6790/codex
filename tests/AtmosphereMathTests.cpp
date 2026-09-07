#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <glm/glm.hpp>

namespace ShaderMath
{
using namespace glm;
#include "../shaders/atmosphere.glsl"
#include "../shaders/water_medium.glsl"
}

namespace
{
void near(double actual, double expected, double relative = 3.0e-5)
{
    if (!std::isfinite(actual) || std::abs(actual-expected) > relative * std::max(1.0, std::abs(expected)))
    {
        std::cerr << "actual " << actual << ", expected " << expected << '\n';
        std::abort();
    }
}

// Independent double-precision midpoint reference (not the closed-form implementation).
double numericalColumn(double height, double dy, double length, double scale)
{
    constexpr int steps = 32768;
    double sum = 0.0;
    for (int i = 0; i < steps; ++i)
        sum += std::exp(-std::max(height + dy * length * (i + 0.5) / steps, 0.0) / scale);
    return sum * length / steps;
}
}

int main()
{
    using namespace ShaderMath;
    constexpr float top = 85000.0f;
    near(atmosphereEntry(top+1000, -1, 3000, top), 1000);
    near(atmosphereExit(top+1000, -1, 3000, top), 3000);
    near(atmosphereEntry(top+1000, -1, 500, top), 500); // geometry before entry
    for (float dy : {0.0f, 1.0e-9f, 1.0f})
        near(atmosphereEntry(top+1,dy,10000,top), atmosphereExit(top+1,dy,10000,top));
    near(atmosphereExit(1000,1,100000,top),84000);
    near(atmosphereExit(top,1,1000,top),0);
    near(atmosphereExit(top,-1,1000,top),1000);
    near(atmosphereExit(top,0,1000,top),1000);
    near(atmosphereEntry(top,-1,1000,top),0);
    near(atmosphereColumn(-1000,0,200000,1200),200000);
    near(atmosphereColumn(0,0,0,1200),0);
    near(atmosphereColumn(8000,0,1000,8000),1000/std::exp(1.0));
    near(atmosphereColumn(0,1,85000,8000),8000*(1-std::exp(-85000.0/8000)));

    for (float scale : {1200.0f,8000.0f})
    for (float height : {-2000.0f,0.0f,1000.0f,20000.0f,85000.0f})
    for (float dy : {-1.0f,-0.01f,-1.0e-8f,0.0f,1.0e-8f,0.01f,1.0f})
    for (float length : {0.0f,0.01f,100.0f,10000.0f,4000000.0f})
    {
        const float column = atmosphereColumn(height,dy,length,scale);
        assert(column >= 0 && column <= length*1.00001f);
        near(column,numericalColumn(height,dy,length,scale),0.001);
        if (column > 1.0e-4f && std::abs(dy)*length/scale < 10.0f)
            near(atmosphereColumnDistance(height,dy,column,scale),length,0.003);
        // Integrals are additive across both exponential and clamped sea-level sections.
        const float split = length*0.37f;
        near(column,atmosphereColumn(height,dy,split,scale)
            + atmosphereColumn(height+dy*split,dy,length-split,scale),0.0004);
    }
    // Extinction at high altitude and outside the top must be lower.
    assert(atmosphereColumn(24000,0,10000,8000) < atmosphereColumn(0,0,10000,8000)*0.051f);
    near(waterScatteringIntegral(0,0,1000),0);
    near(waterScatteringIntegral(0.01f,0.02f,0),0);
    near(waterScatteringIntegral(0.01f,0.02f,100),0.5*(1-std::exp(-2.0)));
    near(waterScatteringIntegral(0.01f,0.02f,0.0001f),0.000001,1e-8);
    const glm::vec3 sigmaT(0.156f,0.057f,0.033f);
    const auto waterT = glm::exp(-sigmaT*20.0f);
    assert(waterT.r < waterT.g && waterT.g < waterT.b);
    assert(glm::all(glm::lessThan(glm::exp(-sigmaT*100.0f),waterT)));
    for (float sunY : {-1.0f,0.0f,1e-10f,0.001f,0.5f,1.0f})
    {
        const float path = 20.0f/std::max(sunY,1e-7f);
        const glm::vec3 illumination = sunY > 0 ? glm::exp(-sigmaT*path) : glm::vec3(0);
        assert(!glm::any(glm::isnan(illumination)));
        assert(illumination.r <= illumination.g && illumination.g <= illumination.b);
    }
    AtmosphereOptics optics{
        glm::vec4(7.4e-6f,17.8e-6f,45.5e-6f,8000.0f),
        glm::vec4(1.7e-6f,4.2e-6f,1200.0f,0.88f),
        glm::vec4(0.650e-6f,1.881e-6f,0.085e-6f,25000.0f),
        glm::vec4(1.0f,0.97f,0.92f,4.8f)};
    for (float h : {-1000.0f,0.0f,top,top+0.1f,100000.0f})
    for (float dy : {-1.0f,-1e-8f,0.0f,1e-8f,1.0f})
    for (float sunY : {-1.0f,0.0f,1e-10f,1.0f})
    for (float distance : {0.0f,1000.0f,4000000.0f})
    {
        glm::vec3 t, scatter;
        evaluateAtmosphere(h,{std::sqrt(1-dy*dy),dy,0},distance,top,
            {std::sqrt(1-sunY*sunY),sunY,0},optics,true,t,scatter);
        assert(!glm::any(glm::isnan(t)) && !glm::any(glm::isinf(t)));
        assert(!glm::any(glm::isnan(scatter)) && !glm::any(glm::isinf(scatter)));
        assert(glm::all(glm::greaterThanEqual(t,glm::vec3(0))) && glm::all(glm::lessThanEqual(t,glm::vec3(1))));
        if (distance==0 || (h>top && dy>=0))
        {
            assert(t==glm::vec3(1));
            assert(scatter==glm::vec3(0));
        }
        if (sunY<=0) assert(scatter==glm::vec3(0));
    }
    optics.skyDisplay = glm::vec4(12.0f,0.0001f,1.0f,0.0001f);
    glm::vec3 dayT, dayS;
    evaluateAtmosphere(300.0f,glm::normalize(glm::vec3(0,0.8f,-0.6f)),4000000,top,
        {0,1,0},optics,true,dayT,dayS);
    const auto blueSky = displaySkyRadiance(glm::vec3(0),dayT,dayS,optics);
    const auto withStars = displaySkyRadiance(glm::vec3(1),dayT,dayS,optics);
    // The air supplies a bright blue sky even with a completely black background.
    assert(blueSky.b > blueSky.g && blueSky.g > blueSky.r && blueSky.b > 0.5f);
    assert(glm::length(withStars-blueSky) < 0.002f);
    glm::vec3 nightT, nightS, spaceT, spaceS;
    evaluateAtmosphere(300,{0,1,0},4000000,top,{0,-1,0},optics,true,nightT,nightS);
    evaluateAtmosphere(100000,{0,1,0},4000000,top,{0,1,0},optics,true,spaceT,spaceS);
    assert(glm::length(displaySkyRadiance(glm::vec3(0.5f),nightT,nightS,optics)) > 0.3f);
    assert(glm::length(displaySkyRadiance(glm::vec3(0.5f),spaceT,spaceS,optics)) > 0.3f);
    // No hard day/night switch: the radiance-to-display curve stays finite down to darkness.
    glm::vec3 previous = displaySkyRadiance(glm::vec3(0.5f),dayT,glm::vec3(0),optics);
    for(int i=1;i<=1000;++i)
    {
        auto color = displaySkyRadiance(glm::vec3(0.5f),dayT,dayS*std::pow(10.0f,-14.0f+float(i)*0.012f),optics);
        assert(!glm::any(glm::isnan(color)) && !glm::any(glm::isinf(color)));
        assert(glm::length(color-previous) < 0.25f);
        previous = color;
    }
    std::cout << "Day sky RGB: " << blueSky.r << ", " << blueSky.g << ", " << blueSky.b << '\n';
    // Independent high-count midpoint radiance reference, only in the test.
    double maxError = 0.0;
    for (float sunY : {0.001f,0.05f,0.8f})
    for (float h : {0.0f, 24000.0f, 86000.0f})
    for (float dy : {-1.0f,-0.1f,0.0f,0.1f,1.0f})
    for (float distance : {1000.0f,100000.0f,4000000.0f})
    {
        glm::vec3 dir(std::sqrt(1-dy*dy),dy,0);
        glm::vec3 sun(std::sqrt(1-sunY*sunY),sunY,0.0f);
        glm::vec3 t, scatter;
        evaluateAtmosphere(h,dir,distance,top,sun,optics,true,t,scatter);
        assert(!glm::any(glm::isnan(scatter)));
        const float begin = atmosphereEntry(h,dy,distance,top);
        const float end = atmosphereExit(h,dy,distance,top);
        const float length = std::max(end-begin,0.0f);
        glm::dvec3 reference(0.0);
        const float mu = glm::dot(dir,sun);
        const float phaseR = 3.0f/(16.0f*3.14159265359f)*(1+mu*mu);
        const float g = optics.mie.w;
        const float phaseM = (1-g*g)/(4*3.14159265359f*std::pow(1+g*g-2*g*mu,1.5f));
        constexpr int steps = 65536;
        for(int i=0;i<steps && length>0;++i)
        {
            const float s = length*(i+0.5f)/steps;
            const float startHeight = h+dy*begin;
            const float sampleHeight = startHeight+dy*s;
            const glm::vec3 viewT = glm::exp(-airOpticalDepth(startHeight,dy,s,top,optics));
            const glm::vec3 source = glm::vec3(optics.rayleigh)* (std::exp(-std::max(sampleHeight,0.0f)/8000)*phaseR)
                + glm::vec3(optics.mie.x*std::exp(-std::max(sampleHeight,0.0f)/1200)*phaseM);
            reference += glm::dvec3(viewT*airSunTransmission(sampleHeight,sun,top,optics)*source)*(double(length)/steps);
        }
        reference *= glm::dvec3(optics.solar)*double(optics.solar.w);
        const double error = glm::length(glm::dvec3(scatter)-reference)/std::max(glm::length(reference),0.01);
        if(error>0.1) std::cout << "Quadrature error h=" << h << " dy=" << dy << " length=" << distance << ": " << error << '\n';
        maxError=std::max(maxError,error);
    }
    std::cout << "Maximum relative scattering error: " << maxError << '\n';
    if (maxError >= 0.05) return 1;
    std::cout << "Atmosphere interval and shared shader column-density tests passed\n";
}
