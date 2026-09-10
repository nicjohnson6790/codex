#include "DisplayTransformRenderer.hpp"
#include "PerformanceCapture.hpp"
#include <algorithm>
#include <cmath>
#include <glm/vec4.hpp>
#include <imgui.h>

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
        SDL_GPUBufferCreateInfo bi{};
        bi.usage=SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE|SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
        bi.size=256*sizeof(unsigned);
        m_histogram=SDL_CreateGPUBuffer(device,&bi);
        bi.usage|=SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        bi.size=2*sizeof(float);
        m_state=SDL_CreateGPUBuffer(device,&bi);
        if(!m_histogram||!m_state) throwSdlError("Exposure buffers");
        const char* names[]{"exposure_clear.comp.spv","exposure_histogram.comp.spv","exposure_reduce.comp.spv"};
        for(int i=0;i<3;++i) {
            auto code=readShaderCode(shaders/names[i]);
            SDL_GPUComputePipelineCreateInfo ci{};
            ci.code=code.data(); ci.code_size=code.size(); ci.entrypoint="main"; ci.format=SDL_GPU_SHADERFORMAT_SPIRV;
            ci.threadcount_x=i==2 ? 1 : 256; ci.threadcount_y=ci.threadcount_z=1;
            ci.num_readwrite_storage_buffers=1; ci.num_samplers=i==1 ? 1 : 0;
            ci.num_readonly_storage_buffers=i==2 ? 1 : 0; ci.num_uniform_buffers=i==0 ? 0 : 1;
            m_compute[i]=SDL_CreateGPUComputePipeline(device,&ci);
            if(!m_compute[i]) throwSdlError("Exposure compute pipeline");
        }
        SDL_GPUSamplerCreateInfo si{};
        si.min_filter = si.mag_filter = SDL_GPU_FILTER_NEAREST;
        si.address_mode_u = si.address_mode_v = si.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        m_sampler = SDL_CreateGPUSampler(device, &si);
        if (!m_sampler) throwSdlError("Display sampler creation failed.");
        vs = createShader(shaders / "cloud.vert.spv", SDL_GPU_SHADERSTAGE_VERTEX, 0);
        fs = createShader(shaders / "display_transform.frag.spv", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1, 1);
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
    for(auto*& pipeline:m_compute) { if(pipeline) SDL_ReleaseGPUComputePipeline(m_device,pipeline); pipeline=nullptr; }
    if(m_histogram) SDL_ReleaseGPUBuffer(m_device,m_histogram);
    if(m_state) SDL_ReleaseGPUBuffer(m_device,m_state);
    m_histogram=m_state=nullptr; m_initialized=false; m_wasAutomatic=true;
    if (m_pipeline) SDL_ReleaseGPUGraphicsPipeline(m_device, m_pipeline);
    if (m_sampler) SDL_ReleaseGPUSampler(m_device, m_sampler);
    m_pipeline = nullptr; m_sampler = nullptr;
}

void DisplayTransformRenderer::render(SDL_GPURenderPass* pass, SDL_GPUCommandBuffer* command, SDL_GPUTexture* scene)
{
    HELLO_PROFILE_SCOPE_GROUPS("DisplayTransformRenderer::Render", ProfileScopeGroup::Renderer);
    exposure = std::isfinite(exposure) ? std::max(exposure, 0.0f) : 1.0f;
    const glm::vec4 settings(exposure, automatic ? 1.0f : 0.0f, 0, 0);
    SDL_PushGPUFragmentUniformData(command, 0, &settings, sizeof(settings));
    SDL_BindGPUGraphicsPipeline(pass, m_pipeline);
    const SDL_GPUTextureSamplerBinding binding{scene, m_sampler};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_BindGPUFragmentStorageBuffers(pass,0,&m_state,1);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
}

void DisplayTransformRenderer::meter(SDL_GPUCommandBuffer* command,SDL_GPUTexture* scene,unsigned width,unsigned height)
{
    HELLO_PROFILE_SCOPE_GROUPS("DisplayTransformRenderer::Meter", ProfileScopeGroup::Renderer);
    if(!width||!height) return;
    auto sanitize=[](float v,float fallback,float low,float high) { return std::isfinite(v) ? std::clamp(v,low,high) : fallback; };
    exposure=sanitize(exposure,1,0,65536);
    compensationEV=sanitize(compensationEV,0,-24,24);
    minimumEV=sanitize(minimumEV,-8,-20,16); maximumEV=sanitize(maximumEV,6,minimumEV,20);
    brightenSceneSeconds=sanitize(brightenSceneSeconds,0.5f,0.01f,60);
    darkenSceneSeconds=sanitize(darkenSceneSeconds,2,0.01f,60);
    deltaSeconds=sanitize(deltaSeconds,0,0,0.1f);
    if(!automatic) { m_wasAutomatic=false; return; }
    float scale=std::min({1.0f,256.0f/width,144.0f/height});
    glm::vec4 grid(std::max(1.0f,std::floor(width*scale)),std::max(1.0f,std::floor(height*scale)),0,0);
    struct Uniforms { glm::vec4 range,timing; } u{
        {compensationEV,minimumEV,maximumEV,!m_wasAutomatic ? std::max(exposure,1e-20f) : (!m_initialized ? -1.0f : 0.0f)},
        {deltaSeconds,brightenSceneSeconds,darkenSceneSeconds,0}};
    static_assert(sizeof(Uniforms)==32);
    // No cycling: queue-ordered passes serialize access to the persistent state,
    // including the previous frame's fragment read. Each dispatch has its own pass.
    for(int i=0;i<3;++i) {
        SDL_GPUStorageBufferReadWriteBinding output{}; output.buffer=i==2 ? m_state : m_histogram;
        auto* pass=SDL_BeginGPUComputePass(command,nullptr,0,&output,1);
        if(!pass) throwSdlError("Exposure compute pass");
        SDL_BindGPUComputePipeline(pass,m_compute[i]);
        if(i==1) {
            SDL_GPUTextureSamplerBinding binding{scene,m_sampler};
            SDL_BindGPUComputeSamplers(pass,0,&binding,1);
            SDL_PushGPUComputeUniformData(command,0,&grid,sizeof(grid));
        }
        if(i==2) {
            SDL_BindGPUComputeStorageBuffers(pass,0,&m_histogram,1);
            SDL_PushGPUComputeUniformData(command,0,&u,sizeof(u));
        }
        SDL_DispatchGPUCompute(pass,i==1 ? (unsigned(grid.x*grid.y)+255)/256 : 1,1,1);
        SDL_EndGPUComputePass(pass);
    }
    m_initialized=true; m_wasAutomatic=true;
}

void DisplayTransformRenderer::drawSettings()
{
    ImGui::Checkbox("Automatic exposure",&automatic);
    ImGui::InputFloat("Manual exposure",&exposure,0.1f,0.5f);
    ImGui::SliderFloat("Exposure compensation (EV)",&compensationEV,-12,12);
    ImGui::InputFloat("Minimum exposure (EV)",&minimumEV);
    ImGui::InputFloat("Maximum exposure (EV)",&maximumEV);
    ImGui::SliderFloat("Bright scene adaptation (s)",&brightenSceneSeconds,0.01f,10);
    ImGui::SliderFloat("Dark scene adaptation (s)",&darkenSceneSeconds,0.01f,10);
}
