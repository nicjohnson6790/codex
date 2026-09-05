# Agent Working Notes

This file is the durable handoff point for coding agents working in this repository. Read it at the start of a session and update it when work changes an enduring project fact, workflow, constraint, or unfinished thread.

Keep notes concise and current. Do not append command transcripts or routine progress logs. Remove or rewrite obsolete notes when their underlying work is completed.

## Project orientation

- Codex is a Windows C++20 terrain sandbox using SDL3 GPU, Dear ImGui, CMake, Ninja, and GLSL compiled to SPIR-V.
- The root [README.md](README.md) is intentionally an onboarding document. Detailed rendering design belongs in [docs/codex_rendering_architecture.md](docs/codex_rendering_architecture.md).
- Canonical builds are `tools\build.cmd Debug`, `tools\build.cmd Release`, and `tools\build.cmd Assets [Debug|Release]`; the Assets configuration defaults to Release.
- `tools\build.cmd Assets [Debug|Release]` builds the converter only. Every asset-pack conversion is an explicit `build\Assets\<Config>\converter\converter.exe <pack>` operation.
- Canonical build directories are `build/Debug`, `build/Release`, `build/Assets/Debug`, and `build/Assets/Release`. Avoid creating alternate build-directory names for routine validation.
- Keep target runtime outputs isolated: the app and its staged resources belong in `build/<Config>/app`, tests in `build/<Config>/tests`, and the asset converter in `build/Assets/<Config>/converter`. Do not stage target outputs directly into a canonical build root.
- Generate local runtime metadata such as `steam_appid.txt` directly in `build/<Config>/app`; do not create an intermediate copy in the canonical build root.
- The build/configure scripts detect CMake caches that reference a removed MSVC compiler and automatically reconfigure with `cmake --fresh`.
- Windows `.cmd`/`.bat` scripts must retain CRLF working-tree line endings (enforced by `.gitattributes`); LF-only scripts can fail with missing batch subroutine labels before compilation.

## Durable constraints

- The entire `assets` directory is ignored by Git except for shared format/reader source code under `src/assets`.
- Runtime asset packs are generated into `assets/runtime` and staged into each app build. Do not commit generated packs or externally licensed source assets.
- Steamworks support is optional. The default SDK location is `../deps/steamworks_sdk_164/sdk`; local runs can use `--disable-steam`.
- The ETOPO converter is an explicit offline operation, not part of the normal asset build. Its format is version 3 and its fixed Airocean projection is version 3.
- ETOPO is the initial runtime terrain source. `WorldGridQuadtreeHeightmapManager` owns tiled source residency, affine source placements, retained final-to-source references, and composed-final scheduling; `QuadtreeMeshRenderer` retains all GPU resource ownership. User-created heightmap layers remain future work.
- Terrain heightmaps, canonical foliage, canopy, and nearby decoded foliage use the shared `FixedAssetCache` / `GenerationQueue` residency protocol in `src/AssetResidency.hpp`; preserve transactional queue-before-cache admission, ready-only request results, manager-owned shared fences, and handle-based stale-result retirement. Nearby residency is owned by `WorldGridNearbyFoliageManager`, not its renderer.
- `FixedAssetCache` lookup storage is `bucketCount * entriesPerBucket`, with a cache-capacity overflow list consulted only for buckets marked overflowed. Do not replace this with bounded linear probing plus unconditional full-cache fallback scans.
- Node-associated final-heightmap, canonical foliage-page, canopy-cell, and nearby-foliage cache hints live in `WorldGridQuadtree::m_residencyHints`, a fixed-capacity array parallel to `m_nodes`. Hints survive frames until node-slot reuse; use this storage instead of temporary hint-preservation bookkeeping. Multi-asset entries follow the semantic caches' ordering, and nearby topology shares canonical foliage entry zero.
- Hints are disposable lookup acceleration, never semantic identity. Requests directly replace stored hints with their return values, including the common `kUnavailableCacheIndex` from `AssetResidency.hpp`; caches validate hinted slots against semantic IDs. Cache clears need no tree-wide hint reset. Parents and children own independent hints and access each other's state through tree links; prospective unallocated nodes still use ordinary requests. Source-heightmap references/hints remain a separate manager-owned mechanism.
- Read-only residency lookups use `hint = manager.isResident(id, hint)`: ready slot or unavailable, without admission, generation, or age touching. Slot-based `build...` data accessors validate semantic identity and payload readiness without hash lookup. Quadtree queries, including neighbors/debug/water, reuse node hints; collision tiles retain their own upstream hints.
- `CollisionManager` uses a 16-slot `FixedAssetCache` for tile identity, ground readiness, and eviction; tree readiness remains independent payload state. It uses `uint64_t` elapsed-frame ages to preserve timestamp eviction ordering beyond 255 frames. Other caches keep the default saturating `uint8_t` age. Collision payload and upstream hints reset on slot reassignment; copied CPU data does not depend on continued upstream residency.
- Preserve unrelated working-tree changes and use `apply_patch` for source/text edits.

## Session handoff notes

- The offline ETOPO heightmap converter and stale-MSVC-cache recovery are committed on local `main`.
- The rendering architecture document now also covers the offline asset boundary and multiplayer render-emission boundary.
- Shared asset-cache/generation-queue infrastructure and migrations are implemented locally; focused coverage is in `tests/AssetResidencyTests.cpp` and built by default as `asset_residency_tests`.
- Runtime heightmaps now compose cached 256x256 ETOPO source tiles through contribution descriptors; repeated placements share source residency and remain additive.
- Terrain materials, water FFT sampling, foam detail, and terrain caustics use CPU-computed periodic phases from `Position` via `src/PeriodicWorldPhase.hpp`; keep repeating shader coordinates render-origin-relative rather than reconstructing absolute X/Z floats.
- The offline `japan-dem10` converter path creates four fixed rotated 10 m ETOPO-relative delta mosaics from Source Cooperative's manifest-backed EPSG:6668 DEM10 files. The runtime registers available mosaics as additive sources only for final-heightmap pitches at or below 32 m; coarser terrain remains ETOPO-only.
- Terrain bridge instances carry independently resolved inner, outer, and two corner final-heightmap slices. `WorldGridQuadtree` owns neighbor, half-edge, and corner resolution; preserve the existing bridge mesh topology and renderer-side GPU ownership.
- Japan DEM10 conversion retains decoded ETOPO tiles and uses a bounded eight-raster DEM10 working set; preserve these caches to avoid severe boundary-thrashing regressions.
- Japan DEM10 bilinear sampling resolves each lattice point through the raster catalog so footprints crossing Source Cooperative GeoTIFF boundaries do not become zero-delta seams. Preserve the cross-raster self-test in `JapanDem10Converter.cpp`.
- Runtime heightmap format v3 stores per-tile float scale/bias and signed 16-bit codes; INT16_MIN is invalid and INT16_MIN+1 is exact additive zero. ETOPO stays float until final tile quantization; regenerate ETOPO before Japan DEM10. Runtime uploads remain float meters. Shared converter quantization and round-trip tests live in `tools/converter/HeightmapQuantization.hpp` and run in `heightmap_dataset_tests` and both converter self-tests.
- Japan DEM10 generation includes a 2 km fading ocean collar around source coverage that cancels positive ETOPO elevation without replacing existing bathymetry.
- Add concrete unfinished work here only when it must survive into another session; include the relevant file or subsystem and the next useful action.
