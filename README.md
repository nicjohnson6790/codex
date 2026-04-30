# SDL3 GPU + ImGui Terrain Sandbox

This project is a small editor-style sandbox built around SDL3 GPU, Dear ImGui docking, quadtree terrain rendering, and shared-cascade FFT water.

## Overview

The app renders into an offscreen viewport that is shown inside an ImGui layout. Terrain is managed as quadtree patches, with heightmaps cached in an LRU and rendered through a reusable indexed patch mesh. Heightmaps are generated on the GPU with a compute shader, and per-slice min/max extents are reduced on the GPU and read back asynchronously for culling, subdivision, and debug bounds.

Water follows the same broad ownership split as terrain, but stays globally simulated:

- `WorldGridQuadtree` decides which leaves are visible
- `WorldGridQuadtreeWaterManager` decides which of those leaves should draw water
- `QuadtreeWaterMeshRenderer` owns the reusable water mesh, FFT compute resources, and water draw path
- `SDLRenderer` orchestrates the terrain and water compute/render order each frame

The current water path uses four shared FFT cascades at `512 x 512`, with a single reusable `129 x 129` water mesh instanced across visible leaves. The simulation uses precomputed static spectrum data, SSBO-backed FFT working buffers, a shared-memory butterfly FFT pass, and final displacement/slope texture arrays sampled in world space by the water draw shaders. The water draw also reuses the resident terrain heightmap slices to estimate local water depth for shallow-water displacement damping, depth-based color/transmission shaping, and a shallow refracted terrain lookup. Water shading is now driven by a sky/atmosphere-aware PBR-style surface model that samples the same cubemap and atmosphere LUT used by the skybox pass.

Core pieces:

- `SDLRenderer`: SDL GPU device, swapchain, viewport targets, and frame submission
- `TriangleRenderer`: simple instanced triangle rendering
- `LineRenderer`: immediate-style debug line rendering
- `QuadtreeMeshRenderer`: terrain draw path, compute heightmap generation, and async extents readback
- `WorldGridQuadtreeWaterManager`: CPU-side water leaf selection and staging
- `QuadtreeWaterMeshRenderer`: water draw path, FFT compute passes, and shared displacement/slope maps
- `SkyboxRenderer`: fullscreen skybox pass using a cubemap and inverse view-projection ray reconstruction
- `WorldGridQuadtree`: quadtree update, draw selection, subdivision/collapse, and debug draw emission
- `WorldGridQuadtreeHeightmapManager`: heightmap residency, LRU replacement, and compute dispatch queueing
- `CameraManager` + `FreeFlightCameraController`: camera storage and free-flight controls
- `LightingSystem`: global sun direction, color, intensity, and time-of-day cycle controls
- `PerformanceCapture` + `PerfPanel`: frame timing and flame graph UI

## Current Terrain Pipeline

Each frame, terrain and water work are split into a few clear phases:

1. `beginHeightmapUpdate`
   Completed GPU extents readbacks are collected and folded back into the heightmap manager and any live quadtree nodes.
2. `updateTree`
   The quadtree updates from cached residency/extents state, decides subdivision/collapse, and marks nodes for drawing.
3. `endHeightmapUpdate`
   Queued leaf requests are pushed from the heightmap manager into the compute generation path.
4. `emitWaterDraws`
   Visible quadtree leaves are offered to the water manager, which filters out dry leaves, including leaves whose terrain min height is more than a configurable cutoff above the water level.

The renderer then:

1. uploads draw-instance data
2. dispatches compute jobs for queued terrain slices
3. dispatches water simulation:
   - optional one-time spectrum initialization when water settings change
   - per-frame spectrum evolution from cached `h0`
   - shared-memory FFT passes over SSBO working buffers
   - displacement/slope map assembly into texture arrays
4. queues an async terrain extents download
5. renders terrain, water, triangles, and debug lines into the offscreen viewport
   - the water vertex shader samples the matching resident terrain slice when available
   - local depth is used to damp wave motion in shallow water, with per-cascade damping strengths
   - the water fragment shader blends sky/atmosphere reflection, depth-based absorption/scattering, and a shallow refracted terrain sample from the resident terrain heightmap
6. renders the skybox as a fullscreen quad using the cubemap and the current time-of-day rotation
7. renders ImGui

## Features

- Docked `Info` and `Viewport` panes with ImGui docking
- Offscreen SDL GPU viewport displayed inside the UI
- Free-flight camera controls with gamepad support
- Multiple cameras with active-camera switching
- Triangle instance editing in grid/local coordinates
- Quadtree terrain rendering with runtime residency and subdivision
- `259 x 259` heightmap slices with halo samples and a reusable terrain patch mesh
- GPU compute terrain generation with async GPU extents readback
- Shared-cascade FFT water with one reusable instanced water mesh
- Four `512 x 512` water cascades sampled in world space across all visible water leaves
- SSBO-backed FFT working set with shared-memory butterfly passes
- Precomputed static water spectrum initialization, rebuilt only when water settings change
- Terrain-aware shallow-water damping by sampling the resident terrain heightmap slice under each water patch
- Per-cascade shallow-water damping strengths so small and large wave bands can fade differently near shore
- Sky/atmosphere-aware PBR-style water shading using the same cubemap and atmosphere LUT as the skybox pass
- Depth-driven water color, absorption, and transmission beyond the shoreline band
- Shallow refracted terrain lookup from resident terrain heightmap data, without screen-space scene color sampling
- Skybox cubemap rendering from `resources/skybox`
- Time-of-day controls that rotate both the sun and skybox around a shared orbit axis
- Runtime terrain tuning:
  - base height
  - base wavelength
  - initial frequency
  - initial amplitude
  - octave count
  - octave frequency scale
  - octave amplitude scale
  - gradient dampening `k`
  - compute dispatches per frame
