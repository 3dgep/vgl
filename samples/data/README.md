# Sample data

Small packs that the samples and their tests use, so that they run offline and without the large datasets (design document, section 11).

| Pack | Contents | Levels | Size |
|---|---|---|---|
| `packs/gebco` | GEBCO 2026 grid: land and ocean floor, as ellipsoidal heights | 0 to 3 (about 20 km between samples) | 3.4 MB |
| `packs/egm2008` | EGM2008 geoid, for heights above mean sea level | 0 to 2 (about 0.7° between samples) | 0.7 MB |
| `packs/glo30-sognefjord` | Copernicus DEM GLO-30 over the Sognefjord, Jostedalsbreen and the western Jotunheimen (5.625 to 8.4375 E, 60.46875 to 61.875 N: two level-7 tiles), as ellipsoidal heights, above GEBCO in the samples | 7 to 11 (about 76 m between samples) | 10.9 MB |
| `packs/bluemarble` | NASA Blue Marble: Next Generation, July, with topography and bathymetry: 256-pixel JPEG tiles with a 1-pixel border | 0 to 3 (about 10 km per pixel) | 1.6 MB |
| `clouds.jpg` | NASA Blue Marble cloud composite, 2002: the clouds' coverage of the whole globe in latitude and longitude, grey, for the backends' cloud shell. Its ice sheets over Antarctica and Greenland show as cloud | 4096 x 2048 (about 10 km per pixel) | 2.7 MB |
| `packs/s2cloudless-sognefjord` | Sentinel-2 cloudless 2016 by EOX over the region of `packs/glo30-sognefjord`, above Blue Marble in the samples: 256-pixel JPEG tiles with a 1-pixel border | 7 to 11 (about 38 m per pixel north to south) | 12.5 MB |

The Sentinel-2 pack shows a flaw of EOX's 2016 mosaic as it is: over Jostedalsbreen and other glaciers and snowfields, which the mosaic's processing took for cloud, it holds a flat near-black fill (RGB 11, 18, 26) instead of imagery (design document, section 9).

The GLO-30 sample is baked without `sea_blend` (`tools/baker/README.md`): it borders the GEBCO sample, which at about 20 km between samples is far coarser than the full GEBCO grid, so its edges show the change of resolution rather than a step at sea, and blending would only pull its inner fjords down to GEBCO's depth.

Each pack is one base shard. They are far too coarse for anything but samples and tests: the full datasets are described in `tools/baker/README.md` and `docs/test-bake.md`.

## Attribution

- **GEBCO**: public domain; the source must be acknowledged. "GEBCO Compilation Group (2026) GEBCO 2026 Grid".
- **EGM2008**: "EGM2008 geoid model, National Geospatial-Intelligence Agency (NGA)", as distributed by PROJ (`us_nga_egm08_25.tif`).
- **Copernicus GLO-30**: free to use with attribution: "Produced using Copernicus WorldDEM-30 © DLR e.V. 2010-2014 and © Airbus Defence and Space GmbH 2014-2018 provided under COPERNICUS by the European Union and ESA; all rights reserved".
- **Blue Marble**: NASA imagery, free of copyright restrictions; NASA asks for credit. "NASA Earth Observatory, Blue Marble: Next Generation (Reto Stöckli)".
- **Blue Marble clouds**: NASA imagery, free of copyright restrictions; NASA asks for credit. "NASA Earth Observatory, Blue Marble: Clouds (Reto Stöckli)".
- **Sentinel-2 cloudless 2016**: CC BY 4.0. "Sentinel-2 cloudless by EOX IT Services GmbH (Contains modified Copernicus Sentinel data 2016)". Only the 2016 mosaic is CC BY; the later years are CC BY-NC-SA.

The packs carry these strings in their metadata, which the runtime reports as attribution.

## Baking them again

The configurations are `gebco-sample.json`, `egm2008-sample.json`, `glo30-sample.json`, `bluemarble-sample.json` and `s2cloudless-sample.json`. Bake them with `vgl-bake bake <configuration>` from a directory that holds the sources, as for the examples in `tools/baker/examples`: `sources/gebco-cog` (GEBCO prepared as described in `tools/baker/README.md`), `sources/geoid/us_nga_egm08_25.tif`, the GLO-30 tiles in `sources/glo30` (as for `tools/baker/examples/glo30-norway.json`), the full geoid pack `packs/egm2008-geoid`, which the GEBCO and GLO-30 bakes use for their datum conversion, and `sources/bluemarble/world.topo.bathy.200407.3x5400x2700.tif`. That last one is NASA's 5400 × 2700 PNG ([Visible Earth, record 73751](https://visibleearth.nasa.gov/images/73751), SHA-256 `8be4641b09f289e25e0b62642c395e48695e66a4ac4ff1753df21c3b2e437601`), georeferenced with `gdal_translate -a_srs EPSG:4326 -a_ullr -180 90 180 -90`. The Sentinel-2 source, `sources/s2cloudless/s2cloudless-2016-sognefjord.tif`, is EOX's WMTS layer `s2cloudless` (the 2016 mosaic) at zoom 11 of its `WGS84` tile matrix set, which is the pack tiling's level 11, over the region with a margin of 256 pixels, so that the tiles at its edges have their borders: with a GDAL WMTS description `s2cloudless.xml` of `<GDAL_WMTS><GetCapabilitiesUrl>https://tiles.maps.eox.at/wmts/1.0.0/WMTSCapabilities.xml</GetCapabilitiesUrl><Layer>s2cloudless</Layer><TileMatrixSet>WGS84</TileMatrixSet><ZoomLevel>11</ZoomLevel><BandsCount>3</BandsCount></GDAL_WMTS>`, it is `gdal_translate -projwin 5.537109375 61.962890625 8.525390625 60.380859375 -co COMPRESS=DEFLATE -co TILED=YES -co PREDICTOR=2 s2cloudless.xml s2cloudless-2016-sognefjord.tif`, which reads 612 tiles of EOX's service. `clouds.jpg` is the first band of NASA's 8192 x 4096 `cloud_combined_8192.tif` (Visible Earth record 57747, `https://eoimages.gsfc.nasa.gov/images/imagerecords/57000/57747/cloud_combined_8192.tif`, SHA-256 `d137775d8966ab8d443fd5126dc6e7ad72072bc1ed50555c5818d221735daf0f`), halved: `gdal_translate -b 1 -outsize 4096 2048 -r average -of JPEG -co QUALITY=90 cloud_combined_8192.tif clouds.jpg`. Then copy `pack.vglm` and the `base-*.vgls` file of each output into `packs/`. Bakes are deterministic, so the same sources give byte-identical packs; for imagery, that holds for the same JPEG and PNG encoders, which come with GDAL.
