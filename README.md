# SDL3 GPU + ImGui Terrain Sandbox

![rendered screenshot](images/Screenshot%202026-05-09%20162818.png)

An editor-style terrain sandbox built on SDL3 GPU, Dear ImGui docking, quadtree terrain rendering, GPU-generated foliage, player follow controls, procedural far-canopy coverage, and shared-cascade FFT water.

This branch does not include the authored pine tree source assets needed to rebuild or fully run the nearby tree mesh path. See `Assets` below.

## Features

- SDL3 GPU renderer presented inside an ImGui docking UI
- Quadtree terrain with GPU-generated heightmaps and async min/max extent readback
- Reusable terrain patch meshes with equal-LOD and `2:1` crack-stitch bridge meshes
- Terrain PBR material blending from generated `pbr` runtime texture arrays
- GPU foliage generation into canonical `256m x 256m` resident pages
- Nearby foliage path that decodes local foliage pages and renders real pine meshes with deterministic yaw, alpha-tested depth prepass, and a short dithered handoff band near the imposter range
- Mid-distance foliage imposter path that renders BC-compressed pine imposter texture arrays with runtime lighting and an alpha-tested depth prepass
- Far-canopy path that renders deterministic procedural coverage as 3 terrain-following shells, with 1024m fallback and 2048m state-driven fade
- Shared-cascade FFT water with equal-LOD and `2:1` bridge meshes, shallow-water damping, crest foam, shoreline foam, and terrain-aware depth response
- Free-flight and third-person follow cameras with position-preserving handoff and terrain-aware follow-camera clamping
- Skybox and water shading driven by the same cubemap and atmosphere model
- Offline asset converter that builds compressed runtime `meshbin`, `texbin`, and `assetbin` packs
- Self-contained build outputs with shaders and runtime assets staged under `build/<Config>`

## Frame Flow

Each frame is split into a few predictable phases:

- Collect completed terrain readbacks into the heightmap manager.
- Update the quadtree structure and terrain residency. Parents stay leaves until children are resident; children stay active until a collapsing parent is resident.
- Emit from a linear scan of the fixed node list. Terrain draws use resident heightmaps; foliage, canopy, nearby foliage, and water request their own residency as they emit.
- Schedule queued terrain, foliage, canopy, and nearby decode work.
- Upload per-frame draw data, dispatch terrain generation, dispatch foliage generation, dispatch canopy generation, dispatch nearby decode work, and run the water simulation passes.
- Render terrain, foliage, nearby foliage, canopy shells, water, skybox, debug overlays, and ImGui into the offscreen viewport.

## Architecture

The runtime is split into four main layers:

- `SDLRenderer` owns the SDL GPU device, swapchain, render targets, and top-level frame submission order.
- `App` owns lifecycle and frame sequencing, with windowing, input, UI, simulation, scene emission, and rendering kept as named phases.
- Renderer classes own concrete draw and compute paths such as terrain, foliage, nearby foliage, canopy, water, skybox, triangles, and debug lines.
- Manager classes own residency, LRU replacement, and queued generation work for terrain slices, foliage pages, canopy cells, and water leaf selection.
- `WorldGridQuadtree` owns the fixed-slot tree structure, LOD decisions, terrain residency update, and linear draw/cache emission.

That split keeps app flow explicit, the quadtree responsible for scene decisions, managers responsible for lifetime and queues, and renderers responsible for GPU work.

## File Index

### App and frame orchestration

- `src/main.cpp`: process entry point and app startup
- `src/App.*`: application lifetime, per-frame phase sequencing, and system wiring
- `src/AppPanels.*`: ImGui layout and editor panels
- `src/AppConfig.hpp`: shared runtime constants and defaults

### Core rendering

- `src/SDLRenderer.*`: SDL GPU setup, render targets, compute dispatch order, and frame submission
- `src/EngineRendererBase.*`: small shared base helpers for renderer classes
- `src/RenderEngines.hpp`: renderer ownership bundle types
- `src/RenderTypes.hpp`: shared render-facing data types
- `src/SceneTypes.hpp`: shared scene-side data passed into rendering

### Terrain

- `src/WorldGridQuadtree.*`: quadtree traversal, visibility, subdivision, neighbor lookup, and draw emission
- `src/WorldGridQuadtreeTypes.hpp`: quadtree ids, bounds, and helper types
- `src/WorldGridQuadtreeDebugRenderer.*`: quadtree debug bounds rendering
- `src/WorldGridQuadtreeHeightmapManager.*`: terrain slice residency, LRU replacement, and generation queueing
- `src/QuadtreeMeshRenderer.*`: terrain mesh draw path, equal-LOD and `2:1` bridge meshes, compute generation, and extents readback
- `src/HeightmapNoiseGenerator.*`: procedural terrain noise settings and sampling helpers

### Foliage

- `src/WorldGridFoliageManager.*`: canonical foliage page residency and GPU generation scheduling
- `src/WorldGridFoliageCanopyManager.*`: canonical canopy cell residency and GPU generation scheduling
- `src/FoliageImposterRenderer.*`: mid-distance foliage imposter page pool decode, runtime asset loading, and imposter rendering
- `src/NearbyFoliageRenderer.*`: nearby decoded-page readback, CPU cache, runtime asset loading, and nearby tree mesh rendering
- `src/FoliageCanopyRenderer.*`: far-canopy bitset generation and 3-shell canopy rendering
- `src/FoliageTypes.hpp`: shared foliage packing and runtime layout types

### Water and sky

