#pragma once
#include "SkyIlluminationRenderer.hpp"
#include <cmath>

// Borrowed frame resources; no scheduling or GPU ownership lives here.
struct FoliageLighting
{
    CloudRenderer::SamplingResources clouds;
    SkyIlluminationRenderer::SamplingResources sky;
    int samples;
    struct Uniforms {
        CloudRenderer::DensityUniforms field;
        glm::vec4 params;
        SkyboxRenderer::AtmosphereOptics optics;
        glm::vec4 atmosphere;
        glm::vec4 solar;
    };
    static_assert(sizeof(Uniforms)==256 && offsetof(Uniforms,optics)==144 && offsetof(Uniforms,atmosphere)==224);
    void bind(SDL_GPURenderPass* pass,SDL_GPUCommandBuffer* commands,unsigned samplerSlot,unsigned storageSlot) const
    {
        SDL_GPUTextureSamplerBinding textures[]{clouds.coverage,clouds.noise};
        SDL_BindGPUFragmentSamplers(pass,samplerSlot,textures,2);
        SDL_GPUBuffer* buffers[]{sky.coefficients,sky.regions};
        SDL_BindGPUFragmentStorageBuffers(pass,storageSlot,buffers,2);
        const Uniforms uniforms{clouds.state.field,{clouds.state.scattering.x,clouds.state.march.y,
            clouds.state.march.z,float(std::clamp(samples,1,32))},clouds.state.optics,clouds.state.atmosphere,
            {std::sin(AppConfig::Terrain::kSolarHorizonFadeDegrees*3.14159265359f/180.0f),0,0,0}};
        SDL_PushGPUFragmentUniformData(commands,1,&uniforms,sizeof(uniforms));
    }
};
