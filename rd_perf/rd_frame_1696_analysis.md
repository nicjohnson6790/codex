# GPU breakdown: frame 1696

Sources: `rd_frame_1696_202610141400_eventlist.txt` and `rd_frame_1696_202610141400_counters.csv`, mapped against the current render submission code. The CSV contains 417 distinct action EIDs. Times below are exported microseconds converted to milliseconds. Aggregate rows sum only leaf actions, not parent event-list groups.

The capture confirms the complete-mesh paths: one terrain draw with 95 instances and one water draw with 45 instances, with no bridge draws. It shows substantial geometry processing and full-screen atmospheric/cloud work. The exported timing attribution has anomalies, so these numbers do not establish live frame time, a reliable percentage budget, or a before/after improvement.

| EIDs | Work | Exported duration, ms | Interpretation |
|---|---|---:|---|
| 61–115 | 19 upload copies | 0.057 | Per-frame draw/state transfers; individual resource ownership cannot be identified from resource IDs alone. |
| 121 | Sky illumination refresh | 2.071 | Dispatch (16,4,1): four tiles, 64 ground probes, 16,384 hemisphere directions. |
| 129 | Water spectrum update | 0.930 | Four 512×512 cascades; 1,048,576 compute invocations. |
| 137,144,151,158 | Water FFT | 4.991 | Horizontal displacement/slope then vertical displacement/slope transforms. Each dispatch covers four cascades. |
| 168,181,194,207 | Water maps | 1.272 | One dispatch per cascade; displacement/slope/foam outputs and history advance. |
| 214,217 | Readback copies | 0.014 | Two small copies after generation/simulation. Exact payload requires resource inspection. |
| 233 | Terrain | 15.408 | 95 complete 256×256-interval grids. |
| 239 | Water | 21.692 | 45 complete 128×128-interval grids; expensive vertex surface evaluation and water fragment optics. |
| 245–256 | Nearby foliage depth/coverage | 72.034* | Twelve mesh subcommands. Timing attribution is especially suspect. |
| 261–272 | Nearby foliage color | 5.339* | Same twelve meshes, shaded at matching prepass depth. |
| 279–450 | Imposter depth/coverage | 2.660* | 172 subcommands, 54,492 quad instances; no reported fragment work. |
| 455–626 | Imposter color | 0.866* | Same instances; no reported fragment work. |
| 633 | Canopy | 1.181 | Twelve instances, 18,432 triangles, 197,701 fragment invocations. |
| 648 | Sky background | 1.208 | Full-screen invocation; writes background pixels only. |
| 651 | Medium transmission | 1.645 | Multiplies the existing scene by RGB transmission. |
| 654 | Medium scattering | 3.426 | Adds atmospheric/medium radiance. |
| 657 | Clouds | 13.953 | Full-screen, depth-clipped volumetric march. |
| 663,668,674 | Exposure clear/histogram/reduce | 0.082 | 256 + 30,464 + 1 compute invocations. |
| 687 | Display transformation | 0.139 | HDR exposure/shoulder/sRGB conversion. |
| 702,706,712 | Triangle/line/text overlays | 0.021 | No reported fragments in this capture. |
| 730,732,735,738 | ImGui/window composition | 0.621 | Four UI draws, including the viewport image. |

`*` These are arithmetic sums of exported subcommand values, not validated batched-pass measurements. Other per-event timings also need interpretation as replay timestamp intervals rather than isolated shader costs.

## Timing limits

The leaf durations sum to **149.610 ms**, but that is not a dependable GPU frame duration. Water EID 239 reports 21.69184 ms; the next nearby-foliage depth subcommands 245, 246 and 247 report 21.69144, 21.68900 and 21.68296 ms, despite only 1,332/335/781 triangles and 3,670/124/163 fragment invocations. EID 248 then reports 6.35004 ms. This pattern suggests timing contamination, dependency attribution or replay behavior; these text exports alone cannot establish the precise cause or a corrected total.

RenderDoc's [Vulkan counter implementation](https://github.com/baldurk/renderdoc/blob/v1.x/renderdoc/driver/vulkan/vk_counters.cpp) describes GPU duration as a timestamp delta and places timestamps at top/bottom of pipe. Such intervals are not direct measurements of individual shader-stage execution. Do not interpret the 72.034 ms nearby depth sum as proof that tiny nearby meshes dominate the live frame, or convert the total into FPS.

The CSV also reports `GS Invocations` equal to input primitives throughout graphics draws, while `GS Primitives` is zero and the app has no geometry-shader stage. Treat that column as anomalous, not evidence of a hidden geometry shader. Hardware/driver/RenderDoc versions and vendor utilization, bandwidth, cache and occupancy counters are absent; the export cannot prove an ALU or bandwidth bottleneck.

