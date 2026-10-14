# Test Bake Report (M0, step 4)

| | |
|---|---|
| **Date** | 14 October 2026 |
| **Baker** | `vgl-bake` 0.1.0, pack format 1.0 |
| **Machine** | 32 logical CPUs, 31 GB RAM available to WSL 2 (Ubuntu 24.04), GDAL 3.8.4 |
| **Purpose** | Settle tile size, deepest level and shard level (design document, section 16, question 7), and check the baker and pack format on real data |

## 1. Summary

All four recommendations were accepted, with level 8 for GEBCO. The baker's defaults, the example configurations, the pack format (section 9) and the design document (sections 8.5, 10.1 and 16) follow them.

| Parameter | Decision | Main reason |
|---|---|---|
| Tile size | **129** samples (keep) | 65 costs 5 to 9% more space and four times the tiles; 257 saves only 3 to 5% and coarsens level-of-detail steps |
| Deepest level, Copernicus GLO-30 | **12** (≈38 m) | Level 13 nearly triples the size (×2.9) while mostly oversampling the source |
| Deepest level, GEBCO base | **8** (≈611 m), or 9 if bathymetry is seen up close | Level 9 is 68% of the pack and oversamples GEBCO's 15″ grid |
| Shard level | **`max_level − 5`** per pack, instead of a fixed 6 | Gives shards of about 10 to 25 MB for every dataset tried, and directories that fit in one 64 KiB read |

Size estimates from the design document hold: GLO-30 at level 12 extrapolates to about 140 GB globally (design: 100 to 150 GB), and level 13 to about 410 GB (design: several hundred GB).

All packs validated with no seam, pole, child-mask or child-bound errors. The bake found eight defects in the baker and the defaults, all fixed (section 6).

## 2. Packs baked

| Pack | Source | Levels | Tiles | Size | Bake time |
|---|---|---|---|---|---|
| EGM2008 geoid | NGA EGM2008 2.5′ grid | 0–5 | 2,730 | 21.8 MB | 12 s |
| GEBCO global | GEBCO 2026, 15″ | 0–9 | 699,050 | 6,793 MB | 237 s |
| GLO-30 Norway | Copernicus DEM GLO-30, 337 tiles over 57–72° N, 4–32° E | 6–12 | 234,778 | 1,794 MB | 56 s |
| GLO-30 Norway | same | 6–13 | 934,408 | 5,276 MB | 226 s |
| **GEBCO global, final** | GEBCO 2026, 15″ | 0–8 | 174,762 | 2,166 MB | 60 s |
| **GLO-30 Norway, final** | as above | 6–12 | 234,778 | 1,777 MB | 60 s |

The measurement packs use 129-sample tiles, shard level 6 and the default height steps (0.5 m to level 5, 0.25 m to level 9, 0.125 m beyond). The two final packs use the accepted defaults, with shard levels 3 (GEBCO: 128 shards, median 18.4 MB, largest 24.7 MB) and 7 (Norway: 205 shards, median 10.4 MB, largest 18.7 MB), and were baked after all fixes; GEBCO has no partial tiles at any level. Elevation is referenced to the WGS84 ellipsoid through the geoid pack. Bake times are wall-clock times with 32 threads, excluding validation.

**Geoid accuracy.** Compared with GeographicLib's `GeoidEval` on the same official 2.5′ grid at 20,005 random points, the geoid pack differs by 3.3 mm RMS and at most 5.2 cm (near Mount Everest, where the geoid is roughest). This covers the whole chain: resampling, quantization, encoding, decoding and runtime interpolation.

**Throughput.** 3,000 to 4,200 tiles per second. A global GLO-30 bake at level 12 (about 18 million tiles) would take a bit over an hour on this machine, and level 13 about five hours, so a global bake fits on one workstation.

## 3. Tile size

The three tile sizes were compared on the same fjord region (5.625–8.4375° E, 60.47–61.875° N, around the Sognefjord), at the same ground resolution:

