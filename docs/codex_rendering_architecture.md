# Codex Rendering Architecture

*Existing frame pipeline, renderer pipelines, caches, and asset lifetimes*

> Scope: the current main branch of github.com/nicjohnson6790/codex, with emphasis on the SDL3 GPU rendering path and the world-residency systems that feed it.

| Document focus | What is covered |
| --- | --- |
| Frame orchestration | CPU update/emission, uploads, compute generation, readback, render passes, submission, fences. |
| Renderer pipelines | Terrain, water, three foliage paths, sky/atmosphere, world text, debug triangles/lines. |
| Caches | Ownership, capacity, residency policy, generation budget, invalidation, and stale-result protection. |
| Asset lifetimes | Application-lifetime GPU assets, viewport resources, resident cache contents, temporal state, and per-frame data. |
| Offline asset boundary | Converter-only source import, stable runtime packs, build staging, and the offline ETOPO base-heightmap foundation. |
| Multiplayer render boundary | Session/transport ownership, remote-entity presentation state, and emission through existing debug/text render queues. |

Prepared from static source inspection; this is an implementation document, not a proposed redesign.

## 1. Architecture at a glance

Codex uses a split architecture in which App orchestrates simulation, camera/UI state, world residency, and draw emission, while SDLRenderer owns GPU command recording and the fixed order of copy, compute, render, and presentation work. World managers sit between those layers: they decide which semantic world assets are resident and which slots may be reused; renderer classes own the actual GPU allocations and perform generation/drawing.

The frame is therefore not simply “build draw list, draw it.” Terrain and foliage are streamed and generated over multiple frames. The CPU first consumes completed asynchronous readbacks, ages and updates residency caches, emits only currently usable draws, and schedules missing resources. The GPU then uploads frame data, generates queued terrain/foliage content, updates water simulation, queues readbacks, renders the viewport, composites atmosphere, renders the editor UI, and finally submits a fence used to retire asynchronous work safely.

### Core ownership rule

> Managers own semantic identity and residency policy; renderers own storage and GPU execution. A cache slot is meaningful only while the manager still maps the same world key to that slot.

| Layer | Primary responsibility | Typical lifetime |
| --- | --- | --- |
| App | Lifecycle, input/simulation, camera/UI synchronization, scene/update ordering, renderer initialization/shutdown. | Application |
| World managers / quadtree | Visibility, LOD, cache residency, LRU/reuse policy, generation requests, fallback choice. | Application; resident entries vary |
| Renderer classes | Pipelines, buffers/textures, frame uploads, compute dispatches, indirect draws, readback staging. | Application; frame buffers reused |
| SDLRenderer | GPU device, viewport targets, pass ordering, command buffer submission, fence handoff, swapchain UI. | Application + viewport |

### Lifetime classes used in this document

- Application lifetime — created during renderer initialization and destroyed during renderer shutdown: pipelines, samplers, static meshes, material/font/cubemap textures, fixed-capacity pools.

- Viewport lifetime — viewport color/depth targets persist until a resize/recreation; recreation is deferred until after the current submission.

- Resident-cache lifetime — semantic content in a fixed GPU slot remains valid until LRU eviction, explicit cache clear/invalidation, or shutdown.

- Temporal lifetime — simulation history intentionally crosses frames, e.g. water FFT working state and foam history.

- Frame lifetime — CPU draw arrays and dynamic uploads are cleared/rebuilt each frame; backing GPU allocations are commonly reused or grown to a high-water mark.

- Asynchronous-work lifetime — transfer/readback objects remain alive until the fence for the command buffer that used them has completed.

## 2. Existing frame pipeline

The current App loop performs platform/input/simulation and ImGui setup, synchronizes renderer state, updates the world scene, emits multiplayer/debug/text primitives, records the GPU frame through SDLRenderer::renderFrame, and finishes the frame. The following sequence captures the rendering-relevant path.

> **Code:** App.cpp — main loop; updateSceneForFrame(); emitWorldDraws(); scheduleGenerationWork(); renderCurrentFrame()

### 2.1 CPU scene/update phase

1. Collect completed asynchronous results before changing residency. The quadtree/terrain path consumes completed height extents and optional CPU heightmap slice readbacks; nearby foliage consumes completed decoded-page readbacks.

