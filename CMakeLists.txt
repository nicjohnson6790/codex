cmake_minimum_required(VERSION 3.25)

project(SdlGpuImguiTriangle LANGUAGES CXX)

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
    glm
    URL https://github.com/g-truc/glm/archive/refs/tags/1.0.1.zip
)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3 imgui glm)

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

compile_shader(${CMAKE_SOURCE_DIR}/shaders/triangle.vert ${TRIANGLE_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/triangle.frag ${TRIANGLE_FRAG_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/line.vert ${LINE_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/line.frag ${LINE_FRAG_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/quadtree_mesh.vert ${QUADTREE_MESH_VERT_SPV})
compile_shader(${CMAKE_SOURCE_DIR}/shaders/quadtree_mesh.frag ${QUADTREE_MESH_FRAG_SPV})

add_custom_target(shaders ALL
    DEPENDS
        ${TRIANGLE_VERT_SPV}
        ${TRIANGLE_FRAG_SPV}
        ${LINE_VERT_SPV}
        ${LINE_FRAG_SPV}
        ${QUADTREE_MESH_VERT_SPV}
        ${QUADTREE_MESH_FRAG_SPV}
)

add_executable(hello_triangle
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
    src/SDLRenderer.cpp
    src/TriangleRenderer.cpp
    src/WorldGridQuadtree.cpp
    src/WorldGridQuadtreeDebugRenderer.cpp
    src/WorldGridQuadtreeHeightmapManager.cpp
)

add_executable(quadtree_sanity
    src/HeightmapNoiseGenerator.cpp
    src/QuadtreeSanityTests.cpp
)

add_dependencies(hello_triangle shaders)

target_include_directories(hello_triangle
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${glm_SOURCE_DIR}
)

target_include_directories(quadtree_sanity
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${glm_SOURCE_DIR}
)

target_compile_definitions(hello_triangle
    PRIVATE
        HELLO_TRIANGLE_SHADER_DIR="${SHADER_OUTPUT_DIR}"
        NOMINMAX
)

target_compile_definitions(quadtree_sanity
    PRIVATE
        NOMINMAX
)

target_link_libraries(hello_triangle
    PRIVATE
        imgui_backend
        SDL3::SDL3
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

    target_compile_options(hello_triangle
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

    target_link_options(hello_triangle
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