| Tile size | Level | Spacing | Tiles at deepest level | Total size | vs. 129 | Bake time |
|---|---|---|---|---|---|---|
| 65 | 13 | 38 m | 8,192 | 38.7 MB | +5.2% | 15 s |
| **129** | **12** | **38 m** | **2,048** | **36.8 MB** | | **12 s** |
| 257 | 11 | 38 m | 512 | 35.5 MB | −3.5% | 19 s |
| 65 | 14 | 19 m | 32,768 | 116.0 MB | +9.0% | 41 s |
| **129** | **13** | **19 m** | **8,192** | **106.4 MB** | | **40 s** |
| 257 | 12 | 19 m | 2,048 | 101.3 MB | −4.8% | 49 s |

Larger tiles compress a little better, because the predictor and Zstandard have more context and the fixed per-tile cost (56-byte header, 24-byte directory entry, frame overhead) is spread over more samples. The differences are small.

The runtime side decides the matter:

- **65** gives the finest level-of-detail steps, but four times as many tiles, draw calls, cache entries and requests for the same area.
- **257** cuts the tile count by four, but each refinement step quadruples an area that must load at once, so the renderer draws more detail than needed near the edges of the view and uploads larger blocks (132 KB of samples per tile).
- **129** sits between the two, at about 33 KB of samples and 8 to 12 KB on disk per tile.

**Recommendation:** keep 129.

## 4. Deepest level

### Copernicus GLO-30

| Level | Spacing (latitude) | Tiles | Size of level | Bytes per tile | Bits per sample |
|---|---|---|---|---|---|
| 10 | 153 m | 11,182 | 113 MB | 10,111 | 4.86 |
| 11 | 76 m | 44,280 | 397 MB | 8,958 | 4.31 |
| 12 | 38 m | 175,475 | 1,244 MB | 7,089 | 3.41 |
| 13 | 19 m | 699,630 | 3,475 MB | 4,967 | 2.39 |

Level 13 makes the Norway pack 2.9 times larger (5,276 MB against 1,794 MB). GLO-30 has a latitude spacing of 1″ (31 m), so level 12 (38 m) is slightly coarser than the source and level 13 (19 m) oversamples it by a factor of 1.6 in latitude. In longitude the geographic scheme already oversamples at these latitudes: at 60° N level 12 has 19 m spacing against GLO-30's 31 m, and level 13 has 9.5 m. The falling bits per sample at level 13 show the same thing: much of the extra data is interpolation.

| Deepest level | Norway | Global estimate |
|---|---|---|
| 12 | 1.8 GB | ≈140 GB |
| 13 | 5.3 GB | ≈410 GB |

The global estimates scale Norway by the number of GLO-30 source tiles (26,450 worldwide against 337 here). Norway is mountainous and has much coastline, so they are rough, but they agree with the design document's estimates.

**Recommendation:** level 12 for GLO-30. Level 13 stays possible per pack, for regions with finer sources.

### GEBCO

| Level | Spacing | Tiles | Size of level |
|---|---|---|---|
| 7 | 1.2 km | 32,768 | 445 MB |
| 8 | 611 m | 131,072 | 1,519 MB |
| 9 | 305 m | 524,288 | 4,644 MB |

GEBCO's 15″ grid has 463 m spacing. Level 9 oversamples it by 1.5 and makes up 68% of the pack; level 8 is slightly coarser than the source. Ending at level 8 gives a 2.1 GB global base (the design estimated under 2 GB), and level 9 gives 6.8 GB.

Over land, GLO-30 packs take over wherever they have tiles, so the choice matters mainly for oceans, which ties it to open question 5 (bathymetry or a flat sea).

**Recommendation:** level 8 for a flat sea or bathymetry seen from a distance; level 9 if bathymetry is seen up close.

## 5. Shard level

The baker's `inspect` command reports, for each possible shard level, how the same tiles would be grouped:

| Pack | Shard level | Shards | Median size | Largest | Base shard |
|---|---|---|---|---|---|
| GLO-30 Norway, level 12 | 6 | 58 | 32.9 MB | 71.0 MB | none |
| | **7** | **205** | **10.5 MB** | **18.8 MB** | 0.6 MB |
| | 8 | 731 | 3.1 MB | 5.0 MB | 2.8 MB |
| GLO-30 Norway, level 13 | 6 | 58 | 95.0 MB | 208.8 MB | none |
| | 7 | 205 | 32.2 MB | 54.9 MB | 0.6 MB |
| | **8** | **731** | **9.0 MB** | **15.4 MB** | 2.8 MB |
| GEBCO, level 9 | 3 | 128 | 57.6 MB | 84.4 MB | 0.9 MB |
| | **4** | **512** | **14.3 MB** | **24.7 MB** | 3.4 MB |
| | 6 | 8,192 | 0.9 MB | 1.8 MB | 45.8 MB |

A fixed shard level does not suit packs of different depths: level 6 gives 33 MB shards for GLO-30 at level 12, 95 MB at level 13, and 0.9 MB for GEBCO. The depth of a shard's subtree, `max_level − shard_level`, is what fixes its size. A depth of 5 gives shards of 9 to 25 MB in all three packs, with at most 1,365 tiles per shard, so the compressed directory (24 bytes per tile, before compression) fits in the first 64 KiB read that section 1.1 of the pack format expects.

Smaller shards also make regional updates and partial downloads finer, at the cost of more files. A world at level 12 with depth 5 has roughly 16,000 GLO-30 shards (205 for Norway, scaled as in section 4), which object storage and file systems handle easily.

**Recommendation:** default the baker's shard level to `max_level − 5`, and keep it overridable per pack. This needs no format change, because the shard level is already a field of each manifest.

## 6. Problems found and fixed during the bake

The test bake found five defects that unit tests had not, all through the `validate` command:

1. **Height steps too fine at level 8.** Tiles around Hawaii span more than 8.5 km from the ocean floor to the summits, beyond the 8,191 m that 0.125 m steps allow. The baker failed as designed rather than clipping. The default is now 0.25 m up to level 9 (pack format, section 6.2).
2. **Ties rounded differently in neighbouring tiles.** Heights exactly halfway between two steps rounded one way relative to one tile's offset and the other way relative to its neighbour's. Writers now quantize on the level's global grid of steps, which the pack format now requires (section 6.2).
3. **GDAL's warper is not position-deterministic.** With default settings, interpolation kernels and area averages depended on the shape of the region being warped, so neighbouring tiles disagreed on their shared borders, by up to 742 m in one case. Interpolation now fixes the kernel scale (`XSCALE`, `YSCALE`), and area filters are computed by the baker itself.
4. **Level-0 tiles touch both poles.** Only the north pole row was made uniform.
5. **Pole rows at interpolated levels had no data**, because kernels reach beyond ±90°. The pole value now falls back to the area average.

Three more created holes (partial tiles), but no seams, and did not change the conclusions above. They were fixed after the measurements, with regression tests that fail without the fixes:

6. **Holes at GLO-30 level 11 above 70° N** (3,923 partial tiles, against 1,094 at the region's edges). GLO-30 pixels are 3″ wide in longitude there, wider than level 11's footprint, but the baker switched to area filtering based on the finer pixel dimension. It now decides on the coarser one.
7. **Pole rows at GEBCO level 9 were empty.** The pole fallback's footprint was narrower than one GEBCO pixel. It is now at least two source pixels wide.
8. **Area averages read overviews too coarse for their footprint.** An overview whose pixels are as wide as the footprint can leave a footprint without pixel centres, as happened at GEBCO's north pole at level 8, whose origin lies a hair below 90°. Averages now read an overview with at least two pixels across each footprint.

## 7. Follow-up

- Bake a region with steep terrain and a long coastline outside Norway, such as the Alps or Hawaii with GLO-30, to check the global extrapolation.
