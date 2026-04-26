# SDL3 GPU + ImGui World Grid Sandbox

This project is a small editor-style sandbox built around:

- SDL3 windowing and gamepad input
- SDL3 GPU as the rendering API
- Dear ImGui docking UI
- An offscreen viewport rendered into an ImGui panel
- Immediate-mode style engine renderers for triangles and lines
- A global-grid-aware camera and position system
- A quadtree world-grid debug overlay
- A quadtree-driven terrain mesh renderer with heightmap LRU management
- A simple global sun light with Gouraud terrain shading
- Built-in CPU/frame-time capture with a flame graph

## Current Stack

- `SDLRenderer`: platform renderer that owns SDL GPU device setup, swapchain flow, viewport targets, and presentation
- `TriangleRenderer`: immediate-style triangle renderer with instanced drawing
- `LineRenderer`: immediate-style 3D line renderer for debug visualization
- `QuadtreeMeshRenderer`: heightmap-backed terrain renderer using indirect indexed draws
- `CameraManager` + `FreeFlightCameraController`: camera storage and free-flight gamepad control
- `WorldGridQuadtree`: dynamic per-cell quadtree generation, transition tracking, and terrain/debug draw submission around the active camera
- `WorldGridQuadtreeHeightmapManager`: quadtree-owned heightmap LRU cache for leaf nodes and parents-of-leaves
- `LightingSystem`: global sun light state used by the terrain path
- `PerfPanel` + `PerformanceCapture`: rolling frame timing, CPU timing, and flame graph inspection

## Project Layout

- `src/App.*`: app lifetime, frame loop, scene update, and high-level wiring
- `src/AppPanels.*`: dock layout and editor panels
- `src/PerfPanel.*`: perf tab UI and flame graph rendering
- `src/SDLRenderer.*`: SDL GPU platform renderer
- `src/TriangleRenderer.*`: instanced triangle rendering
- `src/LineRenderer.*`: immediate-mode debug line rendering
- `src/QuadtreeMeshRenderer.*`: quadtree terrain rendering
- `src/CameraManager.*`: camera storage and projection building
- `src/FreeFlightCameraController.*`: gamepad-driven 6-DoF camera movement
- `src/LightingSystem.*`: global light state
- `src/WorldGridQuadtree.*`: quadtree generation, transition state, visibility counting, and terrain/debug submission
- `src/WorldGridQuadtreeHeightmapManager.*`: heightmap slice cache and upload queue management
- `src/WorldGridQuadtreeDebugRenderer.*`: quadtree debug rendering
- `src/Position.hpp`: global-grid-aware position type
- `src/SceneTypes.hpp`: scene-facing data types
- `src/RenderTypes.hpp`: render-facing data types
- `src/AppConfig.hpp`: shared tuning/config constants
- `shaders/triangle.*`: triangle shaders
- `shaders/line.*`: line shaders

## Features

- Docked `Info` pane on the left and `Viewport` on the right by default
- Gamepad free-flight camera controls
- Multiple cameras with active-camera switching
- Triangle instance editing in grid/local coordinates
- Debug axis rendering and optional quadtree border rendering
- Quadtree terrain rendering with 512 cached `257 x 257` heightmap slices
- Terrain patches render the interior `1..255` sample region, leaving the outer heightmap ring available for border / LOD handling
- CPU-side octave noise height generation feeding an SDL GPU storage buffer
- Simple sun-light controls with Gouraud terrain shading
- Reversed-Z depth with an infinite far plane
- Rolling frame-time graph with CPU overlay
- Click-to-freeze frame inspection and a flame graph in the `Perf` tab
- Compact CPU flame-graph mode that collapses idle/wait time for readability

## Dependencies

- CMake 3.25+
- A C++20 compiler
- Vulkan SDK with `glslc`
- Internet access during configure time for CMake `FetchContent`

The project fetches:

- SDL `release-3.4.0`
- Dear ImGui `v1.92.5-docking`
- GLM `1.0.1`

## Configure And Build

From the repo root:

```powershell
tools\configure.cmd
tools\build.cmd
```

These default to an optimized `Release` build in `build\Release`.
If the requested build directory does not exist yet, the scripts will create it automatically.

For a debug build:

```powershell
tools\configure.cmd Debug
tools\build.cmd Debug
```

If the build directory already exists and only code changed, you can usually skip configure and just run:

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

## Startup Diagnostics

The executable supports two optional flags:

```powershell
.\build\Debug\hello_triangle.exe --verbose-startup --quit-after-first-frame
```

- `--verbose-startup`: prints startup progress to the console
- `--quit-after-first-frame`: exits after the first frame is submitted

These are useful for startup/debugging checks without leaving the app running.

## Notes

- The rendering path is SDL GPU-first, not direct Vulkan-first, though shader compilation still uses `glslc`.
- The viewport is rendered to an offscreen SDL GPU texture and then displayed in ImGui.
- Triangle and debug-line submissions are rebuilt each frame in an immediate-style flow.
- World positions use a large horizontal grid with local coordinates per cell to keep camera-relative rendering stable at large distances.
- The quadtree keeps per-frame transition counts in its debug stats, while resident/queued slice counts are reported by the heightmap manager.
- Shaders are compiled into the active build directory, for example `build/Release/shaders`.
