# Runtime Asset Converter

This folder contains the standalone offline converter that turns source art into the runtime `meshbin`, `texbin`, `assetbin`, and tiled heightmap files used by the project. Runtime camera, quadtree, canopy shell, and gameplay changes do not require regenerating these packs.

The converter is separate on purpose:

- the runtime app does not need to link Assimp
- the runtime app does not parse FBX, TGA, or PNG directly
- import-time cleanup, resizing, packing, and compression happen once offline
- the app can later load validated binary blobs quickly through the shared runtime reader

## Supported Packs

The converter currently builds six asset groups:

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
- `etopo2022`
  - source: `assets/source/etopo2022/ETOPO_2022_v1_60s_N90W180_surface.tif`
  - outputs: `etopo2022.assetbin`, `etopo2022.heightbin`, and diagnostic previews
  - provides the required global base heightmap
- `japan-dem10`
  - source root: `assets/source/japan-dem10`
  - requires the generated ETOPO pack to compute additive deltas
  - outputs four `japan_dem10_delta_<id>.assetbin`/`.heightbin` pairs, provenance metadata, and diagnostic previews
  - is optional at runtime and contributes only at terrain pitches of 32 m or finer

All generated outputs are written to `assets/runtime` and then staged into `build/<Config>/app/assets/runtime` by the main build. Converter executables are isolated under `build/Assets/<Config>/converter`.

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

### PBR terrain pack

Terrain material textures are read from `assets/source/pbr/tex`. The converter
normalizes them to `1024x1024`, builds complete mip chains, and writes BC3 sRGB
albedo, BC5 normal, and BC3 UNORM data for the remaining material channels.

### Roboto font pack

The repo includes the Roboto variable TTF and its license under:

- `assets/source/font/Roboto-VariableFont_wdth,wght.ttf`
- `assets/source/font/OFL.txt`

The converter uses FreeType to flatten glyph outlines and build a `1024x1024` MSDF atlas for printable ASCII glyphs. The atlas texture is packed into `roboto.texbin`; `roboto.assetbin` carries the texture blob compression metadata plus binary font atlas and glyph records for later text layout.

## Build And Usage

From the repo root:

```powershell
tools\build.cmd Assets
tools\build.cmd Assets Debug
```

The configuration argument is optional and defaults to Release. These commands
build the standalone converter into `build\Assets\Release\converter` and
`build\Assets\Debug\converter`, respectively. They do not generate any asset packs;
conversions are explicit offline operations.

All converters report progress unconditionally. File-based packs print each
source file, generated item, and serialized blob as it is processed. The ETOPO
and Japan DEM10 converters print an update after every candidate heightmap tile,
including completed count, percentage, elapsed time, and estimated time
remaining. Output is flushed immediately so long conversions remain observable.

Manual runs after that:

```powershell
.\build\Assets\Release\converter\converter.exe skybox
.\build\Assets\Release\converter\converter.exe pinetreepack
.\build\Assets\Release\converter\converter.exe pbr
.\build\Assets\Release\converter\converter.exe roboto
.\build\Assets\Release\converter\converter.exe etopo2022
.\build\Assets\Release\converter\converter.exe japan-dem10
```

Or with explicit paths:

```powershell
.\build\Assets\Release\converter\converter.exe --name pinetreepack --source assets/source/pinetreepack --out assets/runtime
```

For the four art/font modes, place `--name` before path overrides because
selecting a named pack applies that pack's defaults. The two heightmap modes use
their mode name first and accept the mode-specific options shown below.

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
- `etopo2022`
  - source TIFF: `assets/source/etopo2022/ETOPO_2022_v1_60s_N90W180_surface.tif`
- `japan-dem10`
  - source root: `assets/source/japan-dem10`
  - ETOPO index: `assets/runtime/etopo2022.assetbin`
- output root: `assets/runtime`

## ETOPO 2022 global base-height atlas

ETOPO generation is deliberately separate from `tools\build.cmd Assets`: the official input is approximately 444 MB and is not committed. Download, build, and convert it explicitly from the repository root:

```powershell
tools\download_etopo2022.cmd
tools\build.cmd Assets
.\build\Assets\Release\converter\converter.exe etopo2022
```

