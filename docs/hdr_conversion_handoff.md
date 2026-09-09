# Linear HDR conversion handoff

## Implementation

The viewport now composes terrain, water, canopy, imposters, nearby foliage, sky/atmosphere and clouds in an R16G16B16A16_FLOAT linear Rec.709 scene target. SDLRenderer owns that texture, the existing ordinary UNORM display texture, and one shared depth texture. The existing post-submission resize lifecycle is preserved.

DisplayTransformRenderer performs the only scene display conversion: camera exposure (default 1.0), the exponential shoulder, and exact piecewise sRGB encoding. It writes opaque alpha with blending disabled. ImGui samples the resulting ordinary UNORM texture into the unchanged SDR swapchain, without another transfer. Triangle/pawn/multiplayer indicators, lines and world text render afterward using their original display pipelines and scene depth; they intentionally bypass media and exposure.

The shared atmosphere function calibrates every scattering result with the retained atmosphere/cloud source scale 4.8 and environment scale 12. Transmission is unchanged. Cloud sunlight retains 4.8; the complete local lighting expression receives 12 only after its sqrt(sunlight) ambient term. Accumulated cloud RGB returns directly as linear premultiplied radiance. The sky wrapper adds separately calibrated, GPU sRGB-decoded space radiance (0.0001), with no additional ×12 or adaptation. Water-medium source scale remains 4.8 in its original equation.

The complete material/texture audit and frame contract are in [codex_rendering_architecture.md](codex_rendering_architecture.md#viewport-radiance-and-display-contract). No assets were regenerated and no source, material, cloud, wave, foam, or Fresnel tuning was applied. Temporary camera/lighting/wave-time overrides were removed.

## Technical validation

| Required check | Result |
| --- | --- |
| Canonical Debug and Release builds | Passed, including shader compilation. |
| Full test suite, both configurations | All six tests passed in each configuration. |
| Actual display-transfer shader math | Compiled as C++ by atmosphere_math_tests; zero, finite extremes, monotonicity, range, sRGB breakpoint, authored-color decode and exposure multiplication checked. Shared scattering calibration and separate space contribution checked against actual GLSL; dense atmosphere reference includes the relocated factor. |
| Generated SPIR-V | All 68 modules across Debug/Release validated. |
| Debug cloud traversal | 720 frames passed, 12 stages and 24 macro revisions; below/inside/above/horizontal rays, cell crossings, seeds and extreme universe coordinates. No Vulkan validation errors. |
| Release startup | 120-frame run exited successfully. |
| Fixed horizon | Reproduced and inspected through the Windows capture tool against the saved pre-HDR baseline. Brown band absent; see observations below. |

The existing SDL Vulkan 3D-image maintenance9 forward-compatibility warning remains; this is not an HDR pipeline validation error. Runtime logs are local ignored files under build/Debug/app/hdr-cloud-traversal.log and build/Release/app/hdr-startup.log. These checks do not establish full visual acceptance or performance.

## Visual observations and pending user review

The deterministic comparison used Release Vulkan, viewport 1265 × 865, camera cell (23, -1), local (524250, 300, 347000), direction normalize(0, -0.05, -1), 15:00, clouds disabled, and wave time held at 10 seconds. The existing build/horizon-diagnostic/01-normal.png baseline has a distinct brown band directly below the horizon. In the HDR capture the distant water blends into bright adjacent haze without that band. Water and haze are substantially brighter at the retained source settings and camera exposure 1.0. No water-specific compensation was used.

The first live check caught a vertically inverted final copy. The final shader uses texelFetch at gl_FragCoord, matching the identically sized targets; the corrected horizon and subsequent cameras were inspected upright.

| Case | Exercised evidence | Remaining user review |
| --- | --- | --- |
| Daylight ocean/horizon/foam | Reference view, clouds off/on; bright water/sky and white foam highlights, no prior brown band. | Overall brightness, highlight detail and approved appearance. |
| Near sunset | Same direction at 23.69 on the existing orbital clock; warm sky and reflected water share a continuous color transition. | Sunrise and sunset both toward and away from the sun, with land silhouettes. |
| Night / space | 5.79 orbital-clock night and 100 km outward daytime view, camera exposure 1.0: nearly black, with very faint detail. | Accept reduced visibility or direct a separate exposure/adaptation follow-up. No compensation proposed/applied. |
| Underwater | −3 m, upward normalize(0,0.8,−1): bright transmitted sky and dark TIR regions. −20 m, normalize(0,0.5,−1): stronger blue attenuation and darker surrounding medium. +0.25 m surface view also inspected. | Wave-crossing continuity, critical-angle appearance, bridge artifacts and underwater cloud transmission. Abrupt wave-shaped TIR boundaries remain visible; this is not a claim of underwater appearance approval. |
| Clouds | Enabled/disabled at reference ocean view; visible horizon clouds and water environment use shared HDR integration. Automated traversal passed. | Cloud edge/highlight appearance against terrain, reflections and underside transmission, and performance. |
| Terrain / canopy / imposters / nearby foliage | Pipeline formats, runtime source decode and shader paths audited; GPU traversal exercised residency and scene rendering. | Representative settled land/material views and shoreline foam need direct appearance review; no land comparison capture is claimed. |
| World origin | Existing 12-stage traversal covers cell changes and extreme universe coordinates. | Representative interactive motion and appearance. |
| Overlays / ImGui | Post-display ordering and original alpha/depth state retained; ImGui/viewport orientation inspected across the above cases. | Depth-aware line/triangle/world-text visibility and colors while varying exposure, including multiplayer indicators. |

User visual approval is pending for the full matrix. No new exposure/source defaults are proposed. No implementation-caused automated failures remain, and no required build/test prerequisite was missing. Broader visual checks above are explicitly unaccepted; automated success must not be described as full visual validation.