- Quadtree debug bounds using real extents when known, falling back to a flat square when not
- Gouraud terrain shading with a tunable global sun light
- Automatic day/night progression with configurable day length and time factor
- Built-in frame graph and CPU flame graph
- Water tuning UI for level, amplitude, cutoffs, cascade parameters, terrain-height water culling, and shallow-water damping controls

## Project Layout

- `src/App.*`: app lifetime, frame loop, and scene/update orchestration
- `src/AppPanels.*`: UI layout and editor panels
- `src/SDLRenderer.*`: SDL GPU setup and frame submission
- `src/QuadtreeMeshRenderer.*`: terrain draw path and compute integration
- `src/QuadtreeWaterMeshRenderer.*`: water draw path and FFT compute integration
- `src/SkyboxRenderer.*`: cubemap loading and fullscreen skybox rendering
- `src/WorldGridQuadtree.*`: quadtree state and traversal
- `src/WorldGridQuadtreeHeightmapManager.*`: heightmap LRU and compute queue management
- `src/WorldGridQuadtreeWaterManager.*`: water draw filtering and leaf staging
- `src/WorldGridQuadtreeDebugRenderer.*`: quadtree debug box rendering
- `src/HeightmapNoiseGenerator.*`: shared terrain noise settings and CPU-side sampling helpers
- `src/TriangleRenderer.*`: triangle rendering
- `src/LineRenderer.*`: debug line rendering
- `src/CameraManager.*`: camera storage and projection building
- `src/FreeFlightCameraController.*`: free-flight controls
- `src/LightingSystem.*`: sun-light state
- `resources/skybox/*.png`: cubemap faces for the skybox
- `src/PerformanceCapture.*`: timing capture
- `src/PerfPanel.*`: performance UI
- `src/Position.hpp`: large-world grid/local position type
- `src/WorldGridQuadtreeTypes.hpp`: quadtree leaf ids and bounds helpers
- `src/AppConfig.hpp`: shared constants and tuning defaults
- `shaders/triangle.*`: triangle shaders
- `shaders/line.*`: line shaders
- `shaders/quadtree_mesh.*`: terrain graphics shaders
- `shaders/skybox.*`: skybox fullscreen shaders
- `shaders/heightmap_generate.comp`: terrain compute shader
- `shaders/water_mesh.*`: water graphics shaders
- `shaders/water_initialize_spectrum.comp`: one-time static water spectrum initialization
- `shaders/water_spectrum_update.comp`: per-frame spectrum evolution from cached initial spectrum
- `shaders/water_fft_stage.comp`: shared-memory 1D FFT pass over water working buffers
- `shaders/water_build_maps.comp`: displacement/slope texture assembly for the water draw path

## Dependencies

- CMake 3.25+
- A C++20 compiler
- Vulkan SDK with `glslc`
- Internet access during configure time for `FetchContent`

Fetched dependencies:

- SDL `release-3.4.0`
- SDL_image `release-3.2.4`
- Dear ImGui `v1.92.5-docking`
- GLM `1.0.1`

## Configure And Build

From the repo root:

```powershell
tools\configure.cmd
tools\build.cmd
```

For a debug build:

```powershell
tools\configure.cmd Debug
tools\build.cmd Debug
```

If the build directory already exists and only source changed, you can usually skip configure:

```powershell
tools\build.cmd
```

## Run

Release:

```powershell
.\build\Release\terrain_sandbox.exe
```

Debug:

```powershell
.\build\Debug\terrain_sandbox.exe
```

Startup flags:

```powershell
.\build\Debug\terrain_sandbox.exe --verbose-startup --quit-after-first-frame
```

- `--verbose-startup`: print startup progress
- `--quit-after-first-frame`: submit one frame and exit

## Notes

- The rendering API is SDL GPU, with shaders compiled by `glslc`.
- Skybox faces are loaded through SDL_image and uploaded into an SDL GPU cubemap texture.
- World positions use large horizontal grid cells plus local coordinates for stable camera-relative rendering.
- The maintained terrain path is the runtime GPU compute path; the older standalone quadtree sanity executable has been removed.
- Terrain extents are produced on the GPU and read back asynchronously, so newly generated slices may briefly fall back to conservative bounds until their readback completes.
- The quadtree processes all allocated nodes during update; draw selection is separate from subdivision/collapse decisions.
- Water is sampled globally by world position; visible quadtree leaves become draw instances and do not own unique simulation state.
- Water visibility currently uses both expanded quadtree bounds and a terrain-height gate, so leaves that are clearly dry can skip water entirely while still allowing low terrain under the water plane to remain visible.
- When a matching resident terrain slice exists, the water draw samples that heightmap directly to estimate local depth beneath the patch. That depth signal now affects draw-time damping, water color/transmission, and a shallow refracted terrain lookup, but it still does not feed back into the shared FFT simulation itself.
- The skybox is drawn at the end of the viewport pass on background pixels only, using a fullscreen quad and inverse view-projection reconstruction so it can later grow into atmospheric rendering.
- Shader binaries are emitted into the active build directory, for example `build/Debug/shaders`.