The download script uses Windows `curl.exe`, retries failures, downloads to a `.part` file, and only replaces the final file after success. Pass `--force` to download it again. The official source is NOAA's [ETOPO 2022 60 arc-second surface elevation GeoTIFF](https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO2022/data/60s/60s_surface_elev_gtif/ETOPO_2022_v1_60s_N90W180_surface.tif), stored as `assets/source/etopo2022/ETOPO_2022_v1_60s_N90W180_surface.tif`.

To build only the already-configured converter directly:

```powershell
cmake --build build\Assets\Release --target converter --parallel
```

Input/output overrides and tile logging are available:

```powershell
.\build\Assets\Release\converter\converter.exe etopo2022 --source D:\data\etopo.tif --out D:\generated --verbose
```

The dependency-independent filter/projection checks can be run without the TIFF:

```powershell
.\build\Assets\Release\converter\converter.exe etopo2022 --self-test
```

The output is:

- `etopo2022.assetbin`: compact header and tile-coordinate index;
- `etopo2022.heightbin`: independently compressed tile blobs;
- `etopo2022_preview.png`: diagnostic hypsometric projection preview with tile lines (not runtime data).
- `etopo2022_tiles_preview.png`: dense near-square contact sheet of 32x32 thumbnails decoded from every indexed LZ4 tile blob, in index/blob order. Fully invalid discarded tiles consume no cell; only unused cells at the end of the final row are empty (not runtime data).

Projection version 3 is a fixed Airocean "one-island" icosahedral gnomonic net on the authalic sphere (radius `6,371,007.180918475 m`). Its face tree keeps the north-pole faces connected and routes most cuts through oceans; three faces are subdivided so the cuts pass around Japan and Australia. The globe orientation is `(-83.65929, 25.44458, -87.45184)` degrees and the unfolded net is rotated `-60 degrees` in atlas space. All three orientation components and the atlas rotation are stored explicitly in format-version-2 pack headers, while the precise cut topology is identified by projection version 3. This is an Airocean-layout gnomonic implementation, not Fuller's proprietary per-face transform. The atlas coordinate system is independent of Codex's `Position` grid and does not bake in runtime terrain resolution or world placement.

Tiles have a physical footprint of exactly `524,288 m` and signed `int8` coordinates. Tile `(x,y)` begins at atlas coordinate `(x * 524288, y * 524288)`. Each tile stores `256x256` signed 16-bit integer-meter samples on a global lattice with a stride of 255 intervals, so neighboring tiles duplicate their shared border bit-for-bit. `INT16_MIN` marks samples outside the unfolded projection; only completely invalid tiles are omitted. Ocean and bathymetry remain in the pack.

Each tile is filtered independently using serpentine spatial traversal, modulo-16-bit first differences, signed ZigZag folding, and low/high byte planes, then compressed as its own normal LZ4 blob. The small index contains every tile's signed coordinate, blob offset, compressed size, fixed filtered size, valid sample count, and elevation range, so tile lookup never requires scanning `heightbin`.

Index records are sorted deterministically by `tileY`, then `tileX`.

Generation validates source dimensions/georeferencing/sample type, exhaustively self-tests the reversible residual transform, round-trips every filtered/compressed tile, compares every emitted shared edge, validates index ranges and uniqueness, then closes and reopens both output files and decodes all blobs. Runtime sampling, affine world placement, and final terrain composition are implemented by the runtime; user-created heightmap layers remain outside this converter stage.

## Japan DEM10 ETOPO-relative delta mosaics

Japan's GSI DEM10 conversion is an explicit offline operation. It is not run by
the normal asset build. When present in `assets/runtime`, the game registers the
four generated mosaics as additive ETOPO-relative sources for final heightmaps
whose sample pitch is at most 32 m.
The downloader always refreshes Source Cooperative's current-file manifest and
verifies every cached GeoTIFF using both the declared byte size and MD5:

```powershell
tools\download_japan_dem10.cmd
tools\build.cmd Assets
.\build\Assets\Release\converter\converter.exe japan-dem10
```

