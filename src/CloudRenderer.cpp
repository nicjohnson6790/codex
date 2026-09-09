#include "CloudRenderer.hpp"
#include "CloudSampling.hpp"
#include "PeriodicWorldPhase.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>

void CloudRenderer::initialize(SDL_GPUDevice* device,SDL_GPUTextureFormat color,SDL_GPUTextureFormat depth,const std::filesystem::path& shaders)
{
    initializeRendererBase(device,color,depth);
    m_macroTravel=m_macroEvolution=0;
    try
    {
        SDL_GPUTextureCreateInfo ti{};
        ti.type=SDL_GPU_TEXTURETYPE_2D; ti.format=SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
        ti.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER; ti.width=81; ti.height=81; ti.layer_count_or_depth=1;
        ti.num_levels=1; ti.sample_count=SDL_GPU_SAMPLECOUNT_1;
        m_macro=SDL_CreateGPUTexture(device,&ti);
        ti.type=SDL_GPU_TEXTURETYPE_3D; ti.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER|SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
        for(auto& dimension:m_settings.noiseDimensions) dimension=std::clamp(dimension,8,128);
        ti.width=m_settings.noiseDimensions[0]; ti.height=m_settings.noiseDimensions[1]; ti.layer_count_or_depth=m_settings.noiseDimensions[2];
        m_noise=SDL_CreateGPUTexture(device,&ti);
        // 256-byte aligned rows on all SDL backends. This is the only flattened storage.
        SDL_GPUTransferBufferCreateInfo transfer{};
        transfer.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; transfer.size=128*81*sizeof(float);
        m_upload=SDL_CreateGPUTransferBuffer(device,&transfer);
        SDL_GPUSamplerCreateInfo si{};
        si.min_filter=si.mag_filter=SDL_GPU_FILTER_LINEAR;
        si.address_mode_u=si.address_mode_v=si.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        m_linear=SDL_CreateGPUSampler(device,&si);
        si.address_mode_u=si.address_mode_v=si.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        m_repeat=SDL_CreateGPUSampler(device,&si);
        si.min_filter=si.mag_filter=SDL_GPU_FILTER_NEAREST;
        si.address_mode_u=si.address_mode_v=si.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        m_depth=SDL_CreateGPUSampler(device,&si);
        if(!m_macro||!m_noise||!m_upload||!m_linear||!m_repeat||!m_depth) throwSdlError("Cloud resources");

        auto code=readShaderCode(shaders/"cloud_noise.comp.spv");
        SDL_GPUComputePipelineCreateInfo ci{};
        ci.code=code.data(); ci.code_size=code.size(); ci.entrypoint="main"; ci.format=SDL_GPU_SHADERFORMAT_SPIRV;
        ci.num_readwrite_storage_textures=1; ci.threadcount_x=ci.threadcount_y=ci.threadcount_z=4;
        auto* compute=SDL_CreateGPUComputePipeline(device,&ci);
        if(!compute) throwSdlError("Cloud noise pipeline");
        auto* command=SDL_AcquireGPUCommandBuffer(device);
        if(!command) { SDL_ReleaseGPUComputePipeline(device,compute); throwSdlError("Cloud noise command"); }
        SDL_GPUStorageTextureReadWriteBinding binding{}; binding.texture=m_noise;
        auto* pass=SDL_BeginGPUComputePass(command,&binding,1,nullptr,0);
        if(!pass) { SDL_CancelGPUCommandBuffer(command); SDL_ReleaseGPUComputePipeline(device,compute); throwSdlError("Cloud noise pass"); }
        SDL_BindGPUComputePipeline(pass,compute);
        SDL_DispatchGPUCompute(pass,(ti.width+3)/4,(ti.height+3)/4,(ti.layer_count_or_depth+3)/4);
        SDL_EndGPUComputePass(pass);
        const bool submitted=SDL_SubmitGPUCommandBuffer(command);
        SDL_ReleaseGPUComputePipeline(device,compute);
        if(!submitted) throwSdlError("Cloud noise submission");

        auto* vs=createShader(shaders/"cloud.vert.spv",SDL_GPU_SHADERSTAGE_VERTEX,0);
        auto* fs=createShader(shaders/"cloud.frag.spv",SDL_GPU_SHADERSTAGE_FRAGMENT,1,0,3);
        SDL_GPUColorTargetDescription target{}; target.format=color;
        auto& blend=target.blend_state;
        blend.enable_blend=true; blend.src_color_blendfactor=SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_color_blendfactor=SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA; blend.color_blend_op=SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ZERO; blend.dst_alpha_blendfactor=SDL_GPU_BLENDFACTOR_ONE; blend.alpha_blend_op=SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo pi{};
        pi.vertex_shader=vs; pi.fragment_shader=fs; pi.primitive_type=SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pi.rasterizer_state.fill_mode=SDL_GPU_FILLMODE_FILL; pi.rasterizer_state.cull_mode=SDL_GPU_CULLMODE_NONE;
        pi.multisample_state.sample_count=SDL_GPU_SAMPLECOUNT_1;
        pi.target_info.num_color_targets=1; pi.target_info.color_target_descriptions=&target;
        m_pipeline=SDL_CreateGPUGraphicsPipeline(device,&pi);
        SDL_ReleaseGPUShader(device,vs); SDL_ReleaseGPUShader(device,fs);
        if(!m_pipeline) throwSdlError("Cloud graphics pipeline");
    }
    catch(...) { shutdown(); throw; }
}