1. Synchronize frame-global state: active camera, viewport dimensions, terrain settings, water settings, foliage terrain/water inputs, and world-text camera basis.

1. Age resident caches. Entries used during the current frame are reset to age 0; older entries become candidates for reuse. Cache-specific locks/pending flags prevent unsafe eviction.

1. Update the world quadtree around the camera, determine visible/nearby nodes, and maintain topology/LOD state.

1. Emit scene draws while resolving residency. Visible terrain requires a resident heightmap slice. Foliage selects canopy, imposter, and nearby representations according to distance/readiness; water requests are accumulated for the current frame.

1. Schedule queued generation with fixed per-frame budgets. Missing heightmaps, foliage pages, canopy cells, and nearby decode work converge over later frames rather than blocking the current frame.

1. Build transient renderer queues: debug triangles/lines, multiplayer primitives, world-space MSDF text, terrain/bridge instances, water instances, canopy/imposter/nearby draws.

A notable consequence is that draw emission and generation scheduling are coupled to cache state. The visible frame may deliberately use a lower-detail/fallback representation while a higher-detail asset is pending.

### 2.2 GPU command-recording phase

| Order | Stage | Work recorded |
| --- | --- | --- |
| 1 | Acquire command buffer | SDLRenderer begins one command buffer for the frame. |
| 2 | Copy / upload | Upload triangle, terrain, canopy, imposter, nearby foliage, water, line, and world-text frame data. |
| 3 | Terrain + foliage compute | Generate queued heightmap slices; generate canopy cells; generate canonical foliage page instances; expand/decode nearby pages. |
| 4 | Water compute | Update spectrum/FFT/map generation for the active water cascades. |
| 5 | Readback copy | Queue height extents, optional heightmap slices, foliage live counts, and decoded nearby-page copies. |
| 6 | Main viewport pass | Render terrain → water → nearby foliage → imposter foliage → canopy → debug triangles/lines → world text into viewport color/depth. |
| 7 | Sky/atmosphere composite | Load the existing viewport color and composite sky/haze using the depth texture and inverse view-projection. |
| 8 | Editor UI pass | Acquire swapchain texture and render ImGui, including the viewport texture. |
| 9 | Submit + fence | Submit command buffer, acquire a fence, and attach a shared fence object to readback-producing renderers. |
| 10 | Deferred viewport recreation | If viewport size changed, recreate viewport targets only after submission because ImGui may still reference the old texture this frame. |

> **Code:** SDLRenderer.cpp — renderFrame()

### 2.3 Frame synchronization model

Generation writes, readback copies, render passes, and UI are recorded in a deterministic command-buffer order. CPU-visible completion is fence-driven rather than assumed at submit time. Readback records retain both their transfer resource and semantic identity until the submitted fence completes. On collection, the manager/renderer re-validates that identity before accepting data, which prevents an old GPU result from being applied to a cache slot that has since been reused.

## 3. Cache and asset lifetime overview

