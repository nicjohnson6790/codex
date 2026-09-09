#include "DisplayTransformRenderer.hpp"
#include <algorithm>
#include <cmath>
#include <glm/vec4.hpp>

void DisplayTransformRenderer::initialize(SDL_GPUDevice* device, SDL_GPUTextureFormat sourceFormat,
    SDL_GPUTextureFormat displayFormat, const std::filesystem::path& shaders)
{
    initializeRendererBase(device, displayFormat, SDL_GPU_TEXTUREFORMAT_INVALID);
    if (!SDL_GPUTextureSupportsFormat(device, sourceFormat, SDL_GPU_TEXTURETYPE_2D,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER))
        throwSdlError("Unsupported HDR scene format.");
    SDL_GPUShader* vs = nullptr;
    SDL_GPUShader* fs = nullptr;
    try
    {
        SDL_GPUSamplerCreateInfo si{};
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        m_sampler = SDL_CreateGPUSampler(device, &si);
        if (!m_sampler) throwSdlError("Display sampler creation failed.");
        vs = createShader(shaders / "cloud.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        fs = createShader(shaders / "display_transform.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, 1);
        SDL_GPUColorTargetDescription target{};
        target.format = displayFormat; // Ordinary UNORM; shader encodes exactly once.
        SDL_GPUGraphicsPipelineCreateInfo pi{};
        pi.vertex_shader = vs; pi.fragment_shader = fs;
        pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pi.target_info.num_color_targets = 1;
        pi.target_info.color_target_descriptions = &target;
        m_pipeline = SDL_CreateGPUGraphicsPipeline(device, &pi);
        if (!m_pipeline) throwSdlError("Display pipeline creation failed.");
    }
    catch (...)
    {
        if (vs) SDL_ReleaseGPUShader(device, vs);
        if (fs) SDL_ReleaseGPUShader(device, fs);
        shutdown();
        throw;
    }
    SDL_ReleaseGPUShader(device, vs);
    SDL_ReleaseGPUShader(device, fs);
}

void DisplayTransformRenderer::shutdown()
{
    if (m_pipeline) SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
    if (m_sampler) SDL_ReleaseGPUSampler(m_device, m_sampler);
    m_pipeline = nullptr; m_sampler = nullptr;
}

void DisplayTransformRenderer::render(SDL_GPURenderPass* pass, SDL_GPUCommandBuffer* command, SDL_GPUTexture* scene)
{
    exposure = std::isfinite(exposure) ? std::max(exposure, 0.0f) : 1.0f;
    const glm::vec4 settings(exposure, 0, 0, 0);
    SDL_PushGPUFragmentUniformData(command, 0, &settings, sizeof(settings));
    SDL_BindGPUGraphicsPipeline(pass, m_pipeline);
    const SDL_GPUTextureSamplerBinding binding{scene, m_sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}