void CloudRenderer::shutdown()
{
    if(m_pipeline) SDL_ReleaseGPUGraphicsPipeline(m_device,m_pipeline);
    if(m_upload) SDL_ReleaseGPUTransferBuffer(m_device,m_upload);
    for(auto* texture:{m_macro,m_noise}) if(texture) SDL_ReleaseGPUTexture(m_device,texture);
    for(auto* sampler:{m_linear,m_repeat,m_depth}) if(sampler) SDL_ReleaseGPUSampler(m_device,sampler);
    m_pipeline=nullptr; m_upload=nullptr; m_macro=m_noise=nullptr; m_linear=m_repeat=m_depth=nullptr;
    m_uploadedRevision=0;
}

void CloudRenderer::upload(SDL_GPUCopyPass* copy,const Position& origin,double time,const SkyboxRenderer& sky,const LightingSystem& lighting)
{
    m_activeCameraPosition=origin;
    auto& s=m_settings;
    // Settings are also available to callers outside the bounded ImGui controls.
    s.thickness=std::clamp(s.thickness,100.0f,10000.0f);
    s.coverage=std::clamp(s.coverage,0.0f,1.5f); s.density=std::clamp(s.density,0.0f,4.0f);
    s.baseNoiseScale=std::clamp(s.baseNoiseScale,0.01f,4.0f); s.detailNoiseScale=std::clamp(s.detailNoiseScale,0.01f,10.0f);
    s.baseStrength=std::clamp(s.baseStrength,0.0f,1.0f); s.detailStrength=std::clamp(s.detailStrength,0.0f,1.0f);
    s.erosion=std::clamp(s.erosion,0.0f,3.0f); s.extinction=std::clamp(s.extinction,0.0f,0.02f);
    s.anisotropy=std::clamp(s.anisotropy,0.0f,0.95f); s.lobeWeight=std::clamp(s.lobeWeight,0.0f,1.0f);
    s.powderStrength=std::clamp(s.powderStrength,0.0f,1.0f); s.powderAngularPower=std::clamp(s.powderAngularPower,0.1f,8.0f);
    s.octaveCount=std::clamp(s.octaveCount,1,12); s.viewSteps=std::clamp(s.viewSteps,8,128); s.sunSteps=std::clamp(s.sunSteps,1,32);
    s.octaveA=std::clamp(s.octaveA,0.0f,1.0f); s.octaveB=std::clamp(s.octaveB,0.0f,1.0f); s.octaveC=std::clamp(s.octaveC,0.0f,1.0f);
    s.maxDistance=std::clamp(s.maxDistance,10000.0f,500000.0f); s.termination=std::clamp(s.termination,0.001f,0.1f);
    const auto bounded=[](float value,float low,float high,float fallback) {
        return std::isfinite(value) ? std::clamp(value,low,high) : fallback;
    };
    s.macroDetailFrequency=bounded(s.macroDetailFrequency,0.0001f,0.1f,0.005f);
    s.macroDetailStrength=bounded(s.macroDetailStrength,0,1,1);
    s.frontDirection=bounded(s.frontDirection,-3.14159265f,3.14159265f,0);
    s.transverseStretch=bounded(s.transverseStretch,1,16,4);
    s.frontTravelSpeed=bounded(s.frontTravelSpeed,0,100,5);
    s.macroEvolutionSpeed=bounded(s.macroEvolutionSpeed,0,1,0.1f);
    s.topFalloffStart=bounded(s.topFalloffStart,0,0.95f,0.65f);
    s.topFalloffStrength=bounded(s.topFalloffStrength,0,16,2);
    const double dt=m_previousTime==0.0 ? 0.0 : std::max(0.0,time-m_previousTime); m_previousTime=time;
    const float macroFrequency=s.macroDetailFrequency*0.001f;
    const auto transform=cloudMacroTransform(macroFrequency,s.frontDirection,s.transverseStretch);
    advanceCloudMacroPhases(m_macroTravel,m_macroEvolution,s.frontTravelSpeed,
        macroFrequency,s.macroEvolutionSpeed,dt);
    const auto macroPhase=WorldPhase::periodicWorldPhase(origin,glm::dmat2(transform));
    m_field.macroTransform={transform[0][0],transform[1][0],transform[0][1],transform[1][1]};
    m_field.macroPhase={macroPhase.x,macroPhase.y,m_macroTravel,m_macroEvolution};
    m_field.macroShape={s.macroDetailStrength,s.topFalloffStart,s.topFalloffStrength,0};
    const glm::dvec2 velocity=glm::dvec2(std::cos(s.windDirection),std::sin(s.windDirection))*double(s.windSpeed);
    for(int axis=0;axis<2;++axis)
    {
        m_baseWind[axis]=WorldPhase::fract(m_baseWind[axis]-velocity[axis]*dt*double(s.baseNoiseScale*0.001f));
        m_detailWind[axis]=WorldPhase::fract(m_detailWind[axis]-velocity[axis]*dt*double(s.detailNoiseScale*0.001f));
    }
    const auto phase=[&](float frequency,glm::dvec2 wind)
    {
        // Reduce with the exact coefficient uploaded to GLSL, including float rounding.
        const double cycles=double(frequency*0.001f);
        auto p=WorldPhase::periodicWorldPhase(origin,cycles)+wind;
        return glm::vec4(WorldPhase::fract(p.x),WorldPhase::fract(origin.localPosition().y*cycles),WorldPhase::fract(p.y),cycles);
    };
    const auto center=m_manager.center();
    // Subtract integer grid coordinates before converting to float. No absolute XZ.
    const auto dx=std::int64_t(std::uint64_t(center.x)-std::uint64_t(origin.gridX()));
    const auto dz=std::int64_t(std::uint64_t(center.y)-std::uint64_t(origin.gridY()));
    m_field.layer={s.baseAltitude-origin.localPosition().y,s.thickness,s.coverage,s.density};
    m_field.macro={double(dx)*Position::kCellSize-2.0*Position::kCellSize-origin.localPosition().x,
        double(dz)*Position::kCellSize-2.0*Position::kCellSize-origin.localPosition().z,CloudManager::kPitch,81};
    m_field.basePhase=phase(s.baseNoiseScale,m_baseWind); m_field.detailPhase=phase(s.detailNoiseScale,m_detailWind);
    m_field.shape={s.baseStrength,s.detailStrength,s.erosion,0};
    s.waterSamplingMultiplier=std::clamp(s.waterSamplingMultiplier,0.1f,1.0f);
    m_prepared.field=m_field;
    m_waterSteps={waterCloudSampleCount(s.viewSteps,s.waterSamplingMultiplier),waterCloudSampleCount(s.sunSteps,s.waterSamplingMultiplier)};
    m_prepared.sun=glm::vec4(lighting.sunDirection(),0);
    m_prepared.atmosphere={m_activeCameraPosition.localPosition().y,sky.atmosphereSettings().atmosphereHeight,s.ambient,0.0f};
    m_prepared.optics=sky.buildAtmosphereOptics(lighting);
    m_prepared.scattering={s.extinction,s.anisotropy,s.lobeWeight,float(s.octaveCount)};
    m_prepared.powder={s.powderStrength,s.powderAngularPower,float(s.viewSteps),float(s.sunSteps)};
    m_prepared.march={s.maxDistance,s.termination,0,0}; m_prepared.octaves={s.octaveA,s.octaveB,s.octaveC,0};
    m_prepared.march.z=s.enabled && m_manager.revision()!=0 ? 1.0f : 0.0f;
    if(m_uploadedRevision==m_manager.revision()) return;
    auto* mapped=static_cast<float*>(SDL_MapGPUTransferBuffer(m_device,m_upload,true));
    if(!mapped) throwSdlError("Cloud macro mapping");
    m_manager.writeTexture({mapped,128*81},128);
    SDL_UnmapGPUTransferBuffer(m_device,m_upload);
    SDL_GPUTextureTransferInfo source{}; source.transfer_buffer=m_upload; source.pixels_per_row=128; source.rows_per_layer=81;
    SDL_GPUTextureRegion destination{}; destination.texture=m_macro; destination.w=81; destination.h=81; destination.d=1;
    SDL_UploadToGPUTexture(copy,&source,&destination,true);
    m_uploadedRevision=m_manager.revision();
}

