# SDL3 GPU + ImGui Terrain Sandbox

This project is a small editor-style sandbox built around SDL3 GPU, Dear ImGui docking, and a quadtree terrain renderer.

## Overview

The app renders into an offscreen viewport that is shown inside an ImGui layout. Terrain is managed as quadtree patches, with heightmaps cached in an LRU and rendered through a reusable indexed patch mesh. Heightmaps are generated on the GPU with a compute shader, and per-slice min/max extents are reduced on the GPU and read back asynchronously for culling, subdivision, and debug bounds.

Core pieces:

- `SDLRenderer`: SDL GPU device, swapchain, viewport targets, and frame submission
- `TriangleRenderer`: simple instanced triangle rendering
- `LineRenderer`: immediate-style debug line rendering
- `QuadtreeMeshRenderer`: terrain draw path, compute heightmap generation, and async extents readback
- `WorldGridQuadtree`: quadtree update, draw selection, subdivision/collapse, and debug draw emission
- `WorldGridQuadtreeHeightmapManager`: heightmap residency, LRU replacement, and compute dispatch queueing
- `CameraManager` + `FreeFlightCameraController`: camera storage and free-flight controls
- `LightingSystem`: global sun direction, color, and intensity
- `PerformanceCapture` + `PerfPanel`: frame timing and flame graph UI

## Current Terrain Pipeline

Each frame, terrain work is split into three phases:

1. `beginHeightmapUpdate`
   Completed GPU extents readbacks are collected and folded back into the heightmap manager and any live quadtree nodes.
2. `updateTree`
   The quadtree updates from cached residency/extents state, decides subdivision/collapse, and marks nodes for drawing.
3. `endHeightmapUpdate`
   Queued leaf requests are pushed from the heightmap manager into the compute generation path.

The renderer then:

1. uploads draw-instance data
2. initializes per-slice extents sentinels
3. dispatches compute jobs for queued terrain slices
4. queues an async extents download
5. renders the terrain, triangles, debug lines, and ImGui

## Features

- Docked `Info` and `Viewport` panes with ImGui docking
- Offscreen SDL GPU viewport displayed inside the UI
- Free-flight camera controls with gamepad support
- Multiple cameras with active-camera switching
- Triangle instance editing in grid/local coordinates
- Quadtree terrain rendering with runtime residency and subdivision
- `259 x 259` heightmap slices with halo samples and a reusable terrain patch mesh
- GPU compute terrain generation with async GPU extents readback
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
- Built-in frame graph and CPU flame graph

## Project Layout

- `src/App.*`: app lifetime, frame loop, and scene/update orchestration
- `src/AppPanels.*`: UI layout and editor panels
- `src/SDLRenderer.*`: SDL GPU setup and frame submission
- `src/QuadtreeMeshRenderer.*`: terrain draw path and compute integration
- `src/WorldGridQuadtree.*`: quadtree state and traversal
- `src/WorldGridQuadtreeHeightmapManager.*`: heightmap LRU and compute queue management
- `src/WorldGridQuadtreeDebugRenderer.*`: quadtree debug box rendering
- `src/HeightmapNoiseGenerator.*`: CPU reference terrain sampling and shared settings
- `src/TriangleRenderer.*`: triangle rendering
- `src/LineRenderer.*`: debug line rendering
- `src/CameraManager.*`: camera storage and projection building
- `src/FreeFlightCameraController.*`: free-flight controls
- `src/LightingSystem.*`: sun-light state
- `src/PerformanceCapture.*`: timing capture
- `src/PerfPanel.*`: performance UI
- `src/Position.hpp`: large-world grid/local position type
- `src/WorldGridQuadtreeTypes.hpp`: quadtree leaf ids and bounds helpers
- `src/AppConfig.hpp`: shared constants and tuning defaults
- `shaders/triangle.*`: triangle shaders
- `shaders/line.*`: line shaders
- `shaders/quadtree_mesh.*`: terrain graphics shaders
- `shaders/heightmap_generate.comp`: terrain compute shader

## Dependencies

- CMake 3.25+
- A C++20 compiler
- Vulkan SDK with `glslc`
- Internet access during configure time for `FetchContent`

Fetched dependencies:

- SDL `release-3.4.0`
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
.\build\Release\hello_triangle.exe
```

Debug:

```powershell
.\build\Debug\hello_triangle.exe
```

Startup flags:

```powershell
.\build\Debug\hello_triangle.exe --verbose-startup --quit-after-first-frame
```

- `--verbose-startup`: print startup progress
- `--quit-after-first-frame`: submit one frame and exit

## Notes

- The rendering API is SDL GPU, with shaders compiled by `glslc`.
- World positions use large horizontal grid cells plus local coordinates for stable camera-relative rendering.
- Terrain extents are produced on the GPU and read back asynchronously, so newly generated slices may briefly fall back to conservative bounds until their readback completes.
- The quadtree processes all allocated nodes during update; draw selection is separate from subdivision/collapse decisions.
- Shader binaries are emitted into the active build directory, for example `build/Debug/shaders`.