| System | Owner / storage | Capacity / budget | Fill & use | Eviction / invalidation |
| --- | --- | --- | --- | --- |
| Terrain heightmaps | Heightmap manager / QuadtreeMeshRenderer GPU pool | 512 final slices; 64 source-tile slices; generation budget 4/frame | Leaf → composed final slice residency. Tiled datasets are affinely placed as source heightmaps; contributions are GPU-composed and extents read back. | Final and source LRU caches are separate. Current jobs pin source dependencies; revision changes discard stale work before replacing source GPU data. |
| CPU heightmap mirrors | Heightmap manager CPU mirrors / readback slots | One mirror per slice when requested; 8 readback slots | On-demand only, for CPU consumers. | Invalid on slice reassignment; stale readback ignored if leaf/slice association changed. |
| Canonical foliage pages | Foliage manager / FoliageImposterRenderer page pool | 1024 pages; generation budget 4/frame | Compute writes candidate/live instances into shared persistent pool. Imposter draws directly from it. | LRU; cannot evict pending/current-frame entries. Terrain/water-setting changes clear cache. |
| Canopy cells | Canopy manager / FoliageCanopyRenderer bitset pool | 4096 cells; generation budget 256/frame | Queued per cell; generated into fixed bitset slots; fade age controls visual ramp. | Oldest unlocked resident slot reused. Generation lock protects in-flight slot. Cache invalidates on relevant terrain/water changes. |
| Nearby decoded pages | WorldGridNearbyFoliageManager / NearbyFoliageRenderer | 16-page CPU cache; decode budget 16/frame; 16 readback slots | The manager owns residency/jobs; the renderer decodes the canonical page pool and stores CPU-visible results for draw/gameplay use. | Generic cache aging. Source page contentVersion/layoutVersion mismatch makes entry stale. Fence + job/key/version checks reject obsolete readbacks. |
| Water simulation | QuadtreeWaterMeshRenderer | 512² maps; 4 cascades; update modulo typically 1/1/2/4 | Persistent FFT working buffers and displacement/slope maps updated over time. | Not LRU. Rebuilt/reset when simulation settings/resources require it; foam history is temporal double-buffered state. |
| Sky / atmosphere | SkyboxRenderer | 1 cubemap + 32³ atmosphere LUT | Persistent runtime texture assets; sky composite every frame. | Cubemap lives with renderer. LUT persists until explicit regeneration or renderer shutdown. |
| World text | WorldTextRenderer | Dynamic high-water buffers | CPU text queues rebuilt each frame; MSDF font atlas/metrics persistent; GPU buffer pairs grow as needed. | Frame content clears every frame; backing allocation remains until growth/shutdown. |
| Debug geometry | TriangleRenderer / LineRenderer | Dynamic/high-water frame buffers | Immediate/debug primitives rebuilt and uploaded each frame. | CPU queues clear every frame; pipelines and allocated buffers persist until shutdown. |
| Viewport color/depth | SDLRenderer | One active viewport target set | Main render + sky composite; color shown by ImGui. | Recreated on viewport resize after current frame submission. |

## 4. Terrain / quadtree renderer pipeline

### 4.1 Responsibilities and data flow

QuadtreeMeshRenderer is both the terrain draw renderer and an important GPU-generation hub. It owns the persistent terrain heightmap pool, terrain graphics pipelines, generation compute pipelines, static terrain/bridge meshes, PBR material arrays, and the per-frame instance/indirect data used to draw the current LOD.

- CPU manager resolves each visible leaf to a final heightmap slice. First allocation computes and retains a block of source-heightmap tile contributions. Requests drive shared source-tile residency and queue final composition only after every current source revision is ready.

- Generation scheduling reuses a free slice or the oldest evictable resident slice, then queues a GPU generation descriptor. Slice reuse invalidates cached extents and optional CPU height data immediately.

- During the GPU compute stage, batches of up to 16 final heightmaps are composed into the persistent slice pool. Each Z dispatch layer follows one final descriptor into a permanent contribution-descriptor buffer and samples renderer-owned 256x256 source-tile slices.

- A later copy stage queues extents and requested heightmap-slice readbacks. Those are consumed only after the submission fence completes.

- Terrain draws reference resident slice indices and render main patches plus bridge/coarse-bridge geometry for LOD seams.

### 4.2 Persistent assets

- Terrain and bridge graphics pipelines; heightmap-generation compute pipeline; foliage-page-generation compute pipeline.

- Static terrain and bridge meshes; one-time transfer resources used during initialization.

- Persistent heightmap slice buffer and height extents buffer.

- PBR albedo/normal/roughness/AO texture arrays and sampler; caustics texture/sampler used by terrain shading.

- Persistent foliage-generation/live-count buffers used when this renderer generates the canonical foliage pool owned by the foliage pipeline.

### 4.3 Lifetime and cache invariants

The GPU final-heightmap buffer is application-lifetime storage, but a slice's semantic leaf identity is only resident until the manager reassigns it. Source tile identity excludes placement, so repeated affine placements share one source slice while retaining independent additive contribution descriptors. The manager pins source slices used by queued or submitted final work and does not replace an old revision until its fence is safe. Readbacks carry leaf/slice metadata and are discarded if that identity no longer matches when the fence completes.

> **Code:** QuadtreeMeshRenderer.hpp; WorldGridQuadtreeHeightmapManager.hpp/.cpp; WorldGridQuadtree.cpp

## 5. Foliage pipelines

Foliage is intentionally split into three render paths that share a canonical page representation: imposters for broad coverage, canopy cells for a dense aggregate view, and decoded nearby foliage for detailed tree meshes. Readiness determines fallback; higher-detail representations are layered/selected only when their dependent cache entries are valid.

### 5.1 Canonical page cache and imposter renderer

