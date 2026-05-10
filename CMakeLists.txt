cmake_minimum_required(VERSION 3.25)

project(TerrainSandbox LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

include(FetchContent)

find_program(VULKAN_GLSLC_EXECUTABLE
    NAMES glslc
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/Bin32"
)

if (NOT VULKAN_GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc was not found. Install the Vulkan SDK and ensure glslc is on PATH.")
endif()

FetchContent_Declare(
    SDL3
    URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.0.zip
)

FetchContent_Declare(
    imgui
    URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.5-docking.zip
)

FetchContent_Declare(
    glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.zip
)

FetchContent_Declare(
    lz4
    URL https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.zip
)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_LIBRARY OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3 imgui glm lz4)

add_library(lz4_local STATIC
    ${lz4_SOURCE_DIR}/lib/lz4.c
)

target_include_directories(lz4_local
    PUBLIC
        ${lz4_SOURCE_DIR}/lib
)

add_library(imgui_backend STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlgpu3.cpp
)

target_include_directories(imgui_backend
    PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(imgui_backend
    PUBLIC
        SDL3::SDL3
)

set(SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders)
set(RUNTIME_ASSET_SOURCE_DIR ${CMAKE_SOURCE_DIR}/assets/runtime)
set(RUNTIME_ASSET_OUTPUT_DIR ${CMAKE_BINARY_DIR}/assets/runtime)
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

function(compile_shader SOURCE_FILE OUTPUT_FILE)
    add_custom_command(
        OUTPUT ${OUTPUT_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUTPUT_DIR}
        COMMAND ${VULKAN_GLSLC_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/shaders -o ${OUTPUT_FILE} ${SOURCE_FILE}
        DEPENDS ${SOURCE_FILE} ${ARGN}
        VERBATIM
    )
endfunction()

set(TRIANGLE_VERT_SPV ${SHADER_OUTPUT_DIR}/triangle.vert.spv)
set(TRIANGLE_FRAG_SPV ${SHADER_OUTPUT_DIR}/triangle.frag.spv)
set(LINE_VERT_SPV ${SHADER_OUTPUT_DIR}/line.vert.spv)
set(LINE_FRAG_SPV ${SHADER_OUTPUT_DIR}/line.frag.spv)
set(QUADTREE_MESH_VERT_SPV ${SHADER_OUTPUT_DIR}/quadtree_mesh.vert.spv)
set(QUADTREE_MESH_BRIDGE_VERT_SPV ${SHADER_OUTPUT_DIR}/quadtree_mesh_bridge.vert.spv)
set(QUADTREE_MESH_FRAG_SPV ${SHADER_OUTPUT_DIR}/quadtree_mesh.frag.spv)
set(HEIGHTMAP_GENERATE_COMP_SPV ${SHADER_OUTPUT_DIR}/heightmap_generate.comp.spv)
set(FOLIAGE_GENERATE_COMP_SPV ${SHADER_OUTPUT_DIR}/foliage_generate.comp.spv)
set(FOLIAGE_CANOPY_VERT_SPV ${SHADER_OUTPUT_DIR}/foliage_canopy.vert.spv)
set(FOLIAGE_CANOPY_FRAG_SPV ${SHADER_OUTPUT_DIR}/foliage_canopy.frag.spv)
set(FOLIAGE_CANOPY_GENERATE_COMP_SPV ${SHADER_OUTPUT_DIR}/foliage_canopy_generate.comp.spv)
set(SKYBOX_VERT_SPV ${SHADER_OUTPUT_DIR}/skybox.vert.spv)
set(SKYBOX_FRAG_SPV ${SHADER_OUTPUT_DIR}/skybox.frag.spv)
set(FOLIAGE_IMPOSTER_VERT_SPV ${SHADER_OUTPUT_DIR}/foliage_imposter.vert.spv)
set(FOLIAGE_IMPOSTER_DEPTH_PREPASS_FRAG_SPV ${SHADER_OUTPUT_DIR}/foliage_imposter_depth_prepass.frag.spv)
set(FOLIAGE_IMPOSTER_FRAG_SPV ${SHADER_OUTPUT_DIR}/foliage_imposter.frag.spv)
set(NEARBY_FOLIAGE_VERT_SPV ${SHADER_OUTPUT_DIR}/nearby_foliage.vert.spv)
set(NEARBY_FOLIAGE_DEPTH_PREPASS_FRAG_SPV ${SHADER_OUTPUT_DIR}/nearby_foliage_depth_prepass.frag.spv)
set(NEARBY_FOLIAGE_FRAG_SPV ${SHADER_OUTPUT_DIR}/nearby_foliage.frag.spv)
set(NEARBY_FOLIAGE_DECODE_COMP_SPV ${SHADER_OUTPUT_DIR}/nearby_foliage_decode.comp.spv)
set(WATER_MESH_VERT_SPV ${SHADER_OUTPUT_DIR}/water_mesh.vert.spv)
set(WATER_MESH_BRIDGE_VERT_SPV ${SHADER_OUTPUT_DIR}/water_mesh_bridge.vert.spv)
set(WATER_MESH_FRAG_SPV ${SHADER_OUTPUT_DIR}/water_mesh.frag.spv)
set(WATER_INITIALIZE_SPECTRUM_COMP_SPV ${SHADER_OUTPUT_DIR}/water_initialize_spectrum.comp.spv)
set(WATER_SPECTRUM_UPDATE_COMP_SPV ${SHADER_OUTPUT_DIR}/water_spectrum_update.comp.spv)
set(WATER_FFT_STAGE_COMP_SPV ${SHADER_OUTPUT_DIR}/water_fft_stage.comp.spv)
set(WATER_BUILD_MAPS_COMP_SPV ${SHADER_OUTPUT_DIR}/water_build_maps.comp.spv)

compile_shader(${CMAKE_SOURCE_DIR}/shaders/triangle.vert ${TRIANGLE_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/triangle.frag ${TRIANGLE_FRAG_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/line.vert ${LINE_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/line.frag ${LINE_FRAG_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/quadtree_mesh.vert ${QUADTREE_MESH_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/quadtree_mesh_bridge.vert ${QUADTREE_MESH_BRIDGE_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/quadtree_mesh.frag ${QUADTREE_MESH_FRAG_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/heightmap_generate.comp ${HEIGHTMAP_GENERATE_COMP_SPV})
compile_shader(
    ${CMAKE_SOURCE_DIR}/shaders/foliage_generate.comp
    ${FOLIAGE_GENERATE_COMP_SPV}
    ${CMAKE_SOURCE_DIR}/shaders/foliage_common.glsl)
compile_shader(${CMAKE_SOURCE_DIR}/shaders/foliage_canopy.vert ${FOLIAGE_CANOPY_VERT_SPV})
compile_shader(
    ${CMAKE_SOURCE_DIR}/shaders/foliage_canopy.frag
    ${FOLIAGE_CANOPY_FRAG_SPV}
    ${CMAKE_SOURCE_DIR}/shaders/foliage_common.glsl)
compile_shader(
    ${CMAKE_SOURCE_DIR}/shaders/foliage_canopy_generate.comp
    ${FOLIAGE_CANOPY_GENERATE_COMP_SPV}
    ${CMAKE_SOURCE_DIR}/shaders/foliage_common.glsl)
compile_shader(${CMAKE_SOURCE_DIR}/shaders/skybox.vert ${SKYBOX_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/skybox.frag ${SKYBOX_FRAG_SPV})
compile_shader(
    ${CMAKE_SOURCE_DIR}/shaders/foliage_imposter.vert
    ${FOLIAGE_IMPOSTER_VERT_SPV}
    ${CMAKE_SOURCE_DIR}/shaders/foliage_common.glsl)
compile_shader(${CMAKE_SOURCE_DIR}/shaders/foliage_imposter_depth_prepass.frag ${FOLIAGE_IMPOSTER_DEPTH_PREPASS_FRAG_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/foliage_imposter.frag ${FOLIAGE_IMPOSTER_FRAG_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/nearby_foliage.vert ${NEARBY_FOLIAGE_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/nearby_foliage_depth_prepass.frag ${NEARBY_FOLIAGE_DEPTH_PREPASS_FRAG_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/nearby_foliage.frag ${NEARBY_FOLIAGE_FRAG_SPV})
compile_shader(
    ${CMAKE_SOURCE_DIR}/shaders/nearby_foliage_decode.comp
    ${NEARBY_FOLIAGE_DECODE_COMP_SPV}
    ${CMAKE_SOURCE_DIR}/shaders/foliage_common.glsl)
compile_shader(${CMAKE_SOURCE_DIR}/shaders/water_mesh.vert ${WATER_MESH_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/water_mesh_bridge.vert ${WATER_MESH_BRIDGE_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/water_mesh.frag ${WATER_MESH_FRAG_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/water_initialize_spectrum.comp ${WATER_INITIALIZE_SPECTRUM_COMP_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/water_spectrum_update.comp ${WATER_SPECTRUM_UPDATE_COMP_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/water_fft_stage.comp ${WATER_FFT_STAGE_COMP_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/water_build_maps.comp ${WATER_BUILD_MAPS_COMP_SPV})

add_custom_target(shaders ALL
    DEPENDS
        ${TRIANGLE_VERT_SPV}
        ${TRIANGLE_FRAG_SPV}
        ${LINE_VERT_SPV}
        ${LINE_FRAG_SPV}
        ${QUADTREE_MESH_VERT_SPV}
        ${QUADTREE_MESH_BRIDGE_VERT_SPV}
        ${QUADTREE_MESH_FRAG_SPV}
        ${HEIGHTMAP_GENERATE_COMP_SPV}
        ${FOLIAGE_GENERATE_COMP_SPV}
        ${FOLIAGE_CANOPY_VERT_SPV}
        ${FOLIAGE_CANOPY_FRAG_SPV}
        ${FOLIAGE_CANOPY_GENERATE_COMP_SPV}
        ${SKYBOX_VERT_SPV}
        ${SKYBOX_FRAG_SPV}
        ${FOLIAGE_IMPOSTER_VERT_SPV}
        ${FOLIAGE_IMPOSTER_DEPTH_PREPASS_FRAG_SPV}
        ${FOLIAGE_IMPOSTER_FRAG_SPV}
        ${NEARBY_FOLIAGE_VERT_SPV}
        ${NEARBY_FOLIAGE_DEPTH_PREPASS_FRAG_SPV}
        ${NEARBY_FOLIAGE_FRAG_SPV}
        ${NEARBY_FOLIAGE_DECODE_COMP_SPV}
        ${WATER_MESH_VERT_SPV}
        ${WATER_MESH_BRIDGE_VERT_SPV}
        ${WATER_MESH_FRAG_SPV}
        ${WATER_INITIALIZE_SPECTRUM_COMP_SPV}
        ${WATER_SPECTRUM_UPDATE_COMP_SPV}
        ${WATER_FFT_STAGE_COMP_SPV}
        ${WATER_BUILD_MAPS_COMP_SPV}
)

add_executable(terrain_sandbox
    src/main.cpp
    src/App.cpp
    src/AppPanels.cpp
    src/assets/RuntimeAssetCompression.cpp
    src/assets/RuntimeAssetReader.cpp
    src/CameraManager.cpp
    src/EngineRendererBase.cpp
    src/FoliageCanopyRenderer.cpp
    src/FoliageImposterRenderer.cpp
    src/FreeFlightCameraController.cpp
    src/GamepadInput.cpp
    src/Gameplay.cpp
    src/HeightmapNoiseGenerator.cpp
    src/LineRenderer.cpp
    src/LightingSystem.cpp
    src/NearbyFoliageRenderer.cpp
    src/PerfPanel.cpp
    src/PerformanceCapture.cpp
    src/QuadtreeMeshRenderer.cpp
    src/QuadtreeWaterMeshRenderer.cpp
    src/SDLRenderer.cpp
    src/SkyboxRenderer.cpp
    src/TriangleRenderer.cpp
    src/WorldGridQuadtree.cpp
    src/WorldGridFoliageManager.cpp
    src/WorldGridFoliageCanopyManager.cpp
    src/WorldGridQuadtreeDebugRenderer.cpp
    src/WorldGridQuadtreeHeightmapManager.cpp
    src/WorldGridQuadtreeWaterManager.cpp
)

add_dependencies(terrain_sandbox shaders)

file(GENERATE
    OUTPUT ${CMAKE_BINARY_DIR}/copy_runtime_assets.cmake
    CONTENT [=[
set(asset_source_dir "$ENV{ASSET_SOURCE_DIR}")
set(asset_output_dir "$ENV{ASSET_OUTPUT_DIR}")

file(MAKE_DIRECTORY "${asset_output_dir}")
file(GLOB runtime_asset_bins
    "${asset_source_dir}/*.meshbin"
    "${asset_source_dir}/*.texbin"
    "${asset_source_dir}/*.assetbin")

foreach(asset_bin IN LISTS runtime_asset_bins)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy "${asset_bin}" "${asset_output_dir}"
        COMMAND_ERROR_IS_FATAL ANY)
endforeach()
]=]
)

add_custom_target(stage_runtime_assets ALL
    COMMAND ${CMAKE_COMMAND} -E env
        ASSET_SOURCE_DIR=${RUNTIME_ASSET_SOURCE_DIR}
        ASSET_OUTPUT_DIR=${RUNTIME_ASSET_OUTPUT_DIR}
        ${CMAKE_COMMAND} -P ${CMAKE_BINARY_DIR}/copy_runtime_assets.cmake
    VERBATIM
)

add_dependencies(terrain_sandbox stage_runtime_assets)

add_custom_command(TARGET terrain_sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:terrain_sandbox> $<TARGET_RUNTIME_DLLS:terrain_sandbox>
    COMMAND_EXPAND_LISTS
)

target_include_directories(terrain_sandbox
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${glm_SOURCE_DIR}
)

target_compile_definitions(terrain_sandbox
    PRIVATE
        TERRAIN_SANDBOX_SHADER_DIR="shaders"
        TERRAIN_SANDBOX_ASSET_DIR="assets/runtime"
        NOMINMAX
)

target_link_libraries(terrain_sandbox
    PRIVATE
        imgui_backend
        lz4_local
        SDL3::SDL3
)

if (MSVC)
    target_compile_options(imgui_backend
        PRIVATE
            /permissive-
            /Zc:__cplusplus
            /Gy
            /Gw
            /MP
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/Oi>
            $<$<CONFIG:Release>:/GL>
    )

    target_compile_options(terrain_sandbox
        PRIVATE
            /permissive-
            /Zc:__cplusplus
            /Gy
            /Gw
            /MP
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/Oi>
            $<$<CONFIG:Release>:/GL>
    )

    target_link_options(imgui_backend
        PRIVATE
            $<$<CONFIG:Release>:/LTCG>
    )

    target_link_options(terrain_sandbox
        PRIVATE
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
            $<$<CONFIG:Release>:/LTCG>
    )

endif()
