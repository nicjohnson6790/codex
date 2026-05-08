cmake_minimum_required(VERSION 3.25)

project(pinetreepack_converter LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

option(CONVERTER_ENABLE_COMPRESSONATOR "Enable AMD Compressonator BC compression support in the converter" ON)
set(COMPRESSONATOR_SDK_ROOT "" CACHE PATH "Root directory of the AMD Compressonator SDK")

if (NOT COMPRESSONATOR_SDK_ROOT AND DEFINED ENV{COMPRESSONATOR_SDK_ROOT})
    set(COMPRESSONATOR_SDK_ROOT "$ENV{COMPRESSONATOR_SDK_ROOT}")
endif()

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
    SDL3_image
    URL https://github.com/libsdl-org/SDL_image/archive/refs/tags/release-3.2.4.zip
)

FetchContent_Declare(
    assimp
    URL https://github.com/assimp/assimp/archive/refs/tags/v5.4.3.zip
)

FetchContent_Declare(
    lz4
    URL https://github.com/lz4/lz4/archive/refs/tags/v1.10.0.zip
)

set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
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
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(SDL3 SDL3_image assimp lz4)

set(CONVERTER_SHADER_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${CONVERTER_SHADER_OUTPUT_DIR})

function(compile_converter_shader SOURCE_FILE OUTPUT_FILE)
    add_custom_command(
        OUTPUT ${OUTPUT_FILE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CONVERTER_SHADER_OUTPUT_DIR}
        COMMAND ${VULKAN_GLSLC_EXECUTABLE} -o ${OUTPUT_FILE} ${SOURCE_FILE}
        DEPENDS ${SOURCE_FILE}
        VERBATIM
    )
endfunction()

set(PINE_IMPOSTER_CAPTURE_VERT_SPV ${CONVERTER_SHADER_OUTPUT_DIR}/pine_imposter_capture.vert.spv)
set(PINE_IMPOSTER_CAPTURE_COLOR_FRAG_SPV ${CONVERTER_SHADER_OUTPUT_DIR}/pine_imposter_capture_color.frag.spv)
set(PINE_IMPOSTER_CAPTURE_NORMAL_FRAG_SPV ${CONVERTER_SHADER_OUTPUT_DIR}/pine_imposter_capture_normal.frag.spv)

compile_converter_shader(
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pine_imposter_capture.vert
    ${PINE_IMPOSTER_CAPTURE_VERT_SPV})
compile_converter_shader(
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pine_imposter_capture_color.frag
    ${PINE_IMPOSTER_CAPTURE_COLOR_FRAG_SPV})
compile_converter_shader(
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/pine_imposter_capture_normal.frag
    ${PINE_IMPOSTER_CAPTURE_NORMAL_FRAG_SPV})

add_custom_target(converter_shaders ALL
    DEPENDS
        ${PINE_IMPOSTER_CAPTURE_VERT_SPV}
        ${PINE_IMPOSTER_CAPTURE_COLOR_FRAG_SPV}
        ${PINE_IMPOSTER_CAPTURE_NORMAL_FRAG_SPV}
)

if (CONVERTER_ENABLE_COMPRESSONATOR)
    find_path(COMPRESSONATOR_INCLUDE_DIR
        NAMES compressonator.h
        HINTS ${COMPRESSONATOR_SDK_ROOT}
        PATH_SUFFIXES include Include
    )

    find_library(COMPRESSONATOR_RELEASE_LIBRARY
        NAMES Compressonator_MD Compressonator_MT
        HINTS ${COMPRESSONATOR_SDK_ROOT}
        PATH_SUFFIXES lib lib64 Lib lib/bin/x64 bin/x64
    )

    find_library(COMPRESSONATOR_DEBUG_LIBRARY
        NAMES Compressonator_MDd Compressonator_MTd
        HINTS ${COMPRESSONATOR_SDK_ROOT}
        PATH_SUFFIXES lib lib64 Lib lib/bin/x64 bin/x64
    )

    if (COMPRESSONATOR_INCLUDE_DIR AND (COMPRESSONATOR_RELEASE_LIBRARY OR COMPRESSONATOR_DEBUG_LIBRARY))
        add_library(compressonator_core INTERFACE)
        target_include_directories(compressonator_core INTERFACE ${COMPRESSONATOR_INCLUDE_DIR})
        if (COMPRESSONATOR_DEBUG_LIBRARY)
            target_link_libraries(compressonator_core INTERFACE debug ${COMPRESSONATOR_DEBUG_LIBRARY})
        endif()
        if (COMPRESSONATOR_RELEASE_LIBRARY)
            target_link_libraries(compressonator_core INTERFACE optimized ${COMPRESSONATOR_RELEASE_LIBRARY})
        elseif (COMPRESSONATOR_DEBUG_LIBRARY)
            target_link_libraries(compressonator_core INTERFACE optimized ${COMPRESSONATOR_DEBUG_LIBRARY})
        endif()
    else()
        message(STATUS
            "Compressonator SDK not found. Pine imposter BC output will fail clearly when requested. "
            "Set COMPRESSONATOR_SDK_ROOT or the COMPRESSONATOR_SDK_ROOT environment variable to enable it.")
    endif()
endif()

add_library(lz4_local STATIC
    ${lz4_SOURCE_DIR}/lib/lz4.c
)

target_include_directories(lz4_local
    PUBLIC
        ${lz4_SOURCE_DIR}/lib
)

add_library(runtime_asset_reader STATIC
    ../../src/assets/RuntimeAssetCompression.cpp
    ../../src/assets/RuntimeAssetReader.cpp
)

target_include_directories(runtime_asset_reader
    PUBLIC
        ../../src
)

target_link_libraries(runtime_asset_reader
    PUBLIC
        lz4_local
        SDL3::SDL3
)

add_executable(converter
    ConverterMain.cpp
    CompressonatorBcCodec.cpp
    PineTreePackConverter.cpp
    PineImposterGenerator.cpp
    FbxImport.cpp
    TextureImport.cpp
    MeshBinWriter.cpp
    TexBinWriter.cpp
    AssetBinWriter.cpp
)

add_dependencies(converter converter_shaders)

target_compile_definitions(converter
    PRIVATE
        CONVERTER_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
        CONVERTER_SHADER_DIR="${CONVERTER_SHADER_OUTPUT_DIR}"
)

target_include_directories(converter
    PRIVATE
        ../../src
        ${assimp_SOURCE_DIR}/include
)

if (TARGET assimp::assimp)
    set(ASSIMP_TARGET assimp::assimp)
else()
    set(ASSIMP_TARGET assimp)
endif()

target_link_libraries(converter
    PRIVATE
        runtime_asset_reader
        SDL3::SDL3
        SDL3_image::SDL3_image
        ${ASSIMP_TARGET}
)

if (TARGET compressonator_core)
    target_compile_definitions(converter PRIVATE CONVERTER_HAS_COMPRESSONATOR=1)
    target_link_libraries(converter PRIVATE compressonator_core)
endif()
