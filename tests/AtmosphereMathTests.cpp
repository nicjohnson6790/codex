#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <glm/glm.hpp>

namespace ShaderMath
{
using namespace glm;
#include "../shaders/atmosphere.glsl"
#include "../shaders/display_transfer.glsl"
#include "../shaders/exposure_math.glsl"
#include "../shaders/authored_color.glsl"
#include "../shaders/water_medium.glsl"
#include "../shaders/water_interface.glsl"
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
    // Percentile trimming splits boundary bins, including fractional counts.
    near(exposureRetained(0,3,100),1);
    // Even the brightest cubemap texel rounded to black in the old UNORM path.
    assert(displayTransfer(0.0001f,1.0f)<0.5f/255.0f);
    assert(displayTransfer(0.02f,1.0f)>1.0f/255.0f);
    // Even the brightest cubemap texel rounded to black in the old UNORM path.
    assert(displayTransfer(0.0001f,1.0f)<0.5f/255.0f);
    assert(displayTransfer(0.02f,1.0f)>1.0f/255.0f);
    const float nan=std::numeric_limits<float>::quiet_NaN(), inf=std::numeric_limits<float>::infinity();
    assert(exposureBin(nan)==-1 && exposureBin(inf)==-1);
    assert(exposureBin(0)==0 && exposureBin(-1)==0 && exposureBin(1e30f)==255);
    vec4 exposureRange(0,-12,16,0); vec3 exposureTiming(0.1f,0.5f,2);
    near(exposureState(nan,-1,0,0,exposureRange,exposureTiming),1);
    near(exposureState(4,0,0,0,exposureRange,exposureTiming),4);
    near(exposureState(inf,0,0,0,exposureRange,exposureTiming),1);
    near(exposureState(nan,-1,100,-20,exposureRange,exposureTiming),65536);
    near(exposureState(nan,4,100,0,exposureRange,vec3(0,0.5f,2)),4);
    near(exposureRetained(97,3,100),1);
    near(exposureRetained(0,1,1),0.96);
    near(exposureRetained(20,30,100),30);
    near(exposureTarget(-20,0,-12,16),16); // black endpoint remains finite
    near(exposureTarget(20,0,-12,16),-12);
    near(exposureTarget(log2(0.18f),0,-12,16),0);
    near(exposureAdapt(2,-2,0,0.5f,2),2);
    near(exposureAdapt(2,-2,-1,0.5f,2),2);
    near(exposureAdapt(2,-2,100,0.5f,2),exposureAdapt(2,-2,0.1f,0.5f,2));
    assert(exposureAdapt(0,-2,0.1f,0.5f,2)<-exposureAdapt(0,2,0.1f,0.5f,2));
    near(exposureAdapt(exposureAdapt(0,2,0.05f,0.5f,2),2,0.05f,0.5f,2),exposureAdapt(0,2,0.1f,0.5f,2));
    AtmosphereOptics diskOptics{};
    diskOptics.solar=vec4(1); diskOptics.radianceScales=vec4(12,0.02f,100,0);
    near(directionalSkyRadiance(vec3(0),vec3(1),vec3(0),diskOptics,vec3(0,1,0),vec3(0,1,0),0,100000,0.0001f).x,100);
    near(directionalSkyRadiance(vec3(0),vec3(1),vec3(0),diskOptics,vec3(0,-1,0),vec3(0,-1,0),0,100000,0.0001f).x,0);
    near(linearSkyRadiance(vec3(0),vec3(1),vec3(0),diskOptics).x,0); // no disk in ambient
    constexpr float top = 85000.0f;
    // Compile the actual dielectric shader, checking Snell's law, critical angle,
    // unit rays, rotational invariance, and Fresnel energy over the hemisphere.
    const double critical = std::asin(1.0 / double(kWaterIor));
    near(critical * 180.0 / 3.141592653589793, 48.6066, 0.00001);
    float lastReflectance = 0.0f;
    for (int step = 0; step <= 9000; ++step)
    {
        const double angle = step * (3.141592653589793 / 18000.0);
        const glm::vec3 incident(float(std::sin(angle)), float(std::cos(angle)), 0.0f);
        const auto boundary = waterToAirInterface(incident, glm::vec3(0,-1,0));
        assert(std::isfinite(boundary.reflectance));
        assert(boundary.reflectance >= lastReflectance - 2e-6f);
        assert(boundary.reflectance <= 1.0f);
        lastReflectance = boundary.reflectance;
        near(glm::length(boundary.reflected), 1.0);
        near(boundary.reflected.y, -incident.y);
        if (angle >= critical)
        {
            near(boundary.reflectance, 1.0);
            near(glm::length(boundary.transmitted), 0.0);
        }
        else
        {
            near(glm::length(boundary.transmitted), 1.0);
            near(boundary.transmitted.x, kWaterIor * std::sin(angle));
            assert(boundary.transmitted.y > 0.0f);
        }
        // Same incidence on a tilted wave (orthogonal change of basis).
        const glm::vec3 n = glm::normalize(glm::vec3(0,-1,1));
        const auto tilted = waterToAirInterface(glm::vec3(incident.x,0,0)-n*incident.y,n);
        near(tilted.reflectance,boundary.reflectance,0.0001);
    }
    near(waterToAirInterface(glm::vec3(0,1,0),glm::vec3(0,-1,0)).reflectance,kWaterF0);
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
        glm::vec4(1.0f,0.97f,0.92f,4.8f),
        glm::vec4(12.0f,0.0001f,0,0)};
    // Surface sunlight and scattered sky must respond to the same source scales.
    // In vacuum, white Lambertian ground returns incident irradiance / pi.
    auto vacuum = optics;
    vacuum.rayleigh = glm::vec4(0,0,0,8000);
    vacuum.mie = glm::vec4(0,0,1200,0);
    vacuum.ozone = glm::vec4(0,0,0,25000);
    const auto direct = surfaceSunIrradiance(0,glm::vec3(0,1,0),top,vacuum);
    for(int c=0;c<3;++c) near(direct[c],optics.solar[c]*4.8*12);
    near(glm::length(surfaceSunIrradiance(0,glm::vec3(0,-1,0),top,vacuum)),0);
    auto doubled = optics; doubled.radianceScales.x *= 2;
    const auto groundSun = surfaceSunIrradiance(0,glm::vec3(0,1,0),top,optics);
    const auto scaledSun = surfaceSunIrradiance(0,glm::vec3(0,1,0),top,doubled);
    const auto highSun = surfaceSunIrradiance(4000,glm::vec3(0,1,0),top,optics);
    for(int c=0;c<3;++c) { near(scaledSun[c],2*groundSun[c]); assert(highSun[c]>=groundSun[c]); }
    // Infinite reflected-ray radiance is the limit of the same finite camera
    // medium, with zero starting depth. Zero coefficients and night stay finite.
    const glm::vec3 absorption(0.15f,0.05f,0.02f);
    const glm::vec4 scatter(0.006f,0.007f,0.013f,4.0f);
    const glm::vec3 sun(0,1,0);
    const auto infiniteWater = waterMediumRadiance(0,0,0,true,sun,top,optics,absorption,scatter);
    const auto finiteWater = waterMediumRadiance(0,0,1e6f,false,sun,top,optics,absorption,scatter);
    for (int channel=0; channel<3; ++channel)
    {
        near(infiniteWater[channel],finiteWater[channel]);
        const double expected = optics.solar[channel] * airSunTransmission(0,sun,top,optics)[channel]
            * (scatter.w / (4.0 * 3.141592653589793))
            * scatter[channel] / (absorption[channel]+scatter[channel]);
        near(infiniteWater[channel],expected);
    }
    near(glm::length(waterMediumRadiance(0,0,0,true,sun,top,optics,glm::vec3(0),glm::vec4(0))),0);
    near(glm::length(waterMediumRadiance(0,0,0,true,-sun,top,optics,absorption,scatter)),0);
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
    glm::vec3 dayT, dayS;
    const glm::vec3 direction = glm::normalize(glm::vec3(0,0.8f,-0.6f));
    evaluateAtmosphere(300, direction, 4000000, top, {0,1,0}, optics, true, dayT, dayS);
    auto uncalibrated = optics;
    uncalibrated.radianceScales.x = 1;
    glm::vec3 rawT, rawS;
    evaluateAtmosphere(300, direction, 4000000, top, {0,1,0}, uncalibrated, true, rawT, rawS);
    assert(rawT == dayT);
    for (int c=0; c<3; ++c)
    {
        near(dayS[c], rawS[c] * 12);
        near(linearSkyRadiance(glm::vec3(0.5f), dayT, dayS, optics)[c],
            0.5f * 0.0001f * dayT[c] + dayS[c]);
    }
    // Night and vacuum retain only the separate space source, without adaptation.
    near(linearSkyRadiance(glm::vec3(0.5f), glm::vec3(1), glm::vec3(0), optics).r, 0.00005, 1e-8);
    near(encodeDisplaySrgb(0.0031308f), 0.040449936, 1e-7);
    near(decodeAuthoredSrgb(0.04045f), 0.003130805, 1e-7);
    near(decodeAuthoredSrgb(0.5f), 0.21404114, 1e-7);
    for (float exposure : {0.0f, 0.01f, 1.0f, 100.0f, 3.4e38f})
    {
        near(displayTransfer(0, exposure), 0);
        float previous = 0;
        for (float radiance : {0.0f, 1e-8f, 0.001f, 0.1f, 1.0f, 10.0f, 65504.0f, 3.4e38f})
        {
            float value = displayTransfer(radiance, exposure);
            assert(std::isfinite(value) && value >= 0 && value <= 1);
            assert(value >= previous);
            previous = value;
        }
    }
    for (float r : {0.0f, 0.01f, 0.5f, 2.0f, 100.0f})
        near(displayTransfer(r, 2), displayTransfer(r * 2, 1));
    assert(displayTransfer(2, 1) > displayTransfer(1, 1));
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
        reference *= glm::dvec3(optics.solar)*double(optics.solar.w)*double(optics.radianceScales.x);
        const double error = glm::length(glm::dvec3(scatter)-reference)/std::max(glm::length(reference),0.01);
        if(error>0.1) std::cout << "Quadrature error h=" << h << " dy=" << dy << " length=" << distance << ": " << error << '\n';
        maxError=std::max(maxError,error);
    }
    std::cout << "Maximum relative scattering error: " << maxError << '\n';
    if (maxError >= 0.05) return 1;
    std::cout << "Atmosphere interval and shared shader column-density tests passed\n";
}
