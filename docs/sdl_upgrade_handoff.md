# SDL 3.4.16 upgrade review

The app and converter FetchContent URLs now pin `release-3.4.16.zip`. The old `cmake/PatchSDLVulkan.cmake` is removed. The app applies the narrower `cmake/PatchSDLVulkanFullTexture.cmake` for the remaining upstream full-texture barrier issue. No application code, shaders, device feature requests, validation settings, other dependency versions, or asset packs changed.

## Upstream review and full-texture barrier fix

Reviewed the actual [release-3.4.16 Vulkan source](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/gpu/vulkan/SDL_gpu_vulkan.c). Both canonical app builds populated the identical source (SHA-256 `83750FF683D9F242EF431B155042ECE67E9F99FA04698315E59DEBDA019C5D85`).

- `VULKAN_INTERNAL_TextureSubresourceMemoryBarrier` (lines 2831–2859) initializes the layer count to one and selects `VK_REMAINING_ARRAY_LAYERS` for 3D textures. Arrays/cubemaps retain their individual layer and mip selection. 3D subresources use layer zero.
- `VULKAN_INTERNAL_CreateTexture` initializes native type, level count, and layer count before returning (lines 5756–5758). Swapchain textures explicitly initialize as 2D with one level/layer (lines 4868–4870).
- Defragmentation creates the replacement through that same constructor (line 11130), so barrier metadata is valid before the replacement receives its container (line 11200). The old Codex field is unnecessary.
- However, `VULKAN_INTERNAL_FullTextureMemoryBarrier` (lines 2812–2829) passes `texture->layerCount`, which creation sets to one for 3D. Default-usage transitions and defragmentation use this newer path. It does not apply the subresource helper's 3D exception.

Before the follow-up fix, Debug startup, cloud traversal, and heightmap stress each reported `WARNING-VkImageSubresourceRange-layerCount-compatibility`. Count one covers all depth slices with maintenance9 disabled, but would mean a single slice with that feature enabled.

The user authorized fixing this remaining issue. The new configure-time patch changes only the full-texture barrier's layer-count argument: 3D uses `VK_REMAINING_ARRAY_LAYERS`, while other texture types retain `texture->layerCount`. It uses SDL's native type metadata, needs no extra field, is idempotent, and fails on missing or ambiguous source. Review/remove it on future SDL upgrades. No validation suppression or maintenance9 feature enablement is involved.

An SDL implementation-only change can leave its import library unchanged, so the app previously skipped relinking and its post-build DLL copy. `LINK_DEPENDS` now includes SDL's runtime DLL, ensuring the existing staging step runs for these fixes. The stale staged DLL was detected by hash comparison before accepting runtime results.

## Initial upgrade validation (before the follow-up patch)

- `tools\build.cmd Debug` and `tools\build.cmd Release`: passed. All six CTest tests passed in each canonical directory.
- Both configured source headers identify 3.4.16. Both staged `app/SDL3.dll` files report `3, 4, 16, 0` and match their respective newly built DLLs by SHA-256. Debug: `4FE5C082421038D667D80EBACD707F27D3976C68E8AA8B3560585F00FD4BD5C4`; Release: `66852EEF35210A822BAE6F56A73A81154CCB8D2CF7C36053CA94D1304847C665`.
- Debug `--disable-steam --verbose-startup --quit-after-first-frame`: exit zero, first frame submitted and orderly shutdown.
- Debug `--disable-steam --verify-clouds --quit-after-frames 720`: exit zero, all 12 stages, 24 macro revisions, including the 3D cloud volume.
- Debug `--disable-steam --stress-heightmap-pipeline --quit-after-frames 3600`: exit zero. Uploads 6492, finals 11200, CPU completed 1702, evicted 1348, discarded/stale-retired 163 each, cache-blocked 27510, readback-blocked 1698, staging-blocked 455; readback high-water 8 and decode/upload batch high-water 63 each. All required transition checks passed.
- `tools\build.cmd Assets Release`: passed after local cache repair. The converter's staged SDL DLL reports 3.4.16 and matches its build output (SHA-256 `F0F50C3CE7B32EB2CF6871A01201937A49CBA3901D96630E636C70D431E56B54`). No converter operation or asset generation was run.
- Interactive maximize/restore during traversal changed the viewport from 1265×865 to 1405×1094 and back. Presentation continued, with water and automatic exposure active. This limited smoke inspection does not establish comprehensive terrain/cloud/water visual or performance acceptance.

Build/test logs are under ignored `build/sdl-upgrade-*.log`; GPU logs are under `build/Debug/sdl-upgrade-*.log`. Runtime checks used staged assets and ran from `build/Debug/app`, preserving the user's root `imgui.ini` and `launch.log` edits. The converter initially had obsolete `FETCHCONTENT_SOURCE_DIR_*` cache overrides pointing at removed `build/Assets/_deps` directories; clearing those local cache overrides allowed canonical configuration to succeed without source/script changes or broad build-tree deletion.

## Follow-up fix validation

- Debug/Release builds and all six tests in each passed. Reapplying the patch through `cmake -P` succeeded without modifying already-patched source. Both app configurations contain the same patched source, with exactly one changed line relative to upstream.
- Debug startup passed with no Vulkan validation warnings/errors. The 720-frame cloud traversal passed all 12 stages and 24 macro revisions with no validation diagnostics.
- Staged Debug SDL DLL matches its patched build output: SHA-256 `8DAB49A2F4376410BDA9361F02226730368C868DF5AA425C1B97E836BB52B542`.
- A forced Release clean removed 373 outputs; the subsequent canonical build recompiled SDL, shaders, tests, and the app successfully. All six Release tests passed again. The staged SDL DLL matches its rebuilt output: SHA-256 `EB0BEA884B9F4DE4E737C76256348CE40AF8043C9947FFDDA9331873201450FE`.
- Patched Debug 3600-frame heightmap stress passed with no validation warnings/errors: uploads 5939, finals 11534, CPU completed 1907, evicted 1469, discarded/stale-retired 158 each, cache-blocked 26749, readback-blocked 2722, staging-blocked 606, readback high-water 8, decode/upload batch high-water 63 each.
- Launch verification of the freshly rebuilt Release app was blocked by Windows: `An Application Control policy has blocked this file`, including outside the execution sandbox. This is separate from successful compilation/staging and the passing Debug GPU runs. No security settings were changed.
- Follow-up logs use `sdl-maintenance9-*`; the subsequent forced Release rebuild log is `build/release-forced-rebuild.log`.
