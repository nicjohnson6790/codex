# SDL3 GPU Terrain Sandbox

![rendered screenshot](images/Screenshot%202026-05-16%20175430.png)

An editor-style terrain sandbox built on SDL3 GPU and Dear ImGui. It combines large-world quadtree terrain, GPU-generated foliage, FFT water, atmospheric sky rendering, debug primitives, and immediate-mode world-space MSDF text.

The authored pine tree source pack is not included, so the app can build from this repo, but the nearby tree mesh runtime assets cannot be regenerated end to end without that external content.

## Features

- SDL3 GPU renderer presented inside an ImGui docking UI
- Large-world camera/player flow using grid/local `Position` coordinates
- Quadtree terrain with GPU heightmap generation and resident slice management
- Terrain material blending from generated PBR runtime texture arrays
- Nearby foliage meshes, mid-distance imposters, and far-canopy procedural shells
- Shared-cascade FFT water with bridge meshes, shoreline foam, and terrain-aware response
- Skybox and atmosphere composite driven by shared lighting/time-of-day state
- Immediate-mode world text using generated Roboto MSDF assets, instanced indirect groups, depth testing, stroke, and glow/drop shadow styling
- Offline converter for `meshbin`, `texbin`, and `assetbin` runtime packs
- ImGui performance panel with frame scopes, flame graph zooming, and timing details

## Frame Shape

`App` owns lifecycle, input, simulation, scene emission, and UI. `SDLRenderer` owns the SDL GPU device, viewport targets, pass order, and command submission.

Per frame, the app updates terrain/foliage/water residency, schedules GPU generation work, uploads emitted draw data, runs compute passes, then renders the viewport:

1. Terrain, water, foliage, canopy, debug triangles/lines, and world text render into the main color/depth targets.
2. Skybox and atmosphere composite over the viewport color using the depth texture.
3. ImGui renders the viewport texture and editor UI to the swapchain.

## Key Files

- `src/App.*`: application lifetime, frame sequencing, UI wiring, and scene emission
- `src/SDLRenderer.*`: SDL GPU setup, compute dispatch order, render pass order, and presentation
- `src/WorldGridQuadtree.*`: fixed-slot quadtree, LOD decisions, visibility, and draw/cache emission
- `src/QuadtreeMeshRenderer.*`: terrain mesh rendering, bridge meshes, heightmap generation, and extent readback
- `src/QuadtreeWaterMeshRenderer.*`: FFT water simulation and water mesh rendering
- `src/*Foliage*.*`, `src/NearbyFoliageRenderer.*`: foliage residency, generation, imposters, nearby decode, and canopy rendering
- `src/SkyboxRenderer.*`: cubemap loading, atmosphere LUT generation, and fullscreen sky composite
- `src/WorldTextRenderer.*`: immediate-mode world-space Roboto MSDF text renderer
- `src/assets/*`: runtime asset binary formats, compression, and readers
- `tools/converter/*`: offline asset converter
- `shaders/*`: GLSL shaders compiled to SPIR-V during the build

## Build

Requirements:

- Windows with Visual Studio C++ tools
- CMake 3.25+
- Ninja
- Vulkan SDK with `glslc`
- Optional: Steamworks SDK 1.64 at `../deps/steamworks_sdk_164/sdk`, or configure with `-DSTEAMWORKS_SDK_DIR=...`
- Internet access during first configure for `FetchContent`

From the repo root:

```powershell
tools\configure.cmd Debug
tools\build.cmd Debug
.\build\Debug\terrain_sandbox.exe
```

Useful variants:

```powershell
tools\build.cmd Release
tools\build.cmd Assets
.\build\Debug\terrain_sandbox.exe --disable-steam
.\build\Debug\terrain_sandbox.exe --verbose-startup --quit-after-first-frame
```

`tools\build.cmd Assets` builds `build\Assets\converter.exe` and regenerates supported runtime packs under `assets/runtime`.

## Runtime Assets

Generated runtime assets are staged from `assets/runtime` into `build/<Config>/assets/runtime`.

Supported converter packs:

- `skybox.assetbin`, `skybox.texbin`
- `pbr.assetbin`, `pbr.texbin`
- `pinetreepack.assetbin`, `pinetreepack.meshbin`, `pinetreepack.texbin`
- `roboto.assetbin`, `roboto.texbin`

Included source assets:

- `assets/source/skybox/tex`
- `assets/source/pbr/tex`
- `assets/source/font`

Missing external source assets:

- `assets/source/pinetreepack/fbx`
- `assets/source/pinetreepack/tex`

The Roboto font pack is converted into a `1024x1024` MSDF atlas plus glyph metrics. `WorldTextRenderer` consumes those records for baseline/centered, single-line/multiline, justified world-space text.

For converter details, see [tools/converter/README.md](tools/converter/README.md).
