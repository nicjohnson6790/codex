# HDR conversion planning handoff

## Confirmed problem

A fixed-camera test reproduced the dark/brown water horizon. Bypassing camera-to-surface air removed the band; the raw reflected sky was bright. Transmission-only showed strong dark/yellow attenuation. Scattering-only was dim, while the same scattering passed through `displaySkyRadiance` approached the bright adjacent sky. This isolates the reproduced defect to the later atmospheric compositor, not Fresnel or duplicate integration of the same path.

Test setup: Release Vulkan, 1265 x 865 viewport, camera cell (23, -1), local (524250, 300, 347000), direction normalize(0, -0.05, -1), 15:00, clouds disabled, wave simulation time held at 10 seconds. Local screenshots and full methodology are in the ignored `build/horizon-diagnostic` directory. All temporary overrides were restored; no HDR fix is implemented.

## Current implementation

- Water reflection and non-TIR underside transmission share cloud integration with the visible cloud pass. CloudRenderer owns the textures and prepares common field/lighting state once in the early upload phase. Water defaults to 24 view / 3 sun samples; its multiplier changes only those counts.
- `shaders/atmosphere.glsl`: `displaySkyRadiance` combines sky-specific radiance calibration, directional adaptation, tone mapping and display encoding.
- `shaders/water_mesh.frag`: sampled sky/cloud environment is display-mapped before the surface response.
- `shaders/skybox.frag` and `src/SkyboxRenderer.cpp`: later passes multiply displayed geometry by linear transmission and add raw linear scattering. Sky backgrounds instead receive the sky display mapping.
- `shaders/cloud_integration.glsl`: integrates linear cloud radiance, maps the effective cloud source once, and returns displayed premultiplied color/opacity.
- `src/SDLRenderer.cpp`: owns viewport targets, pass ordering and the texture presented by ImGui.

## Planning questions

Design a coherent linear-radiance pipeline: surface/environment lighting, camera-side atmosphere and clouds compose before a single final display transform. Consider an RGBA16_FLOAT scene target and a separate display target for the editor viewport. Do not move `displaySkyRadiance` unchanged into a final pass: first decide how physical/artist lighting scales, sky calibration, directional adaptation and underwater exposure should relate.

Audit every material, texture decode, blend/alpha convention, emissive/debug output and environment consumer. Preserve renderer ownership, bounded world phases, foam motion, water interfaces and cloud budgets. Decide the migration sequence, exposure policy, tone mapper, output encoding and how to retain the approved appearance without water-specific compensation.

Acceptance should include the fixed horizon comparison plus daylight, sunset/night, underwater transmission/TIR, clouds on/off, foam, terrain/foliage and render-origin transitions. The current water/cloud implementation passed Debug/Release builds, all six tests in both configurations, SPIR-V validation and the 720-frame cloud traversal; those do not validate the future HDR design or its appearance.