## Geometry and shading

**Water simulation:** spectrum update, FFT and map generation report 7.193 ms combined, separate from drawing the water surface. Each FFT dispatch performs a full 512-point line transform with nine butterfly stages and shared-memory barriers inside the shader; four dispatches mean two axes for each of the displacement/slope streams. Horizontal dispatches report 0.748/0.739 ms; vertical dispatches report 1.758/1.745 ms. The source's vertical loads/stores are strided in the row-major spectrum, making memory access a plausible contributor to this difference, without proving a bandwidth bottleneck.

**Terrain:** 12,451,840 submitted triangles, 37,355,520 index/vertex references, 12,702,822 vertex-shader invocations and 692,314 fragment invocations. The mesh contains 6,274,655 unique vertex instances (257² × 95), so reported vertex work is 2.024× that count. Row-by-row grid index order is a plausible cause of limited reuse between rows. This is an optimization lead, not a measured cache diagnosis. Height/normal evaluation executes per vertex, while terrain fragments perform materials, illumination and cloud shadows.

**Water:** 1,474,560 triangles, 4,423,680 index/vertex references, 1,510,216 vertex invocations and only 54,307 fragment invocations. Its 748,845 unique vertex instances (129² × 45) likewise imply 2.017× vertex work. Most submitted geometry yields little visible fragment coverage in this scene. Vertex evaluation includes per-owner terrain depth, distance filtering, shallow damping and enabled FFT cascade sampling. Boundary midpoint evaluations add work only at the perimeter. Fragment optics include sky/cloud reflection or transmission, foam and water lighting. The exports do not separate vertex cost from fragment cost, so the 21.692 ms duration cannot be assigned solely to either stage.

**Nearby trees:** each pass submits 12,247 triangles and reports 19,394 vertex invocations. Depth/coverage reports 274,287 fragment invocations and 127,499 passed samples; color reports 55,798 fragment invocations/passed samples. The second draw is the intentional depth-equal shading pass, not accidental duplicate color rendering. Coverage/alpha handling means the prepass is real fragment work.

**Imposters:** 54,492 quads are submitted in each of two passes, totaling 217,968 triangles and 442,628 vertex invocations across both. Each pass reports only 1,020 rasterized primitives and zero fragment invocations/passed samples. This is consistent with strong clipping/depth rejection in this view; it does not establish whether every rejection was due to occlusion, distance fade, or another shader condition. These batches still incur vertex and submission work despite producing no reported visible contribution. Counts from this single view do not justify removing the system.

**Canopy:** 197,701 fragment invocations versus 24,271 passed samples indicate considerable attempted shading relative to surviving coverage. Alpha/depth behavior matters; this ratio alone is not a precise overdraw factor.

## Full-screen work

The common full-screen fragment count is **1,094,225**, approximately 1.094 megapixels. Sky background writes 439,212 samples; transmission and scattering each write 655,013. Those complementary counts match the source's above-water background/foreground separation. All three draws still report a full viewport of fragment invocations, with shader discards deciding which pixels contribute.

The sky/medium sequence reports 6.280 ms combined. The source uses a background evaluation, analytic transmission and an additional scattering evaluation, and also evaluates GPU camera water classification. Clouds then report 13.953 ms for another full-screen invocation. Cloud shader work varies by ray/slab/footprint intersection, foreground depth, enabled sampling, sample budget and early termination. The exported files do not contain the captured uniform values, so current defaults are not proof of the capture's actual march counts. Clouds are a strong candidate for further measurement, independent of the geometry-heavy terrain/water paths.

Exposure plus display mapping totals only 0.220 ms in the export. UI/window composition adds 0.621 ms. None is the first optimization target suggested by this frame.

## Useful next measurements

1. Obtain a stable whole-frame GPU duration and batched-pass durations before assigning percentages; resolve the repeated nearby-foliage timing pattern first.
2. Compare the same camera/settings with clouds enabled/disabled or a lower cloud budget to isolate the volumetric cost. No expected improvement is assumed.
3. Evaluate cache-friendly grid index ordering without changing geometry, LOD or shading; the ~2× vertex invocation ratio is a concrete lead for terrain and water.
4. Inspect water vertex versus fragment utilization and rejected imposter batches with vendor counters or pipeline inspection. Keep measurements separate from visual-policy changes.

The user accepted the complete-mesh appearance on October 14, 2026. This capture confirms draw-path structure but supplies no matched bridge-era baseline, so it cannot quantify the bridge-removal speedup.

Follow-up live observation: the user found a similar view taking approximately 55 ms/frame, which fell below 40 ms with clouds disabled. This supports an approximately 15 ms cloud cost and makes clouds the next performance focus. The views were described as similar, so this is an approximate comparison rather than a controlled measurement of the captured frame.
