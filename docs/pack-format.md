# vgl Pack Format Specification

| | |
|---|---|
| **Status** | Draft 0.1 |
| **Format version** | 1.0 |
| **Date** | 13 October 2026 |
| **Author** | Jeremiah van Oosten |
| **Scope** | Tiling scheme, file layout and tile encodings of baked tile packs. |

This specification expands sections 7.3, 8.4 and 8.5 of the [design document](design.md). The baker writes packs in this format and the runtime reads them. Both use the same implementation of the tiling rules in section 2, from the shared tiling module (design document, section 8.2).

The key words "must", "must not", "should" and "may" are used in their usual specification sense. Sections marked *informative* explain or give examples and contain no requirements.

---

## 1. Overview

A **pack** holds one layer of data, either elevation or imagery, for some part of the globe. The runtime stacks several packs per layer by priority (FR-05), so a global base pack and regional high-resolution packs combine without rebaking. A third kind of pack holds the geoid model, which converts between ellipsoidal heights and heights above mean sea level (section 6.4).

A pack is a directory with three kinds of file:

```
<pack>/
    pack.vglm                     Manifest: metadata, layer description, shard index
    base-<digest>.vgls            Base shard: all tiles above the shard level
    shards/<y>/<x>-<digest>.vgls  One shard per tile at the shard level, for the subtree below it
```

- The **manifest** is small and is read once when the pack is added.
- Each **shard** is a self-contained file holding one subtree of the quadtree, with its own tile directory. The runtime reads a shard's directory only when it first needs a tile from that shard.
- Shard file names contain a digest of their content (section 5.4). A rebaked shard therefore gets a new name, and the manifest is the only file whose content changes in place.

All files are range-readable: every structure can be located from data read earlier, so a reader on a plain web server needs no server-side code.

### 1.1 Reading a tile (informative)

1. Read `pack.vglm` and keep it in memory.
2. Find the shard that holds tile (*L*, *x*, *y*): the base shard if *L* is above the shard level, otherwise the shard of its ancestor at the shard level. Look it up in the shard index. If it is absent, the tile does not exist in this pack.
3. If the shard's directory is not cached yet, read the shard header and directory. Both fit in the first 64 KiB of a typical shard, so one range request usually suffices.
4. Binary-search the directory for the tile key. If it is absent, the tile does not exist.
5. Read the tile's byte range, check its CRC, and decode it.

---

## 2. Tiling scheme

### 2.1 Tile addresses

The globe is covered by a geographic (equirectangular) quadtree on longitude and geodetic latitude. A tile is addressed by its level *L* and its column *x* and row *y*:

- Level 0 has two tiles of 180° × 180°: (0, 0, 0) covers the western hemisphere and (0, 1, 0) the eastern hemisphere.
- Level *L* has 2<sup>*L*+1</sup> columns and 2<sup>*L*</sup> rows. Each tile spans 180° / 2<sup>*L*</sup> in both longitude and latitude.
- Column *x* counts eastwards from the antimeridian, and row *y* counts southwards from the north pole.
- Tile (*L*, *x*, *y*) covers longitudes [−180° + *x*·*s*, −180° + (*x*+1)·*s*] and latitudes [90° − (*y*+1)·*s*, 90° − *y*·*s*], where *s* = 180° / 2<sup>*L*</sup>.
- The children of (*L*, *x*, *y*) are (*L*+1, 2*x*+*i*, 2*y*+*j*) for *i*, *j* ∈ {0, 1}. They are numbered *c* = *i* + 2*j*: 0 north-west, 1 north-east, 2 south-west, 3 south-east.
- Valid levels are 0 to 28. A pack may use any contiguous range of them.

Rows start at the north so that tile rows and image rows run in the same direction.

### 2.2 Tile keys

A tile's 64-bit **key** orders tiles by level, then along a Morton (Z-order) curve:

```
key = (L << 58) | morton(x, y)
```

