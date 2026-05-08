# Pine Tree Pack Converter

This folder contains a standalone offline asset-conversion project for turning the source pine tree pack into runtime-friendly binary files.

The required source art is not included in this branch. You need to provide the `assets/source/pinetreepack` FBX and TGA content yourself before the converter can produce runtime output.

The converter is intentionally separate from the main terrain sandbox runtime:

- the runtime should not need to link Assimp
- the runtime should not parse FBX or TGA directly
- expensive and format-specific import work should happen once offline
- the main app should later be able to load simple validated binary blobs quickly through SDL file IO

## Outputs

Running the converter for `pinetreepack` produces:

- `assets/runtime/pinetreepack.meshbin`
- `assets/runtime/pinetreepack.texbin`
- `assets/runtime/pinetreepack.assetbin`

Those generated runtime files are also not checked into this branch.

Those files are versioned binary containers described by [src/assets/RuntimeAssetFormat.hpp](C:/Users/siarr/source/repos/codex/src/assets/RuntimeAssetFormat.hpp) and read by the shared SDL-based reader in [src/assets/RuntimeAssetReader.hpp](C:/Users/siarr/source/repos/codex/src/assets/RuntimeAssetReader.hpp).

## Usage

From the repo root, configure and build the standalone converter project:

```powershell
cmake -S tools/converter -B build/converter
cmake --build build/converter
```

Then run either:

```powershell
.\build\converter\converter.exe pinetreepack
```

or:

```powershell
.\build\converter\converter.exe --source assets/source/pinetreepack --out assets/runtime --name pinetreepack
```

Default paths:

- source root: `assets/source/pinetreepack`
- FBX root: `assets/source/pinetreepack/fbx`
- texture root: `assets/source/pinetreepack/tex`
- output root: `assets/runtime`

## Architecture

The converter follows a two-layer split:

1. Import layer
   Reads source authoring formats and normalizes them into an in-memory `ImportedPack`.
2. Runtime-format layer
   Serializes that normalized pack into three binary files and immediately reloads them through the shared runtime reader for validation.

That separation keeps the import code format-aware while keeping the runtime format code simple and stable.

### Import Layer

- [FbxImport.cpp](C:/Users/siarr/source/repos/codex/tools/converter/FbxImport.cpp)
  Uses Assimp to load FBX meshes, convert FBX units to meters, traverse FBX node hierarchy, preserve per-node LOD grouping, bake node transforms into vertices, preserve tangent handedness, infer pack-specific material assignments, and compute per-mesh bounds.
- [TextureImport.cpp](C:/Users/siarr/source/repos/codex/tools/converter/TextureImport.cpp)
  Decodes TGA textures into RGBA8, infers sRGB versus linear usage from filenames, and deduplicates textures by normalized basename.
- [PineTreePackConverter.hpp](C:/Users/siarr/source/repos/codex/tools/converter/PineTreePackConverter.hpp)
  Defines the normalized in-memory data model used between import and serialization.

### Runtime-Format Layer

- [MeshBinWriter.cpp](C:/Users/siarr/source/repos/codex/tools/converter/MeshBinWriter.cpp)
  Writes mesh headers, mesh records, submesh records, per-mesh LZ4-compressed geometry blobs, and a string table.
- [TexBinWriter.cpp](C:/Users/siarr/source/repos/codex/tools/converter/TexBinWriter.cpp)
  Writes texture metadata, per-texture LZ4-compressed RGBA8 blobs, and a string table.
- [AssetBinWriter.cpp](C:/Users/siarr/source/repos/codex/tools/converter/AssetBinWriter.cpp)
  Writes the manifest that links assets, materials, meshes, textures, and the per-item blob compression metadata needed to reload `meshbin` and `texbin`.

### Shared Reader

The converter does not trust its own writes blindly. After emitting the three bins, it reloads them through the shared runtime reader:

- [RuntimeAssetReader.cpp](C:/Users/siarr/source/repos/codex/src/assets/RuntimeAssetReader.cpp)

That reader:

- uses `SDL_IOFromFile`
- reads the whole file into memory
- validates magic, version, flags, offsets, counts, and file-size bounds
- only exposes typed spans after validation succeeds

This is the same reader the main runtime can use later, which helps keep offline output and runtime expectations in lockstep.

It also now decompresses each mesh and texture blob individually on load using the compression metadata carried in `assetbin`.

## Current Runtime Use

The generated `pinetreepack` runtime bins are now used directly by the main app's nearby foliage path:

- `NearbyFoliageRenderer` loads `meshbin`, `texbin`, and `assetbin` at startup
- the nearby tree path uses real mesh LODs at `25m`, `50m`, and `100m` across the imported pine tree variants
- textures are uploaded once into shared `2D` texture arrays
- material metadata is uploaded once into a static GPU storage buffer

That makes converter correctness directly visible at runtime for:

- FBX unit conversion to meters
- LOD grouping
- material-to-texture assignment
- alpha-mask flags and cutoff values
- tangent-space correctness for normal maps

## Pine Pack Notes

For the current pine pack, the converter applies a few pack-specific rules to keep the runtime data sane:

- explicit `LOD0` / `LOD1` / `LOD2` / billboard grouping is inferred from FBX node names
- pine foliage materials are mapped onto the needles atlas textures instead of trusting ambiguous FBX references blindly
- billboard materials stay separate from nearby geometry materials
- `Bark_Bottom_Mat` is emitted as alpha-masked because its source color texture uses cutout alpha

These rules are intentionally local to the offline converter so the runtime stays source-format agnostic.

## Why Three Files

The split into `meshbin`, `texbin`, and `assetbin` is deliberate:

- `meshbin` contains geometry-heavy data that can be streamed or staged independently
- `texbin` contains large pixel blobs and is likely to evolve separately from geometry
- `assetbin` is the lightweight manifest that links meshes, materials, and textures together

This keeps the runtime loading story flexible without requiring source-format parsing in the app.

## Data Theory

The runtime formats aim for a few practical rules:

- fixed-width fields only
- explicit headers with magic, version, flags, offsets, counts, and file size
- string tables instead of JSON or nested text parsing
- simple flat records instead of deeply nested containers
- enough validation metadata to reject corrupt or mismatched files early

The current version still keeps the format flat and explicit, but now applies per-item LZ4 compression to mesh and texture payloads while leaving the manifest easy to validate.

## File Responsibilities

- [ConverterMain.cpp](C:/Users/siarr/source/repos/codex/tools/converter/ConverterMain.cpp)
  Parses CLI arguments and prints the summary.
- [PineTreePackConverter.cpp](C:/Users/siarr/source/repos/codex/tools/converter/PineTreePackConverter.cpp)
  Orchestrates import, serialization, reload validation, and summary reporting.
- [CMakeLists.txt](C:/Users/siarr/source/repos/codex/tools/converter/CMakeLists.txt)
  Defines the standalone build with its own `FetchContent` dependencies for SDL3 and Assimp.

## Important Constraint

Assimp is only used in this converter project. The main terrain sandbox runtime should only need the generated binary files plus the shared runtime reader.
