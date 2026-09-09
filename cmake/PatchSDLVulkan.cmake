# SDL release-3.4.0: preserve whole-volume barriers with or without maintenance9.
# https://docs.vulkan.org/refpages/latest/refpages/source/VkImageMemoryBarrier.html
# Applied on every configure, including already-populated FetchContent sources.
# Review/remove this patch when upgrading SDL; unexpected source must fail visibly.
set(sdl_vulkan_source "${sdl3_SOURCE_DIR}/src/gpu/vulkan/SDL_gpu_vulkan.c")
file(READ "${sdl_vulkan_source}" sdl_vulkan_original)
set(sdl_vulkan_patched "${sdl_vulkan_original}")

function(sdl_vulkan_replace_once before after)
    string(FIND "${sdl_vulkan_patched}" "${after}" applied)
    if(NOT applied EQUAL -1)
        return()
    endif()
    string(FIND "${sdl_vulkan_patched}" "${before}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "SDL Vulkan source changed; review cmake/PatchSDLVulkan.cmake")
    endif()
    string(REPLACE "${before}" "" without_match "${sdl_vulkan_patched}")
    string(LENGTH "${sdl_vulkan_patched}" original_length)
    string(LENGTH "${without_match}" remaining_length)
    string(LENGTH "${before}" match_length)
    math(EXPR removed_length "${original_length} - ${remaining_length}")
    if(NOT removed_length EQUAL match_length)
        message(FATAL_ERROR "SDL Vulkan patch target is ambiguous")
    endif()
    string(REPLACE "${before}" "${after}" updated "${sdl_vulkan_patched}")
    set(sdl_vulkan_patched "${updated}" PARENT_SCOPE)
endfunction()

# Store the type on the texture itself: defrag barriers can precede container assignment.
sdl_vulkan_replace_once(
    "    Uint32 depth; // used for cleanup only"
    "    Uint32 depth; // used for cleanup only\n    bool is3D; // Codex: whole-volume Vulkan barriers")
sdl_vulkan_replace_once(
    "    texture->depth = depth;"
    "    texture->depth = depth;\n    texture->is3D = createinfo->type == SDL_GPU_TEXTURETYPE_3D;")
sdl_vulkan_replace_once(
    "        windowData->textureContainers[i].activeTexture->container = &windowData->textureContainers[i];"
    "        windowData->textureContainers[i].activeTexture->is3D = false;\n        windowData->textureContainers[i].activeTexture->container = &windowData->textureContainers[i];")
sdl_vulkan_replace_once(
    "    memoryBarrier.subresourceRange.layerCount = 1;"
    "    memoryBarrier.subresourceRange.layerCount = textureSubresource->parent->is3D ? VK_REMAINING_ARRAY_LAYERS : 1;")

if(NOT sdl_vulkan_patched STREQUAL sdl_vulkan_original)
    file(WRITE "${sdl_vulkan_source}" "${sdl_vulkan_patched}")
    message(STATUS "Applied SDL Vulkan whole-volume barrier compatibility patch")
endif()