where `morton(x, y)` places bit *i* of *x* at bit 2*i* and bit *i* of *y* at bit 2*i*+1. Within each 2 × 2 block of siblings, Morton order matches the child numbering of section 2.1. Siblings are adjacent in key order, and every subtree occupies a contiguous key range within each level.

Example: tile (7, 181, 37) has key `0x1C00000000004D33`.

### 2.3 Elevation sample grid

Elevation tiles are grid-registered: samples lie on the tile edges, and neighbouring tiles share their border rows and columns. A tile of size *N* (the manifest's `tile_size`, 2<sup>*k*</sup> + 1, for example 129) has *N* × *N* samples. Sample (*i*, *j*), with column *i* and row *j* both in [0, *N* − 1], lies at:

```
longitude = west  + i * s / (N - 1)
latitude  = north - j * s / (N - 1)
```

Rows run from north to south, and samples are stored row by row.

- **Shared borders.** The east column of a tile and the west column of its eastern neighbour lie on the same meridian and must hold the same decoded heights. The same applies to rows shared with northern and southern neighbours, and across the antimeridian between the last and first columns.
- **Children.** The samples of a child at even indices coincide with sample positions of its parent. Their values may differ, because the baker builds coarser levels by filtering.
- **Poles.** Row 0 of the tiles in row 0 lies on the north pole, and row *N* − 1 of the tiles in the last row lies on the south pole. All samples of a pole row must hold the same decoded height.

### 2.4 Imagery pixel grid

Imagery tiles are cell-registered: pixels cover areas, and neighbouring tiles share no pixels. A tile of size *P* (the manifest's `tile_size`, a power of two such as 256 or 512) with border *b* (the manifest's `border`) is an image of (*P* + 2*b*) × (*P* + 2*b*) pixels. Image pixel (*i*, *j*) covers longitudes [west + (*i* − *b*)·*s*/*P*, west + (*i* − *b* + 1)·*s*/*P*], and the corresponding latitudes counted down from the north edge.

The inner *P* × *P* pixels cover the tile exactly. The border pixels repeat data from the neighbouring tiles, so that the runtime can filter textures across tile edges without seams. Format version 1.0 packs use *b* = 0 or *b* = 1.

### 2.5 Level reference (informative)

Spacing is shown for 129-sample elevation tiles, at the equator, on WGS84.

| Level | Tile span | Sample spacing | Tiles in full level |
|---|---|---|---|
| 0 | 180° | 156.5 km | 2 |
| 4 | 11.25° | 9.8 km | 512 |
| 6 | 2.8125° | 2.4 km | 8,192 |
| 8 | 0.703125° | 611.5 m | 131,072 |
| 10 | 0.17578125° | 152.9 m (4.9″) | 2,097,152 |
| 11 | 0.087890625° | 76.4 m (2.5″) | 8,388,608 |
| 12 | 0.0439453125° | 38.2 m (1.2″) | 33,554,432 |
| 13 | 0.02197265625° | 19.1 m (0.6″) | 134,217,728 |

---

## 3. Common conventions

- All multi-byte values are little-endian. Readers must not assume alignment.
- `u8` to `u64` are unsigned integers, `i16` is a two's complement integer, and `f32` and `f64` are IEEE 754 binary32 and binary64.
- A `string` is a `u32` byte length followed by that many bytes of UTF-8, with no terminator.
- Fields marked *reserved* must be written as zero. Readers must ignore their contents, so that later minor versions can use them.
- **CRC-32C** is the Castagnoli CRC: reflected polynomial `0x82F63B78`, initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`. The CRC of the ASCII bytes `123456789` is `0xE3069283`.
- **Zstandard** data is a single Zstandard frame (RFC 8878) with the content size field present. A reader must check that the decompressed size equals the size this specification requires.
- Offsets and sizes are byte counts. Offsets are relative to the start of the file unless stated otherwise.

---

## 4. Manifest

The manifest is the file `pack.vglm` in the pack directory. It is a header followed by a table of sections.

### 4.1 Header

| Offset | Type | Field | Description |
|---|---|---|---|
| 0 | `u8[4]` | `magic` | `VGLM` (`56 47 4C 4D`) |
| 4 | `u16` | `major_version` | 1 |
| 6 | `u16` | `minor_version` | 0 |
| 8 | `u32` | `section_count` | Number of entries in the section table |
| 12 | `u32` | `flags` | Reserved |
| 16 | `u64` | `file_size` | Size of the manifest file in bytes |
| 24 | `u32` | `table_crc` | CRC-32C of the section table |
| 28 | `u32` | `header_crc` | CRC-32C of bytes 0 to 27 |

The section table follows at offset 32, with `section_count` entries of 32 bytes:

| Offset | Type | Field | Description |
|---|---|---|---|
| 0 | `u32` | `type` | Section type (section 4.2). Bit 31 set means the section is optional. |
| 4 | `u32` | `flags` | Bit 0: the section is Zstandard-compressed. Other bits reserved. |
| 8 | `u64` | `offset` | Offset of the stored section data |
| 16 | `u32` | `stored_size` | Size of the stored data |
| 20 | `u32` | `size` | Size after decompression; equal to `stored_size` if uncompressed |
| 24 | `u32` | `crc` | CRC-32C of the stored data |
| 28 | `u32` | | Reserved |

Each section type appears at most once. Sections may be stored in any order and must not overlap.

A reader must reject a manifest with an unknown required section type (bit 31 clear), and must skip unknown optional sections. This is how later minor versions add information that older readers can safely ignore.

### 4.2 Section types

| Type | Name | Required | Contents |
|---|---|---|---|
| `0x00000001` | `LAYER` | Always | Layer description (section 4.3) |
| `0x00000002` | `HEIGHT_STEPS` | Elevation and geoid packs | Quantization step per level (section 4.4) |
| `0x00000003` | `SHARD_INDEX` | Always | Which shards exist (section 4.5) |
| `0x00000004` | `METADATA` | Always | Attribution and other text (section 4.6) |

### 4.3 LAYER section

| Offset | Type | Field | Description |
|---|---|---|---|
| 0 | `u8` | `layer_type` | 1 elevation, 2 imagery, 3 geoid |
| 1 | `u8` | `tiling_scheme` | 1: the geographic scheme of section 2 |
| 2 | `u16` | `tile_size` | Elevation: samples per side, 2<sup>*k*</sup> + 1 with 2 ≤ *k* ≤ 10. Imagery: inner pixels per side, a power of two from 64 to 2048. |
| 4 | `u8` | `min_level` | Coarsest level in the pack |
| 5 | `u8` | `max_level` | Finest level in the pack, at most 28 |
| 6 | `u8` | `shard_level` | Level of the shard roots (section 5.1) |
| 7 | `u8` | `border` | Imagery border *b* (section 2.4); 0 for elevation |
| 8 | `u8` | `height_reference` | Elevation and geoid: 1, heights are relative to the ellipsoid below. Imagery: 0. |
| 9 | `u8[7]` | | Reserved |
| 16 | `f64` | `semi_major_axis` | Ellipsoid semi-major axis in metres (WGS84: 6378137.0) |
| 24 | `f64` | `inverse_flattening` | Ellipsoid inverse flattening (WGS84: 298.257223563) |
| 32 | `f64` | `west` | Coverage bounds in degrees. Informative: tile existence is defined by the shard index and directories. |
| 40 | `f64` | `south` | |
| 48 | `f64` | `east` | |
| 56 | `f64` | `north` | |

The section is 64 bytes in version 1.0, and must not be shorter. Later minor versions may append fields: a reader must accept a longer section, and use defaults for appended fields that a shorter section from an older minor version lacks.

All heights in a pack are ellipsoidal heights (design document, section 8.3). The baker converts source datums; the `source` metadata entries record what they were.

A runtime must reject a pack whose ellipsoid differs from the ellipsoid of its context.

### 4.4 HEIGHT_STEPS section

An array of `f64`, one per level from `min_level` to `max_level`, giving the quantization step of that level in metres (section 6.2). Each step must be a power of two, such as 0.125 or 0.5, so that decoding is exact in binary floating point.

### 4.5 SHARD_INDEX section

| Offset | Type | Field | Description |
|---|---|---|---|
| 0 | `u32` | `shard_count` | Number of entries that follow |
| 4 | `u32` | `has_base` | 1 if the base shard exists, otherwise 0 |
| 8 | `u64` | `base_file_size` | Size of the base shard file |
| 16 | `u32` | `base_digest` | Digest of the base shard (section 5.4) |
| 20 | `u32` | | Reserved |

It is followed by `shard_count` entries of 24 bytes, sorted by ascending `morton(x, y)`:

| Offset | Type | Field | Description |
|---|---|---|---|
| 0 | `u32` | `x` | Column of the shard root at `shard_level` |
| 4 | `u32` | `y` | Row of the shard root |
| 8 | `u64` | `file_size` | Size of the shard file |
| 16 | `u32` | `digest` | Digest of the shard (section 5.4) |
| 20 | `u32` | | Reserved |

The index lists exactly the shards that exist. A tile at or below the shard level whose shard is not listed does not exist in the pack. This section is typically compressed.

### 4.6 METADATA section

A `u32` entry count, followed by that many entries of two strings each: a key and a value. Keys are lowercase ASCII and may repeat. Version 1.0 defines these keys:

| Key | Repeats | Meaning |
|---|---|---|
| `name` | No | Short name of the pack, such as `Copernicus GLO-30, Norway` |
| `description` | No | Longer description |
| `attribution` | Yes | A credit line that the host must be able to display while the pack's data is visible (FR-08), verbatim, such as `Produced using Copernicus WorldDEM-30 © DLR e.V.` |
| `license` | Yes | License terms of the data, or a URL to them |
| `source` | Yes | A source dataset and its original vertical datum, such as `Copernicus DEM GLO-30 2021_1 (EGM2008)` |
| `generator` | No | Tool and version that wrote the pack, such as `vgl-bake 0.1.0` |
| `created` | No | Creation time in ISO 8601 UTC, such as `2026-10-13T18:00:00Z`. Optional, because it makes otherwise identical bakes differ. |

Unknown keys must be ignored.

---

## 5. Shards

### 5.1 Partitioning

The manifest's `shard_level` *S* splits the pack:

- The **base shard** holds every tile at levels `min_level` to *S* − 1. It exists if any such tile exists.
- Each **level-*S* shard** holds the subtree rooted at one tile (*S*, *x*, *y*): that tile, if it exists, and all its descendants down to `max_level`. A tile (*L*, *x*, *y*) with *L* ≥ *S* belongs to the shard (*x* >> (*L* − *S*), *y* >> (*L* − *S*)).

If *S* ≤ `min_level`, there is no base shard. If *S* > `max_level`, the base shard holds the whole pack and the shard index has no entries. The baker chooses *S* so that shards stay at a manageable size for copying and web hosting; the default is settled by the test bake (design document, section 16, question 7).

### 5.2 File names

The base shard is `base-<digest>.vgls`, and the shard rooted at (*S*, *x*, *y*) is `shards/<y>/<x>-<digest>.vgls`. `<x>` and `<y>` are decimal without leading zeros, and `<digest>` is the shard's digest as eight lowercase hexadecimal digits. Paths are relative to the manifest, using `/` as the separator, for local files and URLs alike.

### 5.3 Shard file layout

A shard is a header, a directory and the tile data, in that order.

| Offset | Type | Field | Description |
|---|---|---|---|
| 0 | `u8[4]` | `magic` | `VGLS` (`56 47 4C 53`) |
| 4 | `u16` | `major_version` | 1 |
| 6 | `u16` | `minor_version` | 0 |
| 8 | `u8` | `layer_type` | Must equal the manifest's `layer_type` |
| 9 | `u8` | `root_level` | *S* for a level-*S* shard; `min_level` for the base shard |
| 10 | `u8` | `flags` | Bit 0: this is the base shard. Other bits reserved. |
| 11 | `u8` | | Reserved |
| 12 | `u32` | `root_x` | Column of the shard root; 0 for the base shard |
| 16 | `u32` | `root_y` | Row of the shard root; 0 for the base shard |
| 20 | `u32` | `tile_count` | Number of directory entries |
| 24 | `u64` | `directory_offset` | Offset of the directory; 64 in version 1.0 |
| 32 | `u32` | `directory_stored_size` | Size of the compressed directory |
| 36 | `u32` | `directory_crc` | CRC-32C of the compressed directory |
| 40 | `u64` | `data_offset` | Offset of the first tile |
| 48 | `u64` | `file_size` | Size of the shard file |
| 56 | `u32` | | Reserved |
| 60 | `u32` | `header_crc` | CRC-32C of bytes 0 to 59 |

The **directory** is Zstandard-compressed. Decompressed, it is `tile_count` entries of 24 bytes, sorted by strictly ascending key:

| Offset | Type | Field | Description |
|---|---|---|---|
| 0 | `u64` | `key` | Tile key (section 2.2) |
| 8 | `u64` | `offset` | Offset of the tile, relative to `data_offset` |
| 16 | `u32` | `size` | Size of the tile in bytes |
| 20 | `u32` | `crc` | CRC-32C of the tile's bytes |

Tiles are stored in directory order, contiguously, with no gaps, from `data_offset` to the end of the file. As a result, the four children of a tile are adjacent and can be fetched with one range request. Every key must lie within the shard: in the subtree of the shard root for a level-*S* shard, or at levels `min_level` to *S* − 1 for the base shard.

### 5.4 Digest

A shard's **digest** is its `header_crc`. Because the header contains the directory CRC, and the directory contains the CRC of every tile, the digest changes whenever any part of the shard changes. The manifest records each shard's digest and file size, and the digest is part of the file name. A reader must check that a shard's `header_crc` and `file_size` match the manifest entry, which detects a stale cache or a shard from a different bake.

### 5.5 Updates (informative)

To update a pack, for example after rebaking a region, upload the new shard files first, then replace `pack.vglm`, then delete shard files that the new manifest no longer references. Readers see either the old or the new pack, never a mixture. Shard files never change once written, so web servers and caches may cache them indefinitely. Only the manifest needs a short cache lifetime.

---

## 6. Tiles

### 6.1 Common tile header

Every tile starts with an 8-byte header:

| Offset | Type | Field | Description |
|---|---|---|---|
| 0 | `u8` | `layer_type` | Must equal the manifest's `layer_type` |
| 1 | `u8` | `encoding` | Payload encoding, per layer type (sections 6.2 and 6.3) |
| 2 | `u8` | `child_mask` | Bit *c* is set if child *c* (section 2.1) exists in this pack. Bits 4 to 7 reserved. |
| 3 | `u8` | `flags` | Per layer type. Unknown bits must be ignored. |
| 4 | `u16` | `header_size` | Size of the complete tile header, including this common part and the layer-specific fields |
| 6 | `u16` | | Reserved |

The payload starts at `header_size`. Later minor versions may append header fields, which older readers skip by using `header_size`.

The child mask lets the runtime decide whether refinement is possible (design document, section 7.3) without reading the children's shard. It must agree with the pack: a set bit means the child tile exists, and a clear bit means it does not.

### 6.2 Elevation and geoid tiles

Elevation tiles use `layer_type` 1, and geoid tiles `layer_type` 3. Both have the layout below; section 6.4 gives the additional rules for geoid tiles. Flag bit 0 (`has_nodata`) is set if any sample is nodata. The header continues after the common part:

| Offset | Type | Field | Description |
|---|---|---|---|
| 8 | `f64` | `offset` | Height offset in metres |
| 16 | `f64` | `scale` | Quantization step in metres |
| 24 | `f32[4]` | `child_min` | Minimum decoded height of each child, in child order |
| 40 | `f32[4]` | `child_max` | Maximum decoded height of each child |

`header_size` is 56 in version 1.0.

**Quantization.** Each sample is a quantized value *q*, an `i16` in [−32767, 32767]. Its height in metres is `offset + scale * q`. The value −32768 means nodata. For every tile:

- `scale` must equal the level's step from the `HEIGHT_STEPS` section.
- `offset` must be an integer multiple of `scale`.

Together, these make the decoded height of a sample depend only on the source height and the level's step, not on the tile, so shared borders (section 2.3) decode identically in neighbouring tiles. If a tile's height range does not fit in 65,534 steps, the baker must fail rather than clip, and report that the level needs a larger step. The baker's default steps are 0.5 m for levels 0 to 5 (range 32,767 m), 0.25 m for levels 6 and 7 (16,383 m) and 0.125 m from level 8 (8,191 m).

**Child bounds.** For each child whose `child_mask` bit is set, `child_min` and `child_max` must bound every decoded, non-nodata height of that child, with `f32` rounding directed outwards. For absent children the values are zero and must be ignored. The runtime uses these bounds to cull children before loading them (design document, section 7.3).

**Encoding 1: MED predictor and Zstandard.** The payload is a Zstandard frame. Its decompressed size is 2 × *N* × *N* bytes. To encode:

1. For each sample in row-major order, compute a prediction *p* from the quantized values to its left (*a*), above (*b*) and above-left (*c*), using the median edge detector of LOCO-I, with arithmetic in at least 32 bits:
   - If *c* ≥ max(*a*, *b*), then *p* = min(*a*, *b*).
   - Otherwise, if *c* ≤ min(*a*, *b*), then *p* = max(*a*, *b*).
   - Otherwise, *p* = *a* + *b* − *c*.

   In row 0, *p* = *a*, and *p* = 0 for the first sample. In column 0 of the other rows, *p* = *b*. Nodata samples take part with their value −32768.
2. Compute the residual *r* = (*q* − *p*) mod 2<sup>16</sup>, interpreted as an `i16`, and zigzag-encode it as a `u16`: *z* = (*r* << 1) XOR (*r* >> 15), with an arithmetic shift.
3. Write the low bytes of all *z* in sample order, followed by the high bytes of all *z* in sample order. Separating the byte planes leaves the high plane mostly zero, which Zstandard compresses well.
4. Compress the result as one Zstandard frame. The compression level is not part of the format.

Decoding reverses these steps, with the addition *q* = (*p* + *r*) mod 2<sup>16</sup> interpreted as an `i16`. The encoding is lossless on the quantized values.

**Partial tiles.** A tile with nodata samples is partial; this happens at the coverage edges of regional packs. A runtime may fill nodata samples from a lower-priority pack that has a tile at the same address. A runtime that does not composite must treat a partial tile as absent, so that rendering falls back to other packs or coarser levels. A tile whose samples are all nodata must not be written.

### 6.3 Imagery tiles

Imagery tiles use `layer_type` 2. The tile header is the common header only (`header_size` 8), and no flags are defined. The payload is an image of (*P* + 2*b*) × (*P* + 2*b*) pixels in the sRGB colour space. Where there is alpha, it is straight (not premultiplied), and alpha 0 means no data.

| Encoding | Payload |
|---|---|
| 1 | JPEG file (baseline or progressive, 8-bit greyscale or YCbCr), for opaque tiles |
| 2 | PNG file (8 bits per channel, greyscale, RGB or RGBA) |
| 3 | WebP file (lossy or lossless, with or without alpha) |
| 4 | BC1 blocks: a Zstandard frame of 4 × 4 blocks in row-major order, 8 bytes per block |
| 5 | BC7 blocks: a Zstandard frame of 4 × 4 blocks in row-major order, 16 bytes per block |

A pack may mix encodings, for example JPEG for opaque tiles and PNG for tiles with alpha. Encodings 4 and 5 contain no mipmaps; the coarser pyramid levels take their place. The default runtime decoders handle encodings 1 and 2 through stb_image and encodings 4 and 5 by direct upload. WebP needs an additional decoder, plugged in through the decoder interface (design document, section 13.1). A runtime without a decoder for a tile's encoding reports `VGL_RESULT_NOT_SUPPORTED` for that tile.

---

### 6.4 Geoid packs

A geoid pack holds the geoid height *N*, the height of the geoid above the ellipsoid, on the sample grid of section 2.3. A host adds at most one geoid pack to a context. With it, heights convert between the ellipsoid and mean sea level:

```
height above mean sea level = ellipsoidal height - N
```

**Coverage.** A geoid pack covers the whole globe at every level from `min_level` to `max_level`, without nodata samples. Packs that do not are invalid.

**Interpolation.** *N* at a position is interpolated bilinearly from the four surrounding samples of the tile at `max_level` that contains the position (section 2.1, as found by the rules for tiles owning their west and north edges). With *s* the tile span and *N*<sub>t</sub> the tile size, the fractional sample coordinates are *u* = (longitude − west) · (*N*<sub>t</sub> − 1) / *s* and *v* = (north − latitude) · (*N*<sub>t</sub> − 1) / *s*, clamped to [0, *N*<sub>t</sub> − 1]. Because neighbouring tiles share their border samples, the result is continuous across tile edges, and does not depend on which tile a position on an edge is assigned to. A reader may interpolate from a coarser level while the deepest level is still loading, and must then report the result as not final (`VGL_RESULT_NOT_READY` in the C API).

**Consistency with the baker.** The baker converts heights referenced to EGM2008, such as Copernicus DEM, to ellipsoidal heights with the same geoid pack and the same interpolation (design document, section 8.3). Converting a baked height back to mean sea level then reproduces the source height, apart from quantization.

**Default bake (informative).** The default geoid pack is baked from the EGM2008 grid with 5′ spacing, resampled by bicubic interpolation to the grid of level 5 with 129-sample tiles (spacing 2.64′). Halving the spacing compared with the source keeps the error of bilinear interpolation at runtime small. Its parameters are `tile_size` 129, `min_level` 0, `max_level` 5, a `shard_level` above `max_level` so that the pack is a single base shard, and a height step of 2<sup>−8</sup> m (3.9 mm) at every level, which covers geoid heights within ±127 m; EGM2008 ranges from about −107 m to +86 m. The pack is expected to be 10 to 20 MB, and its metadata credits the source: `source` `EGM2008 5' grid (NGA)`.

## 7. Versioning

The format version is independent of the library version (design document, section 13.3). Manifests and shards carry it separately, and in version 1.0 they must agree.

- A **major** version change may change anything. A reader must reject a file with an unknown major version.
- A **minor** version change keeps files readable by older readers of the same major version, except where they meet something they cannot interpret. A minor version may:
  - add optional manifest sections,
  - append fields to manifest sections and tile headers, which readers skip by size,
  - define reserved fields or flag bits, provided that zero keeps the previous meaning,
  - add tile encodings and metadata keys.

A writer must set the minor version to the lowest one that defines everything the file uses. A reader must accept every minor version up to its own. For a newer minor version, it reads the file and reports `VGL_RESULT_VERSION_MISMATCH` only when it meets an unknown required section or an unknown encoding.

---

## 8. Validation

A reader must check the following, and must report a failure of any check as `VGL_RESULT_CORRUPT_DATA` unless stated otherwise. It must never read outside a file or decompression buffer because of the contents of a file.

**Manifest**
- The magic, a known major version (otherwise `VGL_RESULT_VERSION_MISMATCH`), `header_crc`, `table_crc`, and that `file_size` matches the file.
- Each section lies within the file, sections do not overlap, each section's `crc` matches, and compressed sections decompress to exactly `size` bytes.
- All required sections for the layer type are present, and no unknown required section is present (otherwise `VGL_RESULT_VERSION_MISMATCH`).
- `LAYER` values are within the ranges of section 4.3, with `min_level` ≤ `max_level`, and `HEIGHT_STEPS` has one power-of-two step per level.
- Shard index entries are strictly ascending, and their coordinates are valid at `shard_level`.

**Shards**
- The magic, the version, `header_crc`, and that `header_crc` and `file_size` match the manifest entry.
- `layer_type`, `root_level`, `root_x`, `root_y` and the base flag match the shard's place in the pack.
- The directory lies within the file, matches `directory_crc`, and decompresses to exactly 24 × `tile_count` bytes.
- Keys are strictly ascending and lie within the shard (section 5.3). Tiles are contiguous from `data_offset` to `file_size`.

**Tiles**
- The tile's `crc` matches the directory entry.
- `layer_type` matches, `header_size` is at least the size defined in version 1.0 and no larger than the tile, and the encoding is known (otherwise `VGL_RESULT_NOT_SUPPORTED`).
- For elevation and geoid tiles, `scale` equals the level's step and `offset` is a multiple of it. The payload decompresses to exactly 2 × *N* × *N* bytes, and decoded values are in [−32767, 32767] or −32768. Geoid tiles contain no nodata.
- For imagery, the decoded image has the expected dimensions.

Where a child's tile is available in the same shard, a reader in a debug build should also check that it agrees with its parent's `child_mask` and child bounds.

---

## 9. Parameters to settle by test bake

These values are parameters recorded in each manifest, not constants of the format. Their defaults will be chosen after the regional test bake (design document, section 14, milestone M1):

| Parameter | Field | Provisional default |
|---|---|---|
| Elevation tile size | `tile_size` | 129 |
| Imagery tile size | `tile_size` | 256 |
| Deepest elevation level | `max_level` | 12 (≈38 m) or 13 (≈19 m) |
| Shard level | `shard_level` | 6 |
| Height steps | `HEIGHT_STEPS` | 0.5 m, 0.25 m and 0.125 m, as in section 6.2 |
| Imagery border | `border` | 0 or 1 |

---

## Appendix A: Decisions and alternatives

| Decision | Alternative considered | Reason |
|---|---|---|
| One layer per pack | Several layers per pack | Packs stack by priority per layer (FR-05), so one layer per pack keeps stacking and updates simple. A dataset with elevation and imagery ships as two packs. |
| Geoid as a pack with its own layer type | A geoid grid file in another format, such as GeographicLib's PGM files, or a grid built into the library | The geoid reuses the providers, caching, budgets and streaming of terrain, and only the tiles in use are resident. A separate layer type keeps it out of terrain stacking. The cost is that height queries above mean sea level are unavailable until the pack has loaded. |
| Binary manifest | JSON manifest | A JSON manifest is easier to read, but would add a JSON parser to the runtime (NFR-07). The baker's inspect command can print the manifest as text instead. |
| Rows counted from the north | Rows from the south, as in TMS | Matches image row order, so tile rows and pixel rows run the same way. |
| Same scale for every tile of a level, offsets aligned to it | Free scale and offset per tile | Shared borders must decode identically to avoid seams. Free per-tile scales would give cracks of up to half a step between neighbours. |
| Morton-ordered keys | Row-major order | Siblings and subtrees are contiguous, so a parent's four children can be read with one range request. |
| Directory at the start of the shard | Directory at the end | A reader gets the header and directory in one speculative range request. The baker finishes a shard before writing it, which it does anyway, because bake units are shards. |
| Content digest in shard file names | Fixed names | Makes updates atomic, and lets web servers and caches keep shards indefinitely. |
| CRC-32C | A 64-bit hash such as XXH64 | Easy to implement without a dependency, and sufficient for detecting corruption and changed shards. |
| MED predictor, byte planes and Zstandard | Plain Zstandard, or a lossy mesh encoding | Lossless on the quantized values and simple to implement. The byte planes help Zstandard with the mostly zero high bytes of small residuals. |
| Nodata value and partial tiles | Only complete tiles in regional packs | Regional packs can then reach the exact edge of their coverage. A simple runtime can still treat partial tiles as absent. |
