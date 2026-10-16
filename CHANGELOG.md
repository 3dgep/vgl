# Changelog

All notable changes to vgl are documented in this file. The project follows
[Semantic Versioning](https://semver.org). Before version 1.0, minor versions may contain
breaking changes; they are listed here under **Breaking**.

The pack format has its own version number, independent of the library version.

## [Unreleased]

### Added

- Repository setup: license, README, contribution guidelines, issue templates, clang-format and
  clang-tidy configuration, CMake build skeleton, and GitHub Actions CI for Windows and Linux.
- C API conventions specification (`docs/c-api.md`).
- Pack format specification, format version 1.0 (`docs/pack-format.md`), including geoid packs
  (layer type 3, section 6.4).
- `CMakePresets.json` with configure, build, test and workflow presets for the CI platforms
  (MSVC, and GCC and Clang with Ninja Multi-Config). CI builds through these presets.
- `vgl/base.h`: export and deprecation macros, `vgl_bool`, double-precision vector and matrix
  types, the `vgl_result` codes, `vgl_result_name_get` and `vgl_version_get`.
- C++ wrapper basics: `vgl::result`, `vgl::expected`, `vgl::init` and `vgl::version_get`.
- Foundation module: `vgl/foundation.h` with the allocator interface (`vgl_allocator_interface`)
  and log callback types, the `vgl::allocator` C++ adapter, and internal allocator, logger and
  double-precision math.
- Geodesy module: `vgl/geodesy.h` with `vgl_ellipsoid` and `vgl_geodetic`, WGS84, closed-form
  conversions between geodetic and ECEF coordinates, and local ENU frames, tested against
  GeographicLib 2.3 reference values. Internally, conservative bounding boxes and spheres for
  geographic regions.
- Tiling module: the geographic tiling scheme of the pack format (addresses, keys, bounds,
  sample and pixel placement, shard lookup, bilinear interpolation within a tile), internal to
  the library.
- Test harness without dependencies (`tests/harness`), and unit tests for the modules.
- Dependency handling (`cmake/vgl_dependencies.cmake`): Zstandard is used from an installed
  package, or downloaded (1.5.7, hash-checked) and compiled into vgl with hidden symbols.
- Pack format readers (`src/providers`): manifest, shard header, directory and tile parsing with
  the validation of the specification's section 8, and decoding of elevation and geoid tiles.
  Zstandard decompression routes every allocation through the vgl allocator.
- Pack format writers (`tools/baker/pack_writer`), and tests for round trips, determinism,
  versioning and corruption: every single-byte change and truncation is detected.
- Baking tool `vgl-bake` (`tools/baker`), with GDAL: `bake` (deterministic, parallel by shard,
  resumable, with a preview mode), `inspect`, `validate` (seams, poles, child masks and bounds),
  `query` and `sample-tile`. Example configurations for the M1 test bake, a conda-forge
  environment for Windows, and baker presets and CI jobs for Linux and Windows.
- Baker tests on synthetic surfaces: heights, seams, datum conversion, determinism and resuming.
- macOS presets and CI jobs (built and tested), and Android arm64 and iOS arm64 cross-build
  presets and CI jobs.
- Test bake report (`docs/test-bake.md`): sizes and bake times for GLO-30 over Norway, GEBCO
  and the geoid, and the decisions on tile size (129), deepest levels (12 for GLO-30, 8 for
  GEBCO) and shard level (`max_level` - 5).
- Foundation: the I/O interface (`vgl_io_interface`, `vgl_io_read`, `vgl_io_request` and
  `vgl_io_request_complete`) and the task interface (`vgl_task_interface`), with the C++
  adapters `vgl::io_interface` and `vgl::task_interface`.
- Context module: `vgl/context.h` with `vgl_context` (allocator, log, I/O, tasks, ellipsoid and
  CPU cache budget), `vgl_context_update` with a time budget, packs (`vgl_pack_create`,
  `vgl_pack_destroy`, `vgl_pack_state_get`) layered by priority, and
  `vgl_context_height_query` for ellipsoidal heights and, with a geoid pack, heights above mean
  sea level. Packs load through the I/O interface; tiles decode on the task interface or in the
  update, and are evicted least recently used over the CPU budget. C++ wrapper in
  `vgl/context.hpp`.
- Defaults module (`vgl/defaults.h`, target `vgl::defaults`, pkg-config `vgl-defaults`): a
  thread pool for the task interface and a filesystem reader for the I/O interface, which can
  read on the pool. C++ wrapper in `vgl/defaults.hpp`.
- Tile selection (design document, section 7.3): `vgl_view` and `vgl_context_view_set` set the
  camera, and each update selects tiles by screen-space error with frustum and horizon
  culling, refines only from loaded data, and requests missing tiles coarsest first with a
  cap on outstanding reads. Selected tiles are kept from eviction. The selection geometry is
  in `src/tiling/selection.hpp`; the selected tiles are internal until the backend interface
  exposes them.
- Tile mesh builder (`src/mesh/tile_mesh.hpp`, design document section 7.3): 24-byte vertices
  relative to each tile's centre with height, texture coordinates and octahedral normals, a
  skirt ring, and an index list shared per grid size. Each selected tile gets a mesh, built
  on the task interface from heights gathered in the update; nodata is filled from coarser
  tiles and lower-priority packs, tiles drawn from an ancestor use its samples, and the bare
  ellipsoid gets a curved grid. Meshes count towards the CPU budget, and a tile keeps its
  previous mesh until the new one is built.
- The core side of the render interface (`vgl/render.h`, `vgl/render.hpp`): frames with
  resource lists to destroy and create, draw lists with double-precision model-view matrices,
  an upload budget, a GPU budget (`vgl_context_create_info::gpu_budget`, 512 MiB by default),
  no holes while a region's replacements upload, CPU copies freed after upload, handles,
  failed creates, `vgl_context_resources_reset` and `vgl_context_texture_formats_set`.
  Tested with a consumer that checks the interface's rules in every frame.
- `VGL_POINTER_PADDING` in `vgl/base.h`, and a rule for it in the C API conventions
  (section 8.2): pointers next to 64-bit fields take 8 bytes on every target.
- Render interface specification (`docs/render-interface.md`), accepted:
  per-frame resource events and draw lists that backends and hosts pull, upload and GPU
  budgets, transforms, vertex and texture formats, depth conventions and threading.
- Attribution (FR-08): `vgl_context_attribution_get` lists the attribution lines of the packs
  whose tiles the view selects or the last frame drew, in pack priority order, each distinct
  line once. C++: `vgl::context::attribution_get`. The headless sample prints the credits and
  checks them.
- Statistics: `vgl_context_statistics_get` reports memory against the budgets, work under
  way, the view and the last frame, totals of reads and loaded tiles, the mean tile load time,
  and the times of the last update and frame. C++: `vgl::context::statistics_get`.
- Headless sample (`samples/headless`): a scripted camera from orbit down to 10 km, loading,
  selection and frames without a GPU, statistics per view, and heights at known places. With
  `--check` it verifies its results, and it runs as the `headless` test.
- Sample data (`samples/data`): GEBCO to level 3 and EGM2008 to level 2, about 4 MB, with
  their bake configurations and attribution.
- Context tests with synchronous, deferred and threaded hosts, allocation accounting, failure
  injection for every allocation, corrupt packs and the CPU budget; and tests of the defaults
  module on packs written to disk.

### Changed

- **Breaking:** the C++ wrapper for `vgl/base.h` moved from `vgl/vgl.hpp` to `vgl/base.hpp`.
  `vgl/vgl.hpp` now includes every core wrapper header.
- The allocator interface entry `free` in the C API conventions is now `deallocate`.
- The core modules build into an object library, `vgl_objects`, from which `vgl` is made.
- On Linux, `vgl` links the math library, and a static `vgl` also the C++ runtime, so C hosts
  can link it with the C linker.
- Pack format: writers quantize on the level's global grid of steps (section 6.2), so that ties
  round the same way in every tile, and the default height steps are 0.25 m up to level 9.
- The baker's default shard level is `max_level` - 5, instead of 6.
- On Android, a static `vgl` no longer adds `stdc++` to its link interface; the NDK's libc++ is
  linked by its toolchain.
- The documentation of `vgl_view` said that hosts with row-vector matrices pass them
  transposed; they pass them unchanged, as section 4.2 of the C API conventions says.
- The design document's open questions are settled with recommended choices (section 16):
  Ogre 14 (14.6.0 when decided) with Direct3D 11, OpenGL 3+, OpenGL ES 3, Vulkan and Metal;
  the OpenGL / OpenGL ES backend first, then Direct3D 11; Windows 10, macOS 11 (universal),
  Android 7.0 (API 24, 64-bit) and iOS 15; Blue Marble and Sentinel-2 cloudless 2016 imagery;
  bathymetry with an optional sea surface; hosting on an object store without egress fees.
- C API conventions, section 12: a context may be destroyed while I/O requests are
  outstanding. It cancels them, and the host completes them later, as before; the completions
  are discarded. Previously this was an error.

### Fixed

- A finishing decode or mesh task could notify a condition variable of the context's shared
  state after `vgl_context_destroy` had freed it. ThreadSanitizer found it in the headless
  sample; the task now notifies while it holds the lock.
