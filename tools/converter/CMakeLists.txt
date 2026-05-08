cmake_minimum_required(VERSION 3.25)

project(pinetreepack_converter LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)

include(FetchContent)

FetchContent_Declare(
    SDL3
    URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.0.zip
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
set(ASSIMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ASSIMP_BUILD_ASSIMP_TOOLS OFF CACHE BOOL "" FORCE)
set(ASSIMP_INSTALL OFF CACHE BOOL "" FORCE)
set(ASSIMP_INJECT_DEBUG_POSTFIX OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(SDL3 assimp lz4)

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
    PineTreePackConverter.cpp
    FbxImport.cpp
    TextureImport.cpp
    MeshBinWriter.cpp
    TexBinWriter.cpp
    AssetBinWriter.cpp
)

target_compile_definitions(converter
    PRIVATE
        CONVERTER_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../.."
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
        ${ASSIMP_TARGET}
)
