# SDL3 GPU + ImGui Terrain Sandbox

This project is a small editor-style sandbox built around SDL3 GPU, Dear ImGui docking, quadtree terrain rendering, and shared-cascade FFT water.

## Overview

The app renders into an offscreen viewport that is shown inside an ImGui layout. Terrain is managed as quadtree patches, with heightmaps cached in an LRU and rendered through a reusable indexed patch mesh. Heightmaps are generated on the GPU with a compute shader, and per-slice min/max extents are reduced on the GPU and read back asynchronously for culling, subdivision, and debug bounds.

Water follows the same broad ownership split as terrain, but stays globally simulated:

- `WorldGridQuadtree` decides which leaves are visible
- `WorldGridQuadtreeWaterManager` decides which of those leaves should draw water
- `QuadtreeWaterMeshRenderer` owns the reusable water meshes, FFT compute resources, and water draw path
- `SDLRenderer` orchestrates the terrain and water compute/render order each frame

The current water path uses four shared FFT cascades at `512 x 512`, with one reusable interior water patch mesh plus reusable equal-LOD and `2:1` bridge meshes instanced across visible leaves. The simulation uses precomputed static spectrum data, SSBO-backed FFT working buffers, a shared-memory butterfly FFT pass, and final displacement/slope texture arrays sampled in world space by the water draw shaders. The water draw also reuses the resident terrain heightmap slices to estimate local water depth for shallow-water displacement damping, depth-based transparency shaping, and shoreline-aware transmission over the already rendered terrain. Water shading is now driven by a sky/atmosphere-aware PBR-style surface model that samples the same cubemap and atmosphere LUT used by the skybox pass, with crest foam generated from wave compression and accumulated in a separate persistent foam history texture. The terrain fragment path also samples the shared water displacement and slope maps to drive underwater caustics on submerged terrain. The background sky and water reflection atmosphere probes both treat the atmosphere as a deep participating medium instead of cutting off at `y = 0`.

Core pieces:

- `SDLRenderer`: SDL GPU device, swapchain, viewport targets, and frame submission
- `TriangleRenderer`: simple instanced triangle rendering
- `LineRenderer`: immediate-style debug line rendering
- `QuadtreeMeshRenderer`: terrain draw path, compute heightmap generation, and async extents readback
- `WorldGridQuadtreeWaterManager`: CPU-side water leaf selection and staging
- `QuadtreeWaterMeshRenderer`: water draw path, FFT compute passes, and shared displacement/slope/foam maps
- `SkyboxRenderer`: fullscreen skybox pass using a cubemap and inverse view-projection ray reconstruction
- `WorldGridQuadtree`: quadtree update, draw selection, subdivision/collapse, tree-based neighbor lookup for stitching, and debug draw emission
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
5. `emitMeshDraws`
   Visible terrain and water leaves emit their base patch draws plus stitch-bridge draws, using tree traversal through parent/child links to find same-LOD, finer, and `2:1` coarser edge neighbors without scanning all allocated nodes.

The renderer then:

1. sorts terrain and water draw instances front-to-back, then uploads draw-instance data
2. dispatches compute jobs for queued terrain slices
3. dispatches water simulation:
   - optional one-time spectrum initialization when water settings change
   - per-frame spectrum evolution from cached `h0`
   - shared-memory FFT passes over SSBO working buffers
   - displacement/slope map assembly plus crest-foam generation and persistence into texture arrays
4. queues an async terrain extents download
5. renders terrain, water, triangles, and debug lines into the offscreen viewport
   - terrain uses an interior patch mesh plus equal-LOD and `2:1` bridge meshes to close cracks between quadtree nodes
   - terrain submits both bridge variants through one shared bridge indirect draw
   - submerged terrain can add triplanar Worley-style caustics, with the caustic evolution warped and modulated from the shared water displacement/slope maps
   - water uses the same broad stitch strategy with a trimmed interior base patch plus equal-LOD and `2:1` bridge meshes
   - water also submits both bridge variants through one shared bridge indirect draw
   - the water vertex shader samples the matching resident terrain slice when available
   - local depth is used to damp wave motion in shallow water, with per-cascade damping strengths
   - the water fragment shader blends sky/atmosphere reflection, depth-based absorption/scattering, depth-driven transparency over terrain, and persistent crest foam
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
- Terrain crack stitching with reusable equal-LOD and `2:1` bridge meshes
- Terrain equal-LOD and `2:1` bridge draws batched through one indirect bridge submission
- GPU compute terrain generation with async GPU extents readback
- Shared-cascade FFT water with an interior base mesh plus reusable equal-LOD and `2:1` bridge meshes
- Water equal-LOD and `2:1` bridge draws batched through one indirect bridge submission
- Four `512 x 512` water cascades sampled in world space across visible water leaves, with cascade usage reduced on larger quadtree patches
- SSBO-backed FFT working set with shared-memory butterfly passes
- Precomputed static water spectrum initialization, rebuilt only when water settings change
- Crest foam generated from local wave compression, with temporal persistence and decay in a dedicated ping-pong history texture
- Terrain-aware shallow-water damping by sampling the resident terrain heightmap slice under each water patch
- Per-cascade shallow-water damping strengths so small and large wave bands can fade differently near shore
- Depth-driven water surface transparency over already rendered terrain instead of a local shallow-refraction raymarch
- Underwater terrain caustics from a triplanar Worley-style pattern warped and modulated by the shared FFT water displacement/slope field
- Tiered water cascade selection by patch size:
  - `256` and `512` meter patches use all 4 cascades
  - `1024` and `2048` meter patches use the top 3 cascades
  - `4096` and `8192` meter patches use the top 2 cascades
  - larger patches use only the top cascade
