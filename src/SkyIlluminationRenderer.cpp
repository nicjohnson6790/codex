#include "SkyIlluminationRenderer.hpp"
#include "PerformanceCapture.hpp"
#include <imgui.h>
#include <cstring>

namespace
{
constexpr unsigned kRegionsOffset=16;
constexpr unsigned kCandidatesOffset=kRegionsOffset+SkyIlluminationManager::kVisibleCapacity*sizeof(SkyIlluminationManager::Region);
constexpr unsigned kFrameBytes=kCandidatesOffset+SkyIlluminationManager::kVisibleCapacity*SkyIlluminationManager::kVisibleCapacity*sizeof(std::uint32_t);
}
void SkyIlluminationRenderer::initialize(SDL_GPUDevice* device,const std::filesystem::path& shaders)
{
    initializeRendererBase(device,SDL_GPU_TEXTUREFORMAT_INVALID,SDL_GPU_TEXTUREFORMAT_INVALID);
    m_manager.clear(); m_totalTileUpdates=0;
    try
    {
        SDL_GPUBufferCreateInfo bi{};
        bi.usage=SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ|SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE|SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        bi.size=SkyIlluminationManager::kCapacity*16*9*sizeof(glm::vec4);
        m_coefficients=SDL_CreateGPUBuffer(device,&bi);
        bi.usage=SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ; bi.size=kFrameBytes;
        m_regions=SDL_CreateGPUBuffer(device,&bi);
        SDL_GPUTransferBufferCreateInfo ti{}; ti.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; ti.size=kFrameBytes;
        m_upload=SDL_CreateGPUTransferBuffer(device,&ti);
        const auto code=readShaderCode(shaders/"sky_illumination.comp.spv");
        SDL_GPUComputePipelineCreateInfo ci{};
        ci.code=code.data(); ci.code_size=code.size(); ci.entrypoint="main"; ci.format=SDL_GPU_SHADERFORMAT_SPIRV;
        ci.num_samplers=3; ci.num_readonly_storage_buffers=1; ci.num_readwrite_storage_buffers=1;
        ci.num_uniform_buffers=1; ci.threadcount_x=256; ci.threadcount_y=ci.threadcount_z=1;
        m_pipeline=SDL_CreateGPUComputePipeline(device,&ci);
        if(!m_coefficients||!m_regions||!m_upload||!m_pipeline) throwSdlError("Sky illumination resources");
    }
    catch(...) { shutdown(); throw; }
}
void SkyIlluminationRenderer::shutdown()
{
    if(!m_device) return;
    if(m_pipeline) SDL_ReleaseGPUComputePipeline(m_device,m_pipeline);
    if(m_coefficients) SDL_ReleaseGPUBuffer(m_device,m_coefficients);
    if(m_regions) SDL_ReleaseGPUBuffer(m_device,m_regions);
    if(m_upload) SDL_ReleaseGPUTransferBuffer(m_device,m_upload);
    m_pipeline=nullptr; m_coefficients=m_regions=nullptr; m_upload=nullptr;
}
void SkyIlluminationRenderer::prepare(std::span<const SkyIlluminationManager::Tile> tiles,const Position& origin,double seconds)
{
    HELLO_PROFILE_SCOPE("SkyIlluminationRenderer::Prepare");
    tilesPerFrame=std::clamp(tilesPerFrame,1,int(SkyIlluminationManager::kMaxUpdates));
    m_manager.prepare(tiles,origin,unsigned(tilesPerFrame),seconds);
}
void SkyIlluminationRenderer::upload(SDL_GPUCopyPass* pass)
{
    auto* mapped=static_cast<std::byte*>(SDL_MapGPUTransferBuffer(m_device,m_upload,true));
    if(!mapped) throwSdlError("Sky illumination frame upload");
    const glm::uvec4 header(m_manager.regions().size(),m_manager.fallback(),0,0);
    std::memcpy(mapped,&header,sizeof(header));
    std::memcpy(mapped+kRegionsOffset,m_manager.regions().data(),m_manager.regions().size_bytes());
    std::memcpy(mapped+kCandidatesOffset,m_manager.candidates().data(),m_manager.candidates().size_bytes());
    SDL_UnmapGPUTransferBuffer(m_device,m_upload);
    SDL_GPUTransferBufferLocation source{}; source.transfer_buffer=m_upload;
    SDL_GPUBufferRegion destination{}; destination.buffer=m_regions;
    destination.size=kCandidatesOffset+Uint32(m_manager.candidates().size_bytes());
    SDL_UploadToGPUBuffer(pass,&source,&destination,true);
}
void SkyIlluminationRenderer::dispatch(SDL_GPUCommandBuffer* command,SDL_GPUBuffer* heights,
    const CloudRenderer::SamplingResources& clouds,const SkyboxRenderer& sky,const LightingSystem& lighting,int cloudBudget)
{
    HELLO_PROFILE_SCOPE_GROUPS("SkyIlluminationRenderer::Compute",ProfileScopeGroup::Renderer);
    if(m_manager.jobs().empty()) return;
    Uniforms u{}; u.cloud=clouds.state; u.cloud.march.w=float(std::clamp(cloudBudget,128,1024));
    u.skyRotation=lighting.skyboxRotationMatrix();
    std::copy(m_manager.jobs().begin(),m_manager.jobs().end(),u.jobs.begin());
    // Never cycle this persistent field: prior graphics reads, compute updates
    // and subsequent graphics reads are ordered on the same command queue.
    SDL_GPUStorageBufferReadWriteBinding output{}; output.buffer=m_coefficients; output.cycle=false;
    auto* pass=SDL_BeginGPUComputePass(command,nullptr,0,&output,1);
    if(!pass) throwSdlError("Sky illumination compute pass");
    SDL_BindGPUComputePipeline(pass,m_pipeline);
    const SDL_GPUTextureSamplerBinding samplers[]{{sky.cubemapTexture(),sky.cubemapSampler()},clouds.coverage,clouds.noise};
    SDL_BindGPUComputeSamplers(pass,0,samplers,3);
    SDL_BindGPUComputeStorageBuffers(pass,0,&heights,1);
    SDL_PushGPUComputeUniformData(command,0,&u,sizeof(u));
    SDL_DispatchGPUCompute(pass,16,Uint32(m_manager.jobs().size()),1);
    m_totalTileUpdates+=m_manager.jobs().size();
    SDL_EndGPUComputePass(pass);
}
void SkyIlluminationRenderer::drawSettings()
{
    if(!ImGui::CollapsingHeader("Sky illumination")) return;
    ImGui::SliderInt("Visible tiles updated/frame",&tilesPerFrame,1,int(SkyIlluminationManager::kMaxUpdates));
    ImGui::TextUnformatted("16 ground probes/tile, 256 upper-sky directions/probe");
    ImGui::Text("Visible tiles: %u; scheduled updates: %u",unsigned(m_manager.regions().size()),unsigned(m_manager.jobs().size()));
}