CloudRenderer::SamplingResources CloudRenderer::waterSamplingResources() const
{
    SamplingResources result{{m_macro,m_linear},{m_noise,m_repeat},m_prepared};
    result.state.powder.z=m_waterSteps.x;
    result.state.powder.w=m_waterSteps.y;
    if(!m_uploadedRevision) result.state.march.z=0;
    return result;
}

void CloudRenderer::render(SDL_GPURenderPass* pass,SDL_GPUCommandBuffer* command,const glm::mat4& inverse,SDL_GPUTexture* depth)
{
    if(m_prepared.march.z<0.5f) return;
    Uniforms u{inverse,m_prepared};
    u.cloud.march.w=float(std::clamp(m_settings.fullscreenBudget,128,1024));
    SDL_BindGPUGraphicsPipeline(pass,m_pipeline);
    SDL_GPUTextureSamplerBinding samplers[]{{depth,m_depth},{m_macro,m_linear},{m_noise,m_repeat}};
    SDL_BindGPUFragmentSamplers(pass,0,samplers,3);
    SDL_PushGPUFragmentUniformData(command,0,&u,sizeof(u));
    SDL_DrawGPUPrimitives(pass,3,1,0,0);
}

void CloudRenderer::drawSettings()
{
    if(!ImGui::CollapsingHeader("Clouds")) return;
    ImGui::PushID("clouds");
    auto& s=m_settings;
    ImGui::Checkbox("Enabled",&s.enabled);
    ImGui::InputScalar("Seed",ImGuiDataType_U32,&s.seed);
    ImGui::Text("GPU noise: %d x %d x %d (startup configuration)",s.noiseDimensions[0],s.noiseDimensions[1],s.noiseDimensions[2]);
    ImGui::SliderFloat("Base altitude (m)",&s.baseAltitude,0,20000);
    ImGui::SliderFloat("Thickness (m)",&s.thickness,100,10000);
    ImGui::SliderFloat("Coverage",&s.coverage,0,1.5f);
    ImGui::SliderFloat("Density",&s.density,0,4);
    ImGui::SliderFloat("Macro detail frequency (cycles/km)",&s.macroDetailFrequency,0.0001f,0.1f,"%.4f",ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("Macro detail strength",&s.macroDetailStrength,0,1);
    ImGui::SliderAngle("Front direction",&s.frontDirection);
    ImGui::SliderFloat("Transverse stretch",&s.transverseStretch,1,16);
    ImGui::SliderFloat("Front travel speed (m/s)",&s.frontTravelSpeed,0,100);
    ImGui::SliderFloat("Macro evolution (cycles/hour)",&s.macroEvolutionSpeed,0,1);
    ImGui::SliderFloat("Top falloff start",&s.topFalloffStart,0,0.95f);
    ImGui::SliderFloat("Top falloff strength",&s.topFalloffStrength,0,16);
    ImGui::SliderFloat("Base frequency (cycles/km)",&s.baseNoiseScale,0.01f,4);
    ImGui::SliderFloat("Detail frequency (cycles/km)",&s.detailNoiseScale,0.01f,10);
    ImGui::SliderFloat("Base strength",&s.baseStrength,0,1);
    ImGui::SliderFloat("Detail strength",&s.detailStrength,0,1);
    ImGui::SliderFloat("Erosion",&s.erosion,0,3);
    ImGui::SliderAngle("Wind direction",&s.windDirection);
    ImGui::SliderFloat("Wind speed (m/s)",&s.windSpeed,0,150);
    ImGui::SliderFloat("Extinction (1/m)",&s.extinction,0,0.02f,"%.5f");
    ImGui::SliderFloat("HG anisotropy",&s.anisotropy,0,0.95f);
    ImGui::SliderFloat("Forward lobe weight",&s.lobeWeight,0,1);
    ImGui::SliderFloat("Powder strength",&s.powderStrength,0,1);
    ImGui::SliderFloat("Powder angular power",&s.powderAngularPower,0.1f,8);
    ImGui::SliderInt("Scattering octaves",&s.octaveCount,1,12);
    ImGui::SliderFloat("Octave a",&s.octaveA,0,1);
    ImGui::SliderFloat("Octave b",&s.octaveB,0,1);
    ImGui::SliderFloat("Octave c",&s.octaveC,0,1);
    ImGui::SliderInt("View samples",&s.viewSteps,8,128);
    ImGui::SliderInt("Fullscreen total sample budget",&s.fullscreenBudget,128,1024);
    ImGui::SliderInt("Sun samples",&s.sunSteps,1,32);
    ImGui::SliderFloat("Water sample multiplier",&s.waterSamplingMultiplier,0.1f,1.0f,"%.3f");
    ImGui::SliderFloat("Ambient",&s.ambient,0,1);
    ImGui::SliderFloat("Water cloud maximum distance (m)",&s.maxDistance,10000,500000);
    ImGui::SliderFloat("Transmittance termination",&s.termination,0.001f,0.1f,"%.3f");
    if(ImGui::Button("Reset clouds")) s=Settings{};
    ImGui::PopID();
}