- Sky/atmosphere-aware PBR-style water shading using the same cubemap and atmosphere LUT as the skybox pass
- Depth-driven water color, absorption, transmission, and transparency beyond the shoreline band
- Skybox cubemap rendering from `resources/skybox`
- Deep-atmosphere sky and water reflection probes that do not clamp atmospheric traversal to a ground plane at `y = 0`
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
- Tree-based quadtree neighbor traversal for terrain and water stitching, including world-grid border handoff across the active `3 x 3` base-node set
- Terrain shading with a tunable global sun light plus underwater caustic lighting on submerged terrain
- Automatic day/night progression with configurable day length and time factor
- Built-in frame graph and CPU flame graph
- Water tuning UI for level, amplitude, cutoffs, per-cascade parameters including wind direction, terrain-height water culling, shallow-water damping, and crest-foam controls

## Project Layout

- `src/App.*`: app lifetime, frame loop, and scene/update orchestration
- `src/AppPanels.*`: UI layout and editor panels
- `src/SDLRenderer.*`: SDL GPU setup and frame submission
- `src/QuadtreeMeshRenderer.*`: terrain draw path, bridge meshes, and compute integration
- `src/QuadtreeWaterMeshRenderer.*`: water draw path, bridge meshes, and FFT compute integration
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
- `shaders/quadtree_mesh_bridge.vert`: terrain bridge vertex shader
- `shaders/skybox.*`: skybox fullscreen shaders
- `shaders/heightmap_generate.comp`: terrain compute shader
- `shaders/water_mesh.*`: water graphics shaders
- `shaders/water_mesh_bridge.vert`: water bridge vertex shader
- `shaders/water_initialize_spectrum.comp`: one-time static water spectrum initialization
- `shaders/water_spectrum_update.comp`: per-frame spectrum evolution from cached initial spectrum
- `shaders/water_fft_stage.comp`: shared-memory 1D FFT pass over water working buffers
- `shaders/water_build_maps.comp`: displacement/slope texture assembly plus crest-foam generation and persistence

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

Startup logging:

- startup progress is also written to `launch.log`
- the app truncates `launch.log` at process start, so each normal launch begins with a fresh log
- `launch.log` is ignored by Git

Windows GPU preference:

- the executable exports `NvOptimusEnablement=1` and `AmdPowerXpressRequestHighPerformance=1` so compatible driver stacks can prefer the high-performance GPU

## Notes

- The rendering API is SDL GPU, with shaders compiled by `glslc`.
- Skybox faces are loaded through SDL_image and uploaded into an SDL GPU cubemap texture.
- World positions use large horizontal grid cells plus local coordinates for stable camera-relative rendering.
- The maintained terrain path is the runtime GPU compute path; the older standalone quadtree sanity executable has been removed.
- Terrain extents are produced on the GPU and read back asynchronously, so newly generated slices may briefly fall back to conservative bounds until their readback completes.
- The quadtree processes all allocated nodes during update; draw selection is separate from subdivision/collapse decisions.
- Stitching lookups now follow the quadtree structure directly through parent/child links and only hop across the active base-node ring when a lookup reaches a world-grid border.
- Terrain and water patch instances are sorted front-to-back before upload so nearer patches submit first and can improve depth rejection of farther terrain.
- Water is sampled globally by world position; visible quadtree leaves become draw instances and do not own unique simulation state.
- Terrain and water both render trimmed interior base patches and add edge-specific bridge meshes where neighboring quadtree nodes would otherwise leave a crack.
- Water visibility currently uses both expanded quadtree bounds and a terrain-height gate, so leaves that are clearly dry can skip water entirely while still allowing low terrain under the water plane to remain visible.
- Water patch LOD also controls which shared wave cascades are sampled, so larger/farther quadtree patches shed the finest wave bands instead of always using all four maps.
- When a matching resident terrain slice exists, the water draw samples that heightmap directly to estimate local depth beneath the patch. That depth signal now affects draw-time damping, water color/transmission, and surface transparency, but it still does not feed back into the shared FFT simulation itself.
- Submerged terrain shading samples the shared water displacement and slope maps to drive triplanar Worley-style fake caustics, so the caustic motion evolves with the same FFT water field instead of using an unrelated scrolling overlay.
- Crest foam is generated during the map-build pass from horizontal-displacement compression, then accumulated with decay in a separate foam history texture rather than being packed into displacement alpha.
- The skybox is drawn at the end of the viewport pass on background pixels only, using a fullscreen quad and inverse view-projection reconstruction. Both the skybox path and the water reflection probe treat the atmosphere as a deep medium rather than intersecting a hard `y = 0` ground plane.
- Shader binaries are emitted into the active build directory, for example `build/Debug/shaders`.
