# Sample data

Small packs that the samples and their tests use, so that they run offline and without the large datasets (design document, section 11).

| Pack | Contents | Levels | Size |
|---|---|---|---|
| `packs/gebco` | GEBCO 2026 grid: land and ocean floor, as ellipsoidal heights | 0 to 3 (about 20 km between samples) | 3.4 MB |
| `packs/egm2008` | EGM2008 geoid, for heights above mean sea level | 0 to 2 (about 0.7° between samples) | 0.7 MB |
| `packs/bluemarble` | NASA Blue Marble: Next Generation, July, with topography and bathymetry: 256-pixel JPEG tiles with a 1-pixel border | 0 to 3 (about 10 km per pixel) | 1.6 MB |

Each pack is one base shard. They are far too coarse for anything but samples and tests: the full datasets are described in `tools/baker/README.md` and `docs/test-bake.md`.

## Attribution

- **GEBCO**: public domain; the source must be acknowledged. "GEBCO Compilation Group (2026) GEBCO 2026 Grid".
- **EGM2008**: "EGM2008 geoid model, National Geospatial-Intelligence Agency (NGA)", as distributed by PROJ (`us_nga_egm08_25.tif`).
- **Blue Marble**: NASA imagery, free of copyright restrictions; NASA asks for credit. "NASA Earth Observatory, Blue Marble: Next Generation (Reto Stöckli)".

The packs carry these strings in their metadata, which the runtime reports as attribution.

## Baking them again

The configurations are `gebco-sample.json`, `egm2008-sample.json` and `bluemarble-sample.json`. Bake them with `vgl-bake bake <configuration>` from a directory that holds the sources, as for the examples in `tools/baker/examples`: `sources/gebco-cog` (GEBCO prepared as described in `tools/baker/README.md`), `sources/geoid/us_nga_egm08_25.tif`, the full geoid pack `packs/egm2008-geoid`, which the GEBCO bake uses for its datum conversion, and `sources/bluemarble/world.topo.bathy.200407.3x5400x2700.tif`. That last one is NASA's 5400 × 2700 PNG ([Visible Earth, record 73751](https://visibleearth.nasa.gov/images/73751), SHA-256 `8be4641b09f289e25e0b62642c395e48695e66a4ac4ff1753df21c3b2e437601`), georeferenced with `gdal_translate -a_srs EPSG:4326 -a_ullr -180 90 180 -90`. Then copy `pack.vglm` and the `base-*.vgls` file of each output into `packs/`. Bakes are deterministic, so the same sources give byte-identical packs; for imagery, that holds for the same JPEG and PNG encoders, which come with GDAL.
