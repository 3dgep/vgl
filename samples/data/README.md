# Sample data

Small packs that the samples and their tests use, so that they run offline and without the large datasets (design document, section 11).

| Pack | Contents | Levels | Size |
|---|---|---|---|
| `packs/gebco` | GEBCO 2026 grid: land and ocean floor, as ellipsoidal heights | 0 to 3 (about 20 km between samples) | 3.4 MB |
| `packs/egm2008` | EGM2008 geoid, for heights above mean sea level | 0 to 2 (about 0.7° between samples) | 0.7 MB |

Each pack is one base shard. They are far too coarse for anything but samples and tests: the full datasets are described in `tools/baker/README.md` and `docs/test-bake.md`.

## Attribution

- **GEBCO**: public domain; the source must be acknowledged. "GEBCO Compilation Group (2026) GEBCO 2026 Grid".
- **EGM2008**: "EGM2008 geoid model, National Geospatial-Intelligence Agency (NGA)", as distributed by PROJ (`us_nga_egm08_25.tif`).

The packs carry these strings in their metadata, which the runtime reports as attribution.

## Baking them again

The configurations are `gebco-sample.json` and `egm2008-sample.json`. Bake them with `vgl-bake bake <configuration>` from a directory that holds the sources, as for the examples in `tools/baker/examples`: `sources/gebco-cog` (GEBCO prepared as described in `tools/baker/README.md`), `sources/geoid/us_nga_egm08_25.tif`, and the full geoid pack `packs/egm2008-geoid`, which the GEBCO bake uses for its datum conversion. Then copy `pack.vglm` and the `base-*.vgls` file of each output into `packs/`. Bakes are deterministic, so the same sources give byte-identical packs.