WorldGridFoliageManager owns the page-key → page-slot residency map, while FoliageImposterRenderer owns the persistent GPU page-pool buffer. QuadtreeMeshRenderer's compute pipeline fills that shared pool.

- Capacity is 1024 pages; generation is throttled to 4 new pages per frame.

- A page can be Ready, UploadPending, MaskValid, and/or MaskPending. Pending or current-frame entries are not safe eviction candidates.

- On a cache hit, age resets and the page can draw only when content/mask state is ready. On a miss, generation is queued and a fallback may be used.

- Generated live counts are applied asynchronously; successful completion increments contentVersion. That version is consumed by the nearby decoded cache to detect source mutation.

- Terrain settings or water level changes clear the canonical page cache because placement/masking semantics change.

FoliageImposterRenderer keeps the page pool, imposter color/normal texture arrays, tree-class metadata, material sampler, quad geometry, graphics/depth pipelines, and dynamic indirect/draw metadata. The pool allocation lives for the renderer lifetime; individual page contents live only for their residency interval.

> **Code:** WorldGridFoliageManager.hpp/.cpp; FoliageImposterRenderer.hpp; FoliageTypes.hpp

### 5.2 Canopy renderer and canopy-cell cache

The canopy path is another fixed-slot resident cache. WorldGridFoliageCanopyManager owns semantic cells and eviction; FoliageCanopyRenderer owns the GPU bitset pool and compute/draw machinery.

- Capacity is 4096 canopy cells, with a much larger generation budget (256 cells/frame) than canonical foliage pages.

- Resident cells track eviction age plus resident-frame age used for fade-in; generation-locked slots are protected from reuse while work is queued/in flight.

- Queued requests carry frame/request context so stale work can be skipped rather than consuming capacity after the requesting scene has changed.

- The renderer's compute stage writes generated canopy-cell bitsets using the terrain heightmap as input; its draw stage consumes the ready slot set.

- The quadtree uses a readiness threshold before relying on canopy (minimum ready-cell count is 48). When canopy is not sufficiently ready, imposter foliage provides coverage.

Static canopy geometry, pipelines, bitset pool, and generation buffers are application-lifetime. Per-frame cell draw metadata/indirect data are transient; semantic cell contents persist until manager eviction.

> **Code:** WorldGridFoliageCanopyManager.hpp/.cpp; FoliageCanopyRenderer.hpp; FoliageTypes.hpp

### 5.3 Nearby detailed foliage and decoded-page cache

WorldGridNearbyFoliageManager owns the semantic decoded-page cache and decode-job queue. NearbyFoliageRenderer converts canonical foliage pages into detailed per-tree instances, owns GPU transfer/decode resources, and stores the accepted CPU payload for rendering and gameplay. Unlike the broad page pool, this dependent cache is deliberately small and CPU-visible.

- The decoded page LRU holds 16 pages; up to 16 decode dispatches can be scheduled per frame, with 16 readback slots.

- Each decoded entry records source page index, live count, contentVersion, layoutVersion, cache key, CPU instances, validity, and pending-readback state. Generic cache age and active-job ownership remain manager-side.

- The source page pool is read by the decode compute stage. Results are copied back so CPU systems can query nearby resident pages as well as render them.

- A decoded entry is valid only while the canonical source page still has the same semantic key/version/layout. Canonical page mutation therefore invalidates dependent decoded data without requiring a global flush.

- Readback records retain transfer buffer + submitted fence + key/version metadata; completed data is accepted only if the target entry still matches.

Tree mesh/material assets and texture arrays are renderer-lifetime assets. The decoded-page semantic data is resident-cache lifetime; current-frame detailed draw/indirect buffers are transient/reused.

> **Code:** NearbyFoliageRenderer.hpp; FoliageTypes.hpp

## 6. Water renderer pipeline

WorldGridQuadtreeWaterManager is primarily a per-frame visibility/emission manager rather than a long-lived water-asset cache. It clears its request set each frame, accumulates water patches while the quadtree emits the scene, and flushes the current requests to QuadtreeWaterMeshRenderer.

### 6.1 Simulation

- Persistent initial-spectrum data seeds the wave system. A dirty flag causes the initial spectrum to be regenerated when required.

- Each active cascade updates spectrum state, performs FFT stages through ping/pong buffers, then builds displacement/slope maps.

