# SDL release-3.4.16: extend its 3D subresource barrier fix to full-texture barriers.
# Keep whole-volume transitions with or without maintenance9. Native texture type
# is initialized before defrag barriers; array/cubemap layer counts stay unchanged.
# Review/remove on SDL upgrades. Reject unexpected or ambiguous upstream source.
set(sdl_vulkan_source "${sdl3_SOURCE_DIR}/src/gpu/vulkan/SDL_gpu_vulkan.c")
file(READ "${sdl_vulkan_source}" sdl_vulkan_original)
set(sdl_vulkan_before [=[        texture->levelCount,
        0,
        texture->layerCount,
        texture);]=])
set(sdl_vulkan_after [=[        texture->levelCount,
        0,
        texture->type == SDL_GPU_TEXTURETYPE_3D ? VK_REMAINING_ARRAY_LAYERS : texture->layerCount,
        texture);]=])

string(FIND "${sdl_vulkan_original}" "${sdl_vulkan_after}" sdl_vulkan_applied)
if(NOT sdl_vulkan_applied EQUAL -1)
    set(sdl_vulkan_expected "${sdl_vulkan_after}")
else()
    set(sdl_vulkan_expected "${sdl_vulkan_before}")
endif()
string(REPLACE "${sdl_vulkan_expected}" "" sdl_vulkan_without_match "${sdl_vulkan_original}")
string(LENGTH "${sdl_vulkan_original}" sdl_vulkan_original_length)
string(LENGTH "${sdl_vulkan_without_match}" sdl_vulkan_remaining_length)
string(LENGTH "${sdl_vulkan_expected}" sdl_vulkan_match_length)
math(EXPR sdl_vulkan_removed_length "${sdl_vulkan_original_length} - ${sdl_vulkan_remaining_length}")
if(NOT sdl_vulkan_removed_length EQUAL sdl_vulkan_match_length)
    message(FATAL_ERROR "SDL Vulkan full-texture barrier changed or is ambiguous; review cmake/PatchSDLVulkanFullTexture.cmake")
endif()
if(sdl_vulkan_applied EQUAL -1)
    string(REPLACE "${sdl_vulkan_before}" "${sdl_vulkan_after}" sdl_vulkan_patched "${sdl_vulkan_original}")
    file(WRITE "${sdl_vulkan_source}" "${sdl_vulkan_patched}")
    message(STATUS "Applied SDL Vulkan 3D full-texture barrier compatibility patch")
endif()