Use `--source`, `--etopo`, and `--out` to override the DEM10 directory, existing
Codex ETOPO index, and destination. `--self-test` runs dependency-independent
filter, placement, basis, cross-raster bilinear sampling, 2 km ocean-collar,
and positive-edge ownership checks. Normal conversions
report source-indexing progress and print an update after every candidate tile,
including completion percentage, valid-sample and stored-tile counts, elapsed
time, and ETA. It also writes `japan_dem10_delta_coverage.svg` with a taller,
Mt. Fuji-centered overview and one densely packed 32x32-per-tile preview PNG per
mosaic (`japan_dem10_delta_<id>_preview.png`), following ETOPO's packed-preview
layout. Blue areas are below ETOPO, red areas are above it, and near-zero deltas
are dark neutral.

The converter's libtiff build includes ZSTD support, as required by the Source
Cooperative DEM10 GeoTIFFs. The command reads EPSG:6668 and NoData metadata from every GeoTIFF, bilinearly
samples DEM10 on a fixed rotated 10 m grid, resolving bilinear footprints across
neighboring GeoTIFF files rather than treating file boundaries as missing data, and subtracts a bilinear sample
decoded from the existing `etopo2022.assetbin`/`heightbin`. It emits four sparse
`japan_dem10_delta_<id>.assetbin`/`.heightbin` packs, placement/provenance JSON,
and an SVG coverage/footprint preview. Every mosaic uses signed tile coordinates
`-128..127`; its `tileX=127/sampleX=255` and `tileY=127/sampleY=255` edges are
always zero. The transform is deliberately fixed in source and repeated in the
metadata so future runtime integration cannot silently choose a different fit.

Where DEM10 becomes NoData at the coast, the converter emits a 2 km correction
collar that cancels positive ETOPO elevation while retaining existing ETOPO
bathymetry. The correction fades to zero across the collar and is limited to
the vicinity of the source GeoTIFF coverage.

Conversion keeps decoded ETOPO tiles resident for reuse and maintains a bounded
eight-raster DEM10 working set. This prevents repeated TIFF/ZSTD and ETOPO/LZ4
decode work when output tiles cross source boundaries while keeping nationwide
source-memory use bounded.

The current four-cell layout is `x / x / xx` in one common basis, rotated 20.5
degrees with the long axis following southwest-to-northeast Japan. It derives
from the earlier five-cell layout by rotating 7.5 degrees clockwise about the
four-cell intersection, shifting 7.5 km west, and dropping `cw`. Generated packs and downloaded GSI
data remain under the ignored `assets` tree and must not be committed. The GSI
source carries attribution and reproduction conditions; review them before
redistributing generated products.

Suggested quality validation after a full conversion is to inspect Mt. Fuji and
representative locations by reconstructing `ETOPO + delta`, confirm the result
matches DEM10 to integer-meter encoding tolerance, and inspect
`japan_dem10_delta_coverage.svg` for footprint coverage.

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
- [EtopoHeightmapConverter.cpp](EtopoHeightmapConverter.cpp): global ETOPO projection, tiling, filtering, previews, and pack validation
- [JapanDem10Converter.cpp](JapanDem10Converter.cpp): DEM10 catalog sampling, ETOPO-relative delta generation, coastline collar, mosaics, metadata, and previews
- [EtopoGeoTiffReader.cpp](EtopoGeoTiffReader.cpp): GeoTIFF metadata and raster access shared by heightmap conversion
- [IcosahedralProjection.cpp](IcosahedralProjection.cpp): fixed Airocean-layout projection used by converter and runtime
- [HeightmapTileFilter.cpp](HeightmapTileFilter.cpp): reversible height-sample filter used before LZ4 compression

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
- `WorldGridQuadtreeHeightmapManager` loads `etopo2022.assetbin`/`.heightbin` as the global base source and optionally registers all four Japan DEM10 delta packs with their fixed affine placements and 32 m pitch gate
- composed final heightmaps retain source references while resident; `QuadtreeMeshRenderer` owns the GPU source cache and final-heightmap resources

That makes converter correctness immediately visible in the runtime for mesh layout, material wiring, texture assignment, normal mapping, alpha-mask handling, and skybox cubemap assembly.

## Dependencies

The converter fetches and uses:

- SDL3
- SDL3_image
- Assimp
- DirectXTex
- FreeType
- libtiff, with ZSTD and DEFLATE support for source GeoTIFFs
- LZ4 for independently compressed runtime blobs

These are fetched by the converter build. Assimp, DirectXTex, FreeType, libtiff,
and ZSTD are converter-only; the main runtime consumes the generated binary
packs through the shared reader and uses LZ4 to decode their individual blobs.