- Default configuration supports four 512×512 cascades; cascades may update at different frame modulo rates (typically 1, 1, 2, and 4) to reduce compute cost.

- Foam uses history read/write textures and a validity flag, making it explicitly temporal state rather than a frame-local result.

### 6.2 Draw and dependencies

- The water graphics pass consumes current terrain heightmaps for terrain-relative placement/intersection, the simulated displacement/slope products, and environment/sky information.

- Static water/bridge meshes and graphics/compute pipelines live for the renderer lifetime. Current water instances and indirect data are rebuilt/uploaded per frame.

- Simulation working buffers and output textures persist across frames; they are not managed by an LRU. Settings changes or resource recreation may reset/rebuild state.

> **Code:** WorldGridQuadtreeWaterManager.hpp; QuadtreeWaterMeshRenderer.hpp; AppConfig water settings

## 7. Skybox and atmosphere pipeline

SkyboxRenderer is intentionally outside the main depth-writing viewport pass. After all geometry/text is rendered, SDLRenderer begins a second pass that loads the existing viewport color target and invokes the sky renderer with the depth texture and inverse view-projection. This allows sky, distance haze, and atmospheric contribution to be composited around/behind existing geometry without replacing the main depth buffer.

- Persistent renderer assets: sky graphics pipeline, fullscreen/static vertex buffers, cubemap texture, atmosphere LUT (32³), and cubemap/atmosphere/depth samplers.

- The cubemap is a runtime asset loaded at initialization and lives until shutdown.

- The atmosphere LUT is a persistent derived asset. It remains valid across frames and is regenerated explicitly when atmosphere parameters require it.

- There is no LRU or per-world residency for sky resources; the only per-frame inputs are camera/view data, depth, and current atmospheric settings.

> **Code:** SkyboxRenderer.hpp; SDLRenderer.cpp sky composite pass

## 8. World-space text pipeline

WorldTextRenderer is a first-class depth-tested renderer in the main viewport pass. It uses an MSDF font atlas and glyph metrics to expand queued world-space text into grouped indirect draws.

- Persistent assets: MSDF font texture/sampler, ASCII glyph metrics loaded from the runtime Roboto font pack, static quad geometry, graphics pipeline.

- Frame data: text groups, glyph instances, styles, and indirect commands are cleared/rebuilt from App/world emitters each frame.

- GPU group/glyph/metric/style/indirect BufferPair objects grow on demand. Clearing frame content does not shrink them; backing allocations therefore persist at the renderer's high-water mark until shutdown.

- Camera basis is synchronized before scene emission so text billboarding/orientation can be produced consistently with the active camera.

> **Code:** WorldTextRenderer.hpp; App.cpp buildPrimitiveDraws()/camera synchronization

## 9. Debug triangle and line pipelines

TriangleRenderer and LineRenderer are immediate/debug-oriented paths. App rebuilds their CPU primitive queues every frame (including axes, editor/debug markers, and multiplayer visualization), then SDLRenderer uploads them before the viewport pass.

- Graphics pipeline/static or dynamic geometry resources are renderer-lifetime.

- CPU primitive arrays are frame-lifetime and cleared before emission.

- Dynamic GPU/transfer buffers are reused and grow as necessary rather than being allocated per primitive.

- They render late in the main viewport pass, after world geometry/foliage and before world-space text, so they behave as visualization overlays while still participating in depth behavior configured by their pipelines.

> **Code:** TriangleRenderer.hpp; LineRenderer.hpp; App.cpp buildPrimitiveDraws()/buildDebugAxes()

## 10. Cross-pipeline dependencies

| Producer / state | Consumers | Architectural implication |
| --- | --- | --- |
| Terrain heightmap pool | Terrain draw, canopy generation, canonical foliage generation, nearby placement/decode context, water draw/simulation interactions | Heightmap residency is foundational. A missing/reused slice can gate several downstream representations. |
| Canonical foliage page pool | Imposter rendering; NearbyFoliageRenderer decode | Nearby detailed foliage is a dependent cache, not an independent source of truth. |
| Canonical foliage contentVersion/layoutVersion | Nearby decoded-page validation | Dependent cache coherence is versioned; source mutation invalidates only affected decoded entries. |
| Water displacement/slope/foam state | Water draw; terrain shading/caustic-related inputs | Water is temporal GPU state, so update order before the viewport pass matters. |
| Viewport depth | Sky/atmosphere composite | Sky is a post-geometry composite; it relies on completed geometry depth rather than sharing the geometry pass. |
| SubmittedGpuFence | Heightmap/foliage/nearby readback retirement | CPU-visible results are associated with actual submission completion, preventing premature transfer reuse. |

