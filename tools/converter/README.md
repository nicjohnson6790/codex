# Runtime Asset Converter

This folder contains the standalone offline converter that turns source art into the runtime `meshbin`, `texbin`, and `assetbin` files used by the main app. Runtime camera, quadtree, canopy shell, and gameplay changes do not require regenerating these packs.

The converter is separate on purpose:

- the runtime app does not need to link Assimp
- the runtime app does not parse FBX, TGA, or PNG directly
- import-time cleanup, resizing, packing, and compression happen once offline
- the app can later load validated binary blobs quickly through the shared runtime reader

## Supported Packs

The converter currently builds three asset groups:

- `pinetreepack`
  - source root: `assets/source/pinetreepack`
  - outputs: `pinetreepack.meshbin`, `pinetreepack.texbin`, `pinetreepack.assetbin`
- `skybox`
  - source root: `assets/source/skybox`
  - outputs: `skybox.texbin`, `skybox.assetbin`
- `pbr`
  - source root: `assets/source/pbr`
  - outputs: `pbr.texbin`, `pbr.assetbin`
  - textures are resized to `1024x1024` with full mip chains; albedo maps use `BC3` sRGB, normal maps use `BC5`, and the remaining maps use `BC3` UNORM
- `roboto`
  - source root: `assets/source/font`
  - outputs: `roboto.texbin`, `roboto.assetbin`
  - the MSDF atlas is stored as uncompressed `RGBA8_UNORM` texels inside `texbin`; the texel payload is still LZ4-compressed like the other runtime textures
  - font atlas and glyph layout records are stored in `assetbin`

All generated outputs are written to `assets/runtime` and then staged into `build/<Config>/assets/runtime` by the main build.

## Source Assets

### Pine tree pack

The authored pine assets are not included in this repo.