- `src/WorldGridQuadtreeWaterManager.*`: water leaf filtering and staging
- `src/QuadtreeWaterMeshRenderer.*`: FFT water simulation resources, equal-LOD and `2:1` bridge meshes, and water rendering
- `src/WaterSettings.hpp`: editable water tuning values
- `src/WaterTypes.hpp`: shared water data layouts
- `src/SkyboxRenderer.*`: skybox runtime asset loading and fullscreen sky rendering

### Camera, lighting, and tools

- `src/CameraManager.*`: camera storage and projection setup
- `src/FreeFlightCameraController.*`: free-flight camera movement
- `src/Gameplay.*`: player movement, collision cache, and third-person follow camera
- `src/GamepadInput.*`: gamepad state and input helpers
- `src/LightingSystem.*`: sun direction, intensity, and time-of-day control
- `src/TriangleRenderer.*`: simple upright triangle marker rendering
- `src/LineRenderer.*`: debug line rendering
- `src/PerformanceCapture.*`: frame timing capture
- `src/PerfPanel.*`: timing and flame graph UI
- `src/Position.hpp`: large-world grid/local position representation
- `src/SubmittedGpuFence.hpp`: fence tracking helper

### Runtime assets

- `src/assets/RuntimeAssetFormat.hpp`: binary layout definitions for `meshbin`, `texbin`, and `assetbin`
- `src/assets/RuntimeAssetCompression.*`: shared LZ4 compression and decompression helpers
- `src/assets/RuntimeAssetReader.*`: validated SDL file readers for runtime asset packs
- `assets/source/skybox/tex/*.png`: included skybox source textures for the converter
- `assets/runtime/*`: generated runtime asset packs staged into build outputs
- `tools/converter/*`: standalone offline converter for source asset import and runtime pack generation

### Shaders

- `shaders/quadtree_mesh.*`: terrain shaders
- `shaders/quadtree_mesh_bridge.vert`: terrain bridge vertex shader
- `shaders/heightmap_generate.comp`: terrain generation compute shader
- `shaders/foliage_common.glsl`: shared deterministic foliage candidate helpers
- `shaders/foliage_generate.comp`: foliage page generation compute shader
- `shaders/foliage_imposter.*`: mid-distance foliage imposter shaders
- `shaders/foliage_imposter_depth_prepass.frag`: mid-distance foliage imposter alpha-tested depth prepass shader
- `shaders/nearby_foliage_decode.comp`: nearby foliage decode compute shader
- `shaders/nearby_foliage.*`: nearby tree mesh shaders
- `shaders/nearby_foliage_depth_prepass.frag`: nearby tree mesh alpha-tested depth prepass shader
- `shaders/foliage_canopy_generate.comp`: canopy coverage compute shader
- `shaders/foliage_canopy.*`: far-canopy shaders
- `shaders/water_initialize_spectrum.comp`: one-time water spectrum initialization
- `shaders/water_spectrum_update.comp`: per-frame spectrum evolution
- `shaders/water_fft_stage.comp`: FFT stage compute shader
- `shaders/water_build_maps.comp`: displacement, slope, and foam map build pass
- `shaders/water_mesh.*`: water shaders
- `shaders/water_mesh_bridge.vert`: water bridge vertex shader
- `shaders/skybox.*`: fullscreen skybox shaders
- `shaders/triangle.*`: triangle shaders
- `shaders/line.*`: debug line shaders

## Build

Requirements:

- CMake 3.25+
- Ninja
- A C++20 compiler
- Vulkan SDK with `glslc`
- Internet access during configure time for `FetchContent`

Common commands from the repo root:

```powershell
tools\configure.cmd
tools\build.cmd
```

```powershell
tools\configure.cmd Debug
tools\build.cmd Debug
```

```powershell
tools\build.cmd Release
```

Asset pack build:

```powershell
tools\build.cmd Assets
```

Run:

```powershell
.\build\Debug\terrain_sandbox.exe
```

```powershell
.\build\Debug\terrain_sandbox.exe --verbose-startup --quit-after-first-frame
```

`tools\build.cmd Assets` builds the standalone converter in `build\Assets` and regenerates:

- `skybox.assetbin`
- `skybox.texbin`
- `pinetreepack.assetbin`
- `pinetreepack.meshbin`
- `pinetreepack.texbin`
- `pbr.assetbin`
- `pbr.texbin`

Current pine runtime asset details:

- imported source material textures are normalized to `1024x1024` for the runtime pack
- imposter capture renders from the original source textures, not the resized runtime copies
- each pine imposter texture set is generated as `8` yaw views x `4` pitch views
- final imposter arrays are `512x512`, `32` layers, full mip chain
- color/alpha imposters are stored as `BC3`
- normal imposters are stored as `BC5`

Current PBR runtime asset details:

- source textures are imported from `assets/source/pbr/tex`
- runtime textures are normalized to `1024x1024`
- full mip chains are generated before BC compression
- albedo maps are stored as `BC3` sRGB
- normal maps are stored as `BC5`
- roughness, AO, height, metallic, and similar scalar maps are stored as `BC3` UNORM

## Assets

This repository does not include the authored pine tree pack source content.

- Missing source content: `assets/source/pinetreepack`
- Included source content: `assets/source/skybox/tex`
- Generated runtime outputs are expected under `assets/runtime`
- The main app stages those runtime bins into `build/<Config>/assets/runtime`

Without the external pine content, the project still builds, but the nearby foliage mesh path cannot be regenerated end to end.

For converter details, see [tools/converter/README.md](C:/Users/siarr/source/repos/codex/tools/converter/README.md).
