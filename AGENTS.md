# Agent Working Notes

This file is the durable handoff point for coding agents working in this repository. Read it at the start of a session and update it when work changes an enduring project fact, workflow, constraint, or unfinished thread.

Keep notes concise and current. Do not append command transcripts or routine progress logs. Remove or rewrite obsolete notes when their underlying work is completed.

## Project orientation

- Codex is a Windows C++20 terrain sandbox using SDL3 GPU, Dear ImGui, CMake, Ninja, and GLSL compiled to SPIR-V.
- The root [README.md](README.md) is intentionally an onboarding document. Detailed rendering design belongs in [docs/codex_rendering_architecture.md](docs/codex_rendering_architecture.md).
- Canonical builds are `tools\build.cmd Debug`, `tools\build.cmd Release`, and `tools\build.cmd Assets`.
- Canonical build directories are `build/Debug`, `build/Release`, and `build/Assets`. Avoid creating alternate build-directory names for routine validation.
- The build/configure scripts detect CMake caches that reference a removed MSVC compiler and automatically reconfigure with `cmake --fresh`.

## Durable constraints

- The entire `assets` directory is ignored by Git except for shared format/reader source code under `src/assets`.
- Runtime asset packs are generated into `assets/runtime` and staged into each app build. Do not commit generated packs or externally licensed source assets.
- Steamworks support is optional. The default SDK location is `../deps/steamworks_sdk_164/sdk`; local runs can use `--disable-steam`.
- The ETOPO converter is an explicit offline operation, not part of the normal asset build. Its format is version 2 and its fixed Airocean projection is version 3.
- The ETOPO runtime sampler, terrain integration, user layers, and world-placement transforms have not been implemented yet.
- Preserve unrelated working-tree changes and use `apply_patch` for source/text edits.

## Session handoff notes

- The offline ETOPO heightmap converter and stale-MSVC-cache recovery are committed on local `main`.
- The rendering architecture document now also covers the offline asset boundary and multiplayer render-emission boundary.
- Add concrete unfinished work here only when it must survive into another session; include the relevant file or subsystem and the next useful action.