The expected source pack is [Realistic Pine Trees Pack for games](https://www.cgtrader.com/3d-models/plant/conifer/realistic-pine-tree-pack-for-games) from CGTrader. At the time I checked, that listing describes:

- `16` custom pine trees
- `4` LODs per tree, with the last LOD as a billboard
- FBX source files
- PBR textures including base color, normal, roughness, specular, translucency/SSS, opacity, AO, and bark displacement

The converter expects that pack to be arranged like this:

- `assets/source/pinetreepack/fbx`
- `assets/source/pinetreepack/tex`

Without that external source content, the repo still builds, but you cannot regenerate the nearby tree runtime assets.

### Skybox pack

The repo does include the skybox source textures under:

- `assets/source/skybox/tex`

The converter expects six cubemap face images there:

- `px.png`
- `nx.png`
- `py.png`
- `ny.png`
- `pz.png`
- `nz.png`

These are intended to be the base nighttime cubemap that the runtime atmosphere shader runs on top of, not a fully baked final sky with atmospheric scattering already solved into it. All six faces should share the same dimensions because the runtime uploads them into one cubemap texture.

### Roboto font pack

The repo includes the Roboto variable TTF and its license under:

- `assets/source/font/Roboto-VariableFont_wdth,wght.ttf`
- `assets/source/font/OFL.txt`

The converter uses FreeType to flatten glyph outlines and build a `1024x1024` MSDF atlas for printable ASCII glyphs. The atlas texture is packed into `roboto.texbin`; `roboto.assetbin` carries the texture blob compression metadata plus binary font atlas and glyph records for later text layout.

## Build And Usage

From the repo root:

```powershell
tools\build.cmd Assets
```

That command configures the standalone converter into `build\Assets`, builds it, and regenerates the supported packs.

Manual runs after that:

```powershell
.\build\Assets\converter.exe skybox
.\build\Assets\converter.exe pinetreepack
.\build\Assets\converter.exe pbr
.\build\Assets\converter.exe roboto
```

Or with explicit paths:

```powershell
.\build\Assets\converter.exe --source assets/source/pinetreepack --out assets/runtime --name pinetreepack
```

Default roots:

- `pinetreepack`
  - source root: `assets/source/pinetreepack`
  - FBX root: `assets/source/pinetreepack/fbx`
  - texture root: `assets/source/pinetreepack/tex`
- `skybox`
  - source root: `assets/source/skybox`
  - texture root: `assets/source/skybox/tex`
- `pbr`
  - source root: `assets/source/pbr`
  - texture root: `assets/source/pbr/tex`
- `roboto`
  - source root: `assets/source/font`
  - font file: `assets/source/font/Roboto-VariableFont_wdth,wght.ttf`
- output root: `assets/runtime`

## Conversion Pipeline

The converter follows a simple two-stage flow:

- import source files into a normalized in-memory pack
- serialize that pack into runtime bins, then reload those bins through the shared runtime reader to validate them

That keeps source-format logic in the converter while keeping the runtime format flat and stable.

For the pine tree pack, that flow now includes an additional imposter-generation stage between import and final serialization.

## File Roles

### Entry and orchestration

- [ConverterMain.cpp](ConverterMain.cpp): CLI parsing and default pack routing
- [PineTreePackConverter.cpp](PineTreePackConverter.cpp): top-level import, write, reload validation, and summary reporting
- [PineTreePackConverter.hpp](PineTreePackConverter.hpp): normalized in-memory data model and pack configuration
- [CMakeLists.txt](CMakeLists.txt): standalone converter build and dependencies
- [BcTextureCompression.cpp](BcTextureCompression.cpp): converter-only BC3 and BC5 compression wrapper

### Import

- [FbxImport.cpp](FbxImport.cpp): FBX mesh import through Assimp, unit conversion, transforms, bounds, and LOD grouping
- [TextureImport.cpp](TextureImport.cpp): TGA and PNG decoding, texture normalization, resize rules, color-space inference, and deduplication
- [PineImposterGenerator.cpp](PineImposterGenerator.cpp): offscreen pine imposter capture, supersampled downfiltering, alpha-coverage-preserving mip generation, and BC compression setup
- [FontMsdfConverter.cpp](FontMsdfConverter.cpp): FreeType-based Roboto outline loading, MSDF atlas generation, and glyph metric record generation

### Runtime format writers

- [MeshBinWriter.cpp](MeshBinWriter.cpp): mesh metadata and per-mesh compressed geometry blobs
- [TexBinWriter.cpp](TexBinWriter.cpp): texture metadata and per-texture compressed RGBA8 blobs
- [AssetBinWriter.cpp](AssetBinWriter.cpp): manifest linking meshes, materials, textures, and per-item compression metadata

### Shared runtime reader

- [RuntimeAssetReader.cpp](../../src/assets/RuntimeAssetReader.cpp): validation and reload of emitted runtime bins
- [RuntimeAssetFormat.hpp](../../src/assets/RuntimeAssetFormat.hpp): binary format definitions
- [RuntimeAssetCompression.*](../../src/assets/RuntimeAssetCompression.cpp): shared LZ4 compression and decompression helpers

## Format Notes

The runtime pack format is intentionally simple:

- `meshbin` stores geometry-heavy payloads
- `texbin` stores texture pixel payloads, including BC-compressed 2D array imposter textures
- `assetbin` stores the lightweight manifest that links everything together
- font atlas and glyph records in `assetbin` store layout metadata for the Roboto atlas texture

Each mesh blob and texture blob is individually LZ4-compressed, and `assetbin` carries the metadata needed to decompress those items on load.

## Pine Texture And Imposter Budgets

The pine converter currently uses these texture budgets:

- imported pine material textures are resized to `1024x1024` for the runtime pack
- the imposter capture pass reloads the original source texture files so capture shading uses full source resolution
- the offscreen imposter render target is `1024x1024`
- the final stored imposter texture arrays are `512x512`
- each imposter texture contains `32` layers in `pitchIndex * 8 + yawIndex` order
- color/alpha imposter arrays use `BC3`
- normal imposter arrays use `BC5`
- both BC payloads are packed per-layer, per-mip and then LZ4-compressed inside `texbin`

The pine imposter generation flow is:

- import pine FBX meshes and material textures
- render `8` yaw views by `4` pitch views for each tree asset using the lowest non-billboard LOD
- capture albedo/alpha and normal outputs
- dilate RGB around alpha edges
- downsample the supersampled captures to the final imposter resolution
- build full mip chains with alpha-coverage preservation
- compress color/alpha to `BC3` and normal to `BC5`
- write the resulting array textures into `pinetreepack.texbin`
- write imposter texture references into `pinetreepack.assetbin`
- reopen the generated bins through the shared runtime reader and validate the metadata

## Runtime Use

The generated bins are loaded directly by the main app:

- `NearbyFoliageRenderer` loads the pine `meshbin`, `texbin`, and `assetbin` for nearby tree mesh rendering
- `FoliageImposterRenderer` loads the pine `texbin` and `assetbin` imposter metadata for the mid-distance tree pass
- `SkyboxRenderer` loads `skybox.texbin` and `skybox.assetbin`
- `QuadtreeMeshRenderer` loads `pbr.texbin` and `pbr.assetbin` for terrain material layers
- `WorldTextRenderer` loads `roboto.texbin` and `roboto.assetbin` for the MSDF atlas and glyph layout

That makes converter correctness immediately visible in the runtime for mesh layout, material wiring, texture assignment, normal mapping, alpha-mask handling, and skybox cubemap assembly.

## Dependencies

The converter fetches and uses:

- SDL3
- SDL3_image
- Assimp
- DirectXTex
- FreeType

These are converter-only dependencies. The main runtime app only consumes the generated binary packs and the shared runtime reader.
