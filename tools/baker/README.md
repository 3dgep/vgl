# vgl-bake

`vgl-bake` turns source elevation data into vgl tile packs ([pack format](../../docs/pack-format.md)), and checks packs. It is the only part of vgl that uses GDAL (design document, section 8.2).

## Building

Configure with `-DVGL_BUILD_BAKER=ON`. The baker needs GDAL 3.4 or later; nlohmann/json is used if installed and downloaded otherwise.

**Linux.** Install the packages, then use the baker preset:

```sh
sudo apt-get install libgdal-dev nlohmann-json3-dev libzstd-dev ninja-build
cmake --workflow --preset linux-gcc-baker-release
```

**Windows.** GDAL comes from conda-forge, through [micromamba](https://mamba.readthedocs.io/en/latest/installation/micromamba-installation.html). Create the environment from `environment.yml` once, activate it so that `CONDA_PREFIX` is set, and use the baker preset:

```powershell
micromamba create -p D:\vgl-env -f tools\baker\environment.yml
micromamba activate D:\vgl-env
cmake --workflow --preset windows-msvc-baker-release
```

To run `vgl-bake.exe` outside the test preset, the environment must be active, so that GDAL's DLLs and the PROJ database are found.

## Commands

```
vgl-bake bake <config.json> [--jobs N] [--resume] [--max-level L] [--cache MB] [--verbose]
vgl-bake inspect <pack>
vgl-bake validate <pack>
vgl-bake query <pack> < points.txt
```

- **bake** builds the pack a configuration describes. Work is split by shard and runs on all cores. Finished shards are recorded in `<output>/.bake`, so an interrupted bake continues with `--resume`. `--max-level` makes a quick preview with fewer levels. Bakes are deterministic: the same sources and configuration give byte-identical packs, whatever the number of threads.
- **inspect** prints the manifest, the tile count and size of every level, and the shard counts and sizes that every other shard level would give.
- **validate** reads and decodes every tile, which checks every CRC and structure, and then checks that shared borders are identical (across the antimeridian too), that pole rows are uniform, and that child masks and child height bounds agree with the tiles. It exits with 1 if any check fails.
- **query** reads `latitude longitude` lines in degrees and prints the height at each from the deepest tile, with the level it came from.

## Configuration

A configuration is a JSON file; comments are allowed. Relative paths are relative to the configuration file. See [examples](examples) for the packs of the M1 test bake.

| Key | Default | Meaning |
|---|---|---|
| `output` | required | Directory the pack is written to |
| `layer` | `"elevation"` | `"elevation"` or `"geoid"` |
| `tile_size` | `129` | Samples per tile side, 2^k + 1 |
| `min_level`, `max_level` | `0`, `12` | Range of levels in the pack |
| `shard_level` | `max_level - 5` | Level of the shard roots; the default gives shards of about 10 to 25 MB |
| `region` | the globe | `[west, south, east, north]` in degrees: only tiles that intersect it are baked |
| `filter` | `"average"` | How coarser levels are filtered: `"average"`, or `"maximum"` so that terrain is never lower than it is |
| `height_steps` | see below | Quantization step per level from `min_level`, powers of two |
| `zstd_level` | `19` | Zstandard compression level |
| `geoid` | | Geoid pack, required for sources referenced to EGM2008 |
| `metadata` | | Object of strings or lists of strings: `name`, `attribution`, `license`, `source`, ... |
| `sources` | required | Sources, highest priority first |

Each source has:

| Key | Default | Meaning |
|---|---|---|
| `name` | `source N` | Name used in messages |
| `files` | required | A path or a list of paths; the file name may contain `*` and `?` |
| `datum` | `"ellipsoid"` | `"ellipsoid"`, or `"egm2008"` for heights above mean sea level (Copernicus DEM, GEBCO) |
| `max_level` | the pack's | Deepest level this source contributes to |
| `resampling` | `"bilinear"` | GDAL resampling near the source's resolution, such as `"cubic"` |

Sources must be in geographic WGS84 coordinates (such as EPSG:4326); reproject others with `gdalwarp` first. Where a source has no data, the next source fills in.

The default height steps are 0.5 m to level 5, 0.25 m to level 9 and 0.125 m beyond, and 2^-8 m for geoid packs. A tile whose heights do not fit in 65,534 steps fails the bake with a message that names it, rather than being clipped.

## How tiles are computed

Every tile is resampled from its sources directly onto its own sample grid, through GDAL's warper with an exact transform and a fixed kernel scale, so that each sample's value depends only on its position. Neighbouring tiles, even in different shards, therefore compute identical values for their shared borders, and shards bake independently. Samples that represent the same point, on the antimeridian and at the poles, are taken at one position (longitude -180, and longitude 0 at the poles), so they are identical too. Near a source's resolution the source is interpolated; at levels coarser than 1.5 times its pixel size it is filtered by area averaging (or the maximum). Heights referenced to EGM2008 are converted to ellipsoidal heights with the geoid pack, using the same interpolation as the runtime.

## Sources of the M1 test bake

Put the data under one directory (here `~/vgl-data`), with the configurations from `examples` next to it:

```
~/vgl-data/
    egm2008-geoid.json  gebco-global.json  glo30-norway.json
    sources/geoid/us_nga_egm08_25.tif
    sources/gebco-cog/gebco_2026_*.tif
    sources/glo30/Copernicus_DSM_COG_10_*.tif
```

- **EGM2008, 2.5′** (80 MB, public domain): `https://cdn.proj.org/us_nga_egm08_25.tif`.
- **GEBCO 2026**, ice surface elevation, GeoTIFF (4.2 GB zip): from [gebco.net](https://www.gebco.net/data_and_products/gridded_bathymetry_data/). Its GeoTIFFs are stored in strips without overviews, so convert them once to tiled COGs with average overviews, which makes the coarse levels fast:

  ```sh
  gdal_translate -of COG -co COMPRESS=ZSTD -co PREDICTOR=2 -co BLOCKSIZE=512 \
      -co OVERVIEW_RESAMPLING=AVERAGE gebco_2026_n90.0_s0.0_w0.0_e90.0_geotiff.tif \
      sources/gebco-cog/gebco_2026_n90.0_s0.0_w0.0_e90.0_geotiff.tif
  ```

- **Copernicus DEM GLO-30** (337 tiles over 57 to 72 N, 4 to 32 E, 5.4 GB): from the AWS open data mirror, `https://copernicus-dem-30m.s3.amazonaws.com/<tile>/<tile>.tif`, with the tile names in `https://copernicus-dem-30m.s3.amazonaws.com/tileList.txt`.

Bake the geoid pack first, because the other two use it:

```sh
cd ~/vgl-data
vgl-bake bake egm2008-geoid.json && vgl-bake validate packs/egm2008-geoid
vgl-bake bake gebco-global.json && vgl-bake validate packs/gebco-global
vgl-bake bake glo30-norway.json && vgl-bake validate packs/glo30-norway
```
