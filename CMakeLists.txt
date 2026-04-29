cmake_minimum_required(VERSION 3.25)

project(TerrainSandbox LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
    SDL3_image
    URL https://github.com/libsdl-org/SDL_image/archive/refs/tags/release-3.2.4.zip
)

FetchContent_Declare(
    glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.zip
)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TESTS OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_BACKEND_STB ON CACHE BOOL "" FORCE)
set(SDLIMAGE_BACKEND_WIC OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_AVIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_BMP OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_GIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JPG OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_JXL OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_LBM OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_PCX OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_PNG ON CACHE BOOL "" FORCE)
set(SDLIMAGE_PNM OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_QOI OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_SVG OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TGA OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_TIF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_WEBP OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_XCF OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_XPM OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_XV OFF CACHE BOOL "" FORCE)
set(SDLIMAGE_PNG_SAVE OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3 SDL3_image imgui glm)

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
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

function(compile_shader SOURCE_FILE OUTPUT_FILE)
    add_custom_command(
        OUTPUT ${OUTPUT_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUTPUT_DIR}
        COMMAND ${VULKAN_GLSLC_EXECUTABLE} -o ${OUTPUT_FILE} ${SOURCE_FILE}
        DEPENDS ${SOURCE_FILE}
        VERBATIM
    )
endfunction()

set(TRIANGLE_VERT_SPV ${SHADER_OUTPUT_DIR}/triangle.vert.spv)
set(TRIANGLE_FRAG_SPV ${SHADER_OUTPUT_DIR}/triangle.frag.spv)
set(LINE_VERT_SPV ${SHADER_OUTPUT_DIR}/line.vert.spv)
set(LINE_FRAG_SPV ${SHADER_OUTPUT_DIR}/line.frag.spv)
set(QUADTREE_MESH_VERT_SPV ${SHADER_OUTPUT_DIR}/quadtree_mesh.vert.spv)
set(QUADTREE_MESH_FRAG_SPV ${SHADER_OUTPUT_DIR}/quadtree_mesh.frag.spv)
set(HEIGHTMAP_GENERATE_COMP_SPV ${SHADER_OUTPUT_DIR}/heightmap_generate.comp.spv)
set(SKYBOX_VERT_SPV ${SHADER_OUTPUT_DIR}/skybox.vert.spv)
set(SKYBOX_FRAG_SPV ${SHADER_OUTPUT_DIR}/skybox.frag.spv)
set(WATER_MESH_VERT_SPV ${SHADER_OUTPUT_DIR}/water_mesh.vert.spv)
set(WATER_MESH_FRAG_SPV ${SHADER_OUTPUT_DIR}/water_mesh.frag.spv)

compile_shader(${CMAKE_SOURCE_DIR}/shaders/triangle.vert ${TRIANGLE_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/triangle.frag ${TRIANGLE_FRAG_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/line.vert ${LINE_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/line.frag ${LINE_FRAG_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/quadtree_mesh.vert ${QUADTREE_MESH_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/quadtree_mesh.frag ${QUADTREE_MESH_FRAG_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/heightmap_generate.comp ${HEIGHTMAP_GENERATE_COMP_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/skybox.vert ${SKYBOX_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/skybox.frag ${SKYBOX_FRAG_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/water_mesh.vert ${WATER_MESH_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/water_mesh.frag ${WATER_MESH_FRAG_SPV})

add_custom_target(shaders ALL
    DEPENDS
        ${TRIANGLE_VERT_SPV}
        ${TRIANGLE_FRAG_SPV}
        ${LINE_VERT_SPV}
        ${LINE_FRAG_SPV}
        ${QUADTREE_MESH_VERT_SPV}
        ${QUADTREE_MESH_FRAG_SPV}
        ${HEIGHTMAP_GENERATE_COMP_SPV}
        ${SKYBOX_VERT_SPV}
        ${SKYBOX_FRAG_SPV}
        ${WATER_MESH_VERT_SPV}
        ${WATER_MESH_FRAG_SPV}
)

add_executable(terrain_sandbox
    src/main.cpp
    src/App.cpp
    src/AppPanels.cpp
    src/CameraManager.cpp
    src/EngineRendererBase.cpp
    src/FreeFlightCameraController.cpp
    src/GamepadInput.cpp
    src/HeightmapNoiseGenerator.cpp
    src/LineRenderer.cpp
    src/LightingSystem.cpp
    src/PerfPanel.cpp
    src/PerformanceCapture.cpp
    src/QuadtreeMeshRenderer.cpp
    src/QuadtreeWaterMeshRenderer.cpp
    src/SDLRenderer.cpp
    src/SkyboxRenderer.cpp
    src/TriangleRenderer.cpp
    src/WorldGridQuadtree.cpp
    src/WorldGridQuadtreeDebugRenderer.cpp
    src/WorldGridQuadtreeHeightmapManager.cpp
    src/WorldGridQuadtreeWaterManager.cpp
)

add_executable(quadtree_sanity
    src/HeightmapNoiseGenerator.cpp
    src/QuadtreeSanityTests.cpp
)

add_dependencies(terrain_sandbox shaders)

add_custom_command(TARGET terrain_sandbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy -t $<TARGET_FILE_DIR:terrain_sandbox> $<TARGET_RUNTIME_DLLS:terrain_sandbox>
    COMMAND_EXPAND_LISTS
)

target_include_directories(terrain_sandbox
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${glm_SOURCE_DIR}
)

target_include_directories(quadtree_sanity
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${glm_SOURCE_DIR}
)

target_compile_definitions(terrain_sandbox
    PRIVATE
        TERRAIN_SANDBOX_SHADER_DIR="${SHADER_OUTPUT_DIR}"
        TERRAIN_SANDBOX_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/resources"
        NOMINMAX
)

target_compile_definitions(quadtree_sanity
    PRIVATE
        NOMINMAX
)

target_link_libraries(terrain_sandbox
    PRIVATE
        imgui_backend
        SDL3::SDL3
        SDL3_image::SDL3_image
)

target_link_libraries(quadtree_sanity
    PRIVATE
        SDL3::SDL3
)

if (MSVC)
    target_compile_options(imgui_backend
        PRIVATE
            /permissive-
            /Zc:__cplusplus
            /Gy
            /Gw
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
            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/Oi>
            $<$<CONFIG:Release>:/GL>
    )

    target_compile_options(quadtree_sanity
        PRIVATE
            /permissive-
            /Zc:__cplusplus
            /Gy
            /Gw
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

    target_link_options(quadtree_sanity
        PRIVATE
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
            $<$<CONFIG:Release>:/LTCG>
    )
endif()
