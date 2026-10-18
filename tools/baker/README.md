# vgl-bake

`vgl-bake` turns source elevation data into vgl tile packs ([pack format](../../docs/pack-format.md)), and checks packs. It is the only part of vgl that uses GDAL (design document, section 8.2).

## Building

Configure with `-DVGL_BUILD_BAKER=ON`. The baker needs GDAL 3.7 or later (for warping one band of a multi-band source); nlohmann/json is used if installed and downloaded otherwise. Its `check` command needs libcurl, which it shares with the defaults module's HTTP reader: the baker presets turn `VGL_DEFAULTS_HTTP` on, and Linux needs `libcurl4-openssl-dev` for it.

**Linux.** Install the packages, then use the baker preset:

```sh
sudo apt-get install libgdal-dev nlohmann-json3-dev libzstd-dev libcurl4-openssl-dev ninja-build
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
vgl-bake check <url> [--sample N] [--timeout S]
```

- **bake** builds the pack a configuration describes. Work is split by shard and runs on all cores. Finished shards are recorded in `<output>/.bake`, so an interrupted bake continues with `--resume`. `--max-level` makes a quick preview with fewer levels. Bakes are deterministic: the same sources and configuration give byte-identical packs, whatever the number of threads.
- **inspect** prints the manifest, the tile count and size of every level, and the shard counts and sizes that every other shard level would give.
- **validate** reads and decodes every tile, which checks every CRC and structure. For heights, it then checks that shared borders are identical (across the antimeridian too), that pole rows are uniform, and that child masks and child height bounds agree with the tiles. For imagery, it checks that each image has the tile's size with its border, that borders repeat the neighbouring tiles' pixels (to within a mean difference of 12 per channel, which JPEG's losses stay well inside), and that child masks agree. It exits with 1 if any check fails.
- **query** reads `latitude longitude` lines in degrees and prints the height at each from the deepest tile, with the level it came from.
- **check** checks a pack on a web server, by the URL of its directory, as vgl and browsers read it ([hosting guide](../../docs/hosting.md#checking-a-host)): the manifest, byte ranges, compression, CORS and caching headers, every shard's size and digest (or `--sample N` of them, spread over the pack), and one shard's directory and a tile. Errors are what stops vgl reading the pack, or makes every read download a whole shard; warnings are what stops browsers on other sites, or caches, from doing their part. It exits with 1 if there is an error.

## Configuration

A configuration is a JSON file; comments are allowed. Relative paths are relative to the configuration file. See [examples](examples) for the packs of the M1 test bake.

| Key | Default | Meaning |
|---|---|---|
| `output` | required | Directory the pack is written to |
| `layer` | `"elevation"` | `"elevation"`, `"imagery"` or `"geoid"` |
| `tile_size` | `129`; imagery `256` | Heights: samples per tile side, 2^k + 1. Imagery: pixels per side, a power of two from 64 to 2048 |
| `border` | imagery `1` | Imagery only: pixels repeated from the neighbouring tiles around each tile, 0 or 1 |
| `encoding` | `"auto"` | Imagery only: `"auto"` (JPEG for tiles with data everywhere, PNG with alpha for the others), `"jpeg"` or `"png"` |
| `jpeg_quality` | `85` | Imagery only: JPEG quality from 1 to 100 |
| `min_level`, `max_level` | `0`, `12` | Range of levels in the pack |
| `shard_level` | `max_level - 5` | Level of the shard roots; the default gives shards of about 10 to 25 MB |
| `region` | the globe | `[west, south, east, north]` in degrees: only tiles that intersect it are baked |
| `filter` | `"average"` | Heights only: how coarser levels are filtered: `"average"`, or `"maximum"` so that terrain is never lower than it is |
| `height_steps` | see below | Heights only: quantization step per level from `min_level`, powers of two |
| `zstd_level` | `19` | Zstandard compression level |
| `geoid` | | Geoid pack, required for sources referenced to EGM2008 |
| `metadata` | | Object of strings or lists of strings: `name`, `attribution`, `license`, `source`, ... |
| `sources` | required | Sources, highest priority first |

Each source has:

| Key | Default | Meaning |
|---|---|---|
| `name` | `source N` | Name used in messages |
| `files` | required | A path or a list of paths; the file name may contain `*` and `?` |
| `datum` | `"ellipsoid"` | Heights only: `"ellipsoid"`, or `"egm2008"` for heights above mean sea level (Copernicus DEM, GEBCO) |
| `max_level` | the pack's | Deepest level this source contributes to |
| `resampling` | `"bilinear"` | GDAL resampling near the source's resolution, such as `"cubic"` |
| `fill_only` | `false` | Fill only the gaps of tiles that earlier sources have files over, adding no tiles of its own, as GEBCO under a regional pack |
| `sea_blend` | `0` | Heights only: a distance in metres. Within it of where the pack's data from this source ends (the edge of its files, or of the region), the source's sea, exactly 0 m in its datum, eases into the later sources' heights, at most sea level, so that it meets them without a step. See below |

Sources must be in geographic WGS84 coordinates (such as EPSG:4326); reproject others with `gdalwarp` first, and georeference plain images with `gdal_translate -a_srs EPSG:4326 -a_ullr <west> <north> <east> <south>`. Where a source has no data, the next source fills in. Imagery sources have 1 (grey) or 3 (RGB) bands of 8-bit values; a pack is grey only if all its sources are. Pixels without data are those at a band's nodata value.

**Sea at the edge of a regional pack.** Copernicus DEM sets the sea to exactly 0 m, while GEBCO, which fills in around a regional GLO-30 pack, has the seabed there, often hundreds of metres lower. Where the GLO-30 pack ends at sea, the two surfaces would meet with a step, which the runtime's skirts fill as a wall. With `"sea_blend": 10000` on the GLO-30 source and GEBCO as the next source, with `"fill_only": true` so that the pack keeps to GLO-30's tiles, GLO-30's sea within 10 km of the pack's edge takes GEBCO's seabed at the edge, eased to sea level at 10 km, as in `examples/glo30-norway.json`. Only the sea near the edge changes: fjords and lakes inside the coverage keep GLO-30's flat water, which GEBCO, at 450 m, does not resolve.

The default height steps are 0.5 m to level 5, 0.25 m to level 9 and 0.125 m beyond, and 2^-8 m for geoid packs. A tile whose heights do not fit in 65,534 steps fails the bake with a message that names it, rather than being clipped.

## How tiles are computed

Every tile is resampled from its sources directly onto its own sample grid, through GDAL's warper with an exact transform and a fixed kernel scale, so that each sample's value depends only on its position. Neighbouring tiles, even in different shards, therefore compute identical values for their shared borders, and shards bake independently. Samples that represent the same point, on the antimeridian and at the poles, are taken at one position (longitude -180, and longitude 0 at the poles), so they are identical too. Near a source's resolution the source is interpolated; at levels coarser than 1.5 times its pixel size it is filtered by area averaging (or the maximum). Heights referenced to EGM2008 are converted to ellipsoidal heights with the geoid pack, using the same interpolation as the runtime.

Imagery is computed the same way, at pixel centres: an image of (*P* + 2*b*) pixels per side for a tile of *P* pixels with border *b* (pack format, section 2.4), one band at a time. Because each pixel depends only on its position, a border pixel equals the neighbour's own pixel at the same place before encoding. A border column beyond the antimeridian is sampled on the other side of it, and a border row beyond a pole repeats the row inside it. A tile is written only if one of its own pixels has data.

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
