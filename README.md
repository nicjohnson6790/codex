# SDL3 GPU Terrain Sandbox

![NW of Teshima looking WSW](images/Screenshot%202026-09-04%20181340.png)

Codex is an experimental large-world terrain sandbox and editor built in C++20 with SDL3 GPU and Dear ImGui. It combines streamed quadtree terrain, procedural foliage, FFT water, atmospheric rendering, world-space text, and optional Steam multiplayer in a dockable desktop UI.

## Highlights

- Large-world terrain with GPU-composed tiled ETOPO heightmaps and quadtree LOD
- Near-detail trees, mid-distance imposters, and far-field procedural canopy
- Cascaded FFT water with shoreline foam and terrain interaction
- Skybox, atmosphere, time-of-day lighting, debug drawing, and profiling tools
- Offline runtime-asset conversion for meshes, textures, fonts, and global elevation data
- Optional Steamworks lobbies, networking, and Steam Input support

## Requirements

- Windows 10 or 11
- Visual Studio 2022 or 2026 Community with the **Desktop development with C++** workload
- [CMake](https://cmake.org/) 3.25 or newer, installed at the standard `C:\Program Files\CMake` location
- [Vulkan SDK](https://vulkan.lunarg.com/) 1.4.341.1 installed at `C:\VulkanSDK\1.4.341.1` so `glslc` is available
- Internet access during the first configure so CMake can download declared dependencies
- Generated runtime assets under `assets/runtime` (see [Runtime assets](#runtime-assets))

Ninja is supplied by supported Visual Studio installations. The build scripts initialize the Visual Studio environment automatically and refresh stale CMake caches after an MSVC toolset upgrade.

## Build and run

From a PowerShell or Command Prompt opened at the repository root:

```powershell
tools\build.cmd Debug
.\build\Debug\app\terrain_sandbox.exe
```

`tools\build.cmd` configures the requested build directory on its first run, compiles shaders, builds the app, and stages runtime assets. Later builds are incremental.

Useful launch options:

```powershell
# Run locally without initializing Steam
.\build\Debug\app\terrain_sandbox.exe --disable-steam

# Exercise startup and exit after one rendered frame
.\build\Debug\app\terrain_sandbox.exe --verbose-startup --quit-after-first-frame

# Exercise asynchronous source uploads and composed-heightmap completion
.\build\Debug\app\terrain_sandbox.exe --disable-steam --quit-after-frames 120 --verify-heightmap-pipeline

# Stress bounded heightmap staging, CPU readbacks, and invalidation during traversal
.\build\Release\app\terrain_sandbox.exe --disable-steam --quit-after-frames 3600 --stress-heightmap-pipeline
```

For an optimized build:

```powershell
tools\build.cmd Release
.\build\Release\app\terrain_sandbox.exe
```

The canonical build directories are `build/Debug`, `build/Release`, and `build/Assets/<Config>`. Application runtime files are isolated under `build/<Config>/app`, tests under `build/<Config>/tests`, and the offline converter under `build/Assets/<Config>/converter`.
When `STEAMWORKS_APP_ID` is set, CMake generates `steam_appid.txt` directly beside the application executable in `build/<Config>/app`, including configurations where Steamworks support itself is disabled.

## Runtime assets

The application loads prebuilt runtime packs from `assets/runtime`; the build copies them into `build/<Config>/app/assets/runtime`. The `assets` tree is intentionally ignored by Git, so a clean clone needs the source/runtime asset set supplied separately before the complete scene can run.

Build the optimized Release converter, then explicitly generate whichever
standard packs are needed after their source files are present:

```powershell
tools\build.cmd Assets
.\build\Assets\Release\converter\converter.exe skybox
.\build\Assets\Release\converter\converter.exe pinetreepack
.\build\Assets\Release\converter\converter.exe pbr
.\build\Assets\Release\converter\converter.exe roboto
# Generate the global ETOPO base heightmap after installing its source data
.\build\Assets\Release\converter\converter.exe etopo2022
# Optionally generate the higher-resolution Japan DEM10 delta heightmaps
.\build\Assets\Release\converter\converter.exe japan-dem10
```

Runtime heightmap format v3 uses per-tile float scale/bias with 16-bit samples and an exact additive-zero code. Older heightmap packs must be regenerated: ETOPO first, then Japan DEM10. Source elevation stays floating point until each completed runtime tile is quantized.

The terrain requires the generated ETOPO pack as its global base heightmap. The optional Japan DEM10 conversion produces four additive, ETOPO-relative delta packs; the runtime streams them over Japan only at terrain pitches of 32 m or finer. Their coastline correction collars suppress positive coarse ETOPO terrain for 2 km beyond valid DEM10 coverage so land does not reappear immediately offshore.

The ETOPO, Japan DEM10, and pine source data are supplied separately and are not part of the repository. Details about their expected source directories and every converter mode are in the [asset converter guide](tools/converter/README.md). Generated heightmap packs remain under the ignored `assets/runtime` tree and are staged into the application by the next normal build.

## Optional Steamworks support

Steam support is enabled automatically when Steamworks SDK 1.64 is found at:

```text
../deps/steamworks_sdk_164/sdk
```

Without the SDK, the project configures with Steam support disabled. You can also provide another SDK location through `STEAMWORKS_SDK_DIR` during manual CMake configuration. At runtime, use `--disable-steam` when testing without the Steam client.

## Troubleshooting

- **`glslc was not found`**: verify Vulkan SDK 1.4.341.1 is installed at the path above.
- **Dependency download failed**: confirm GitHub access, then rerun the same build command.
- **Runtime asset load failed**: confirm the required `.assetbin`, `.meshbin`, and `.texbin` files exist under `assets/runtime`, then rebuild the app to stage them.
- **Visual Studio was upgraded**: rerun the normal build command; stale compiler paths are detected and the CMake cache is refreshed automatically.

## Documentation

- [Rendering architecture](docs/codex_rendering_architecture.md)
- [Asset converter and ETOPO workflow](tools/converter/README.md)
- [Agent working notes](AGENTS.md)