## 11. Cache invalidation and stale-work protection

Terrain heightmaps, canonical foliage pages, canopy cells, and nearby decoded pages now share the `FixedAssetCache` and `GenerationQueue` protocol. Each owner composes independently configured cache and queue instances while retaining only content-specific metadata. The shared protocol keeps fixed-capacity GPU storage reusable without confusing storage identity with world identity.

`FixedAssetCache` owns bit-packed open/ready state, saturating `uint8_t` eviction age, configurable hash lookup with a full-cache fallback, and the active generation-job handle for every slot. Requests return a usable slot or unavailable; queued assets remain unavailable, and queue admission occurs before any cache reassignment. A supplied slot hint is validated against the semantic asset ID before use.

`GenerationQueue` is a hard-capacity front/count ring. Entries independently track submitted, discarded, and completed state plus a shared submission fence. Completion can be processed out of fence order, but physical storage is reclaimed only by advancing the front across completed entries. Discard immediately severs cache ownership; submitted discarded work retains its fence until signaling and never runs cache completion logic.

| Mechanism | Used for | Effect |
| --- | --- | --- |
| LRU age / age=0 pin | Heightmaps, foliage pages, decoded pages, canopy cells | Prefer old resources for reuse; prevent resources touched by the current frame from being evicted mid-emission. |
| Pending / generation lock | Foliage pages, canopy cells, readbacks | Prevent a slot or transfer object from being recycled while GPU work still depends on it. |
| Leaf/key association check | Heightmap CPU/extents readback; decoded foliage | Reject completed results if the slot now belongs to a different semantic world asset. |
| contentVersion / layoutVersion | Canonical → nearby foliage dependency | Invalidate detailed decoded data when source contents or interpretation change. |
| Settings-driven clear | Foliage, terrain-derived placement | Invalidate caches wholesale when the placement/masking function changes materially. |
| Dirty / valid flags | Water initial spectrum and foam history; atmosphere LUT | Retain expensive temporal/derived assets until an explicit dependency change requires regeneration. |

## 12. Practical maintenance invariants

- Do not treat a GPU pool index as a durable world identifier. The manager mapping/version is the authoritative identity.

- Generation budgets are part of visible behavior. Increasing LOD/detail demand does not imply all assets become available in the same frame; fallback paths must remain valid.

- Any new asynchronous readback should carry enough semantic metadata to prove that its destination is still the same object when the fence completes.

- If a setting changes the deterministic content of terrain/foliage placement, invalidate the corresponding semantic cache, not merely its current draw list.

- If a new renderer samples a generated pool, place its compute/read dependency in SDLRenderer's command order explicitly; do not rely on renderer call-site coincidence.

- Keep viewport target recreation after submission when the just-built ImGui draw data can reference the old viewport texture.

- For dependent caches, prefer narrow version-based invalidation over global flushing when a stable source version can be propagated.

- Renderer shutdown should occur only after GPU idle/fence completion, because renderer-lifetime textures/buffers are shared across multiple passes and generation systems.

## 13. Source map for future changes

| Area | Primary implementation files |
| --- | --- |
| Application/frame orchestration | App.cpp / App.hpp |
| GPU frame/pass orchestration | SDLRenderer.cpp / SDLRenderer.hpp |
| Terrain + GPU generation | QuadtreeMeshRenderer.hpp/.cpp |
| Terrain residency/cache | WorldGridQuadtreeHeightmapManager.hpp/.cpp |
| World visibility/LOD/emission | WorldGridQuadtree.hpp/.cpp |
| Canonical foliage residency | WorldGridFoliageManager.hpp/.cpp; FoliageTypes.hpp |
| Foliage imposters/page-pool storage | FoliageImposterRenderer.hpp/.cpp |
| Canopy residency + draw/generation | WorldGridFoliageCanopyManager.hpp/.cpp; FoliageCanopyRenderer.hpp/.cpp |
| Nearby detailed foliage | NearbyFoliageRenderer.hpp/.cpp |
| Water visibility/emission | WorldGridQuadtreeWaterManager.hpp/.cpp |
| Water simulation/rendering | QuadtreeWaterMeshRenderer.hpp/.cpp |
| Sky/atmosphere | SkyboxRenderer.hpp/.cpp |
| World text | WorldTextRenderer.hpp/.cpp |
| Debug primitives | TriangleRenderer.hpp/.cpp; LineRenderer.hpp/.cpp |
| Capacities/configuration | AppConfig.hpp; FoliageTypes.hpp |
| Multiplayer session and transport | MultiplayerManager.hpp/.cpp; IMultiplayerTransport.hpp; SteamSocketsTransport.hpp/.cpp; MultiplayerProtocol.hpp |
| Multiplayer render emission | MultiplayerRenderManager.hpp/.cpp; App.cpp |
| Offline asset conversion | tools/converter/*; src/assets/RuntimeAssetFormat.hpp; src/assets/RuntimeHeightmapFormat.hpp |

## 14. Condensed pipeline reference

> CPU: collect prior readbacks -> sync camera/settings -> update multiplayer snapshot/session -> age/update caches -> update quadtree -> resolve residency + emit world/multiplayer draws -> schedule generation -> build debug/text
> GPU: upload -> terrain/foliage compute -> water compute -> queue readbacks -> terrain -> water -> nearby foliage -> imposter -> canopy -> debug -> world text -> sky/atmosphere composite -> ImGui/swapchain -> submit fence -> deferred viewport resize

This sequence is the most useful single mental model for the current renderer: the first half produces and validates resident world data, while the second half consumes that data in a fixed GPU ordering. Most renderer assets are application-lifetime allocations; what changes frame-to-frame is the semantic mapping of world content into those allocations.

## 15. Offline asset pipeline and global heightmap foundation

Runtime rendering is intentionally separated from authored-source import. The standalone converter owns expensive or format-specific processing and emits compact binary packs that the application can validate and upload without linking source-format libraries into the runtime. Generated packs live under assets/runtime and the main CMake build stages them into build/&lt;Config&gt;/assets/runtime.

- The converter builds in its own build/Assets tree. Assimp, SDL_image, FreeType, DirectXTex, libtiff, and converter-side image/shader work remain outside the main executable.

- Small assetbin manifests describe independently compressed payloads held in meshbin, texbin, or heightbin files. Runtime readers validate headers, offsets, counts, formats, and compression metadata before consumers create GPU resources.

- Mesh, material, texture, font, skybox, and foliage-imposter artifacts are application-lifetime inputs after upload. Their authored files are not consulted during rendering.

- The ETOPO 2022 path generates a versioned Airocean one-island atlas as independently filtered and LZ4-compressed 256x256 tiles. It is currently an offline foundation only: no runtime base-height sampler, terrain composition layer, or atlas placement transform consumes it yet.

> **Code:** tools/converter; RuntimeAssetReader.cpp; RuntimeAssetFormat.hpp; RuntimeHeightmapFormat.hpp; root CMakeLists.txt asset staging

## 16. Multiplayer-to-render boundary

Multiplayer is integrated above the renderer rather than as a new GPU pipeline. SteamService owns platform initialization, lobby discovery, and callbacks; IMultiplayerTransport defines the networking boundary; SteamSocketsTransport implements that boundary with Steam Networking Sockets; and MultiplayerManager owns session state, protocol handling, remote snapshots, and presentation-ready remote entities.

- App builds the local PlayerPawn snapshot and updates MultiplayerManager before scene emission. This keeps network polling, snapshot publication, interpolation, and rendering on a deterministic frame boundary.

- Transport messages carry semantic player state, not renderer handles or GPU slot indices. Multiplayer state therefore remains independent of terrain and foliage residency storage.

- MultiplayerRenderManager reads presentation positions from remote entities and emits player markers and labels through the existing TriangleRenderer and WorldTextRenderer queues.

- Because multiplayer presentation reuses established immediate render paths, SDLRenderer's GPU pass order and resource-lifetime rules remain unchanged. Disconnecting or losing a remote entity removes future emissions without introducing a separate renderer cache.

> **Code:** App.cpp updateMultiplayerForFrame()/buildPrimitiveDraws(); MultiplayerManager.*; IMultiplayerTransport.hpp; SteamSocketsTransport.*; MultiplayerRenderManager.*
