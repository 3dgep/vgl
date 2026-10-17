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
- OpenGL / OpenGL ES backend (`vgl/gl.h`, target `vgl::gl`, pkg-config `vgl-gl`):
  `vgl_gl_backend_create` with the host's function loader, `vgl_gl_backend_draw`,
  `vgl_gl_backend_reset` for a lost OpenGL context, and `vgl_gl_backend_destroy`. It draws on
  OpenGL 3.3 and OpenGL ES 3.0 or later, one draw call per tile, shaded by height and normal
  with an optional sun direction, and restores the host's state. C++ wrapper in `vgl/gl.hpp`.
- Depth conventions for every backend in `vgl/render.h`: `vgl_depth_convention` (logarithmic
  by default, reversed-Z and conventional) and `VGL_LOGARITHMIC_DEPTH_FAR_DEFAULT`.
- For libraries that work on behalf of a context, such as backends: `vgl_context_view_get`,
  `vgl_context_allocator_get` and `vgl_context_log`, with C++ wrappers.
- OpenGL sample (`samples/gl`, `vgl-gl`): the globe in a GLFW window on OpenGL or, with `--es`,
  OpenGL ES, with every depth convention and mouse controls. With `--check` it draws five
  views with each convention and checks coverage, agreement between the conventions, OpenGL
  errors and the restored host state.
- Pure-C sample (`samples/c`, `vgl-c`): the OpenGL sample's core in C99, with a `--check`.
- GLFW 3.4 for the samples, found or downloaded with a checked hash. The `VGL_TEST_GL` option
  runs the OpenGL samples' checks as tests, and a CI job runs them on Mesa under Xvfb, on
  OpenGL and OpenGL ES.
- `vgl::common::pointer_map` (`src/common`), a map from resource identifiers to objects for
  the libraries built on the C API, with tests.
- Imagery packs in the baker: `"layer": "imagery"` with `border`, `encoding` (`auto`, `jpeg`
  or `png`) and `jpeg_quality`. Tiles are sampled at pixel centres from grey or RGB 8-bit
  sources, one band at a time, with borders that repeat the neighbouring tiles' pixels (across
  the antimeridian too), and written as JPEG, or as PNG with alpha where data is missing.
  `vgl-bake validate` checks imagery packs: image sizes, borders and child masks. Tested on
  synthetic global and regional images.
- Sample imagery (`samples/data/packs/bluemarble`): NASA Blue Marble: Next Generation to level
  3, 1.6 MB, with its bake configuration.
- Imagery in the runtime: imagery packs load with the terrain and take part in selection,
  refining beyond the terrain's depth where they are finer. Their tiles are cached encoded and
  decoded on the task interface with stb_image 2.30 (found, or downloaded at a pinned commit),
  whose allocations go to the context's allocator, into RGBA8 textures with alpha-weighted mip
  levels. Textures are resources of the render interface, shared by every tile drawn with them,
  offered within the upload and GPU budgets and decoded again after eviction or a reset. Draw
  items get one layer per imagery pack with the deepest resident texture, mapped into an
  ancestor's texture and inside its border. Imagery packs are credited when in view.
- Debug views in the OpenGL backend: `vgl_gl_draw_info::debug_flags` with
  `VGL_GL_DEBUG_FLAG_WIREFRAME`, `VGL_GL_DEBUG_FLAG_LEVEL_COLORS` (a tint per tile level,
  with tile borders) and `VGL_GL_DEBUG_FLAG_IMAGERY_SOURCE` (imagery coarser than the tile, or
  missing).
- Sample detail (`samples/data/packs/glo30-sognefjord`): Copernicus GLO-30 over the Sognefjord,
  Jostedalsbreen and the western Jotunheimen to level 11 (about 76 m), 10.9 MB, above GEBCO in
  the samples, with its bake configuration. The OpenGL sample has a place there (7), and the
  headless check queries Galdhøpiggen in it.
- Mean sea level for consumers: draw items carry the geoid's heights at the tile's corners
  (`vgl_draw_item::geoid_heights` and `has_geoid`, appended), sampled from the geoid pack and
  filled in for meshes built before it loaded.
- A sea surface in the OpenGL backend (`vgl_gl_draw_info::sea_surface`, F8 in the OpenGL
  sample): terrain below mean sea level is raised to the geoid and shaded as water. Skirts are
  raised only as far as their tile's lowest point, so that they still hang below the raised
  surface, and keep the colour of the edge above them, rather than showing as strips of water
  along land in tiles that reach below sea level. Its colours
  by height and contour lines are now above mean sea level, with smoother hypsometric tints,
  anti-aliased contours that fade out where they crowd, and an automatic interval of about a
  250th of the altitude.
- Elevation views in the OpenGL backend: `VGL_GL_DEBUG_FLAG_HEIGHT_COLORS` (colours by height
  instead of imagery) and `VGL_GL_DEBUG_FLAG_CONTOURS`, with
  `vgl_gl_draw_info::contour_interval` (500 m by default, every fifth line heavier). The OpenGL
  sample turns them on with F6 and F7, with an interval that follows the altitude or a slider.
- Navigation in the style of Google Earth for every interactive sample, in the samples' shared
  library `vgl_sample_common` (`samples/common/navigation.hpp`), used first by the OpenGL sample:
  grabbing the globe with inertia, smooth zooms to the point under the cursor (also by
  double-click and right drag), orbiting and tilting about the point at the centre, looking
  around, keys to move, turn, tilt and zoom, and N, U and R to turn north up and look straight
  down. Its check verifies that grabbed points, zoom targets and orbit pivots stay in place to
  within a pixel, that tilting stops at its limit, and that the camera stays above the ground.
  The terrain height that points are found on is taken when a drag, throw or smooth zoom
  begins and held until it ends, so that panning and zooming over relief do not jitter or drift. Tilting stops at straight
  down instead of turning the view over. Orbiting with the middle button or Shift and the left
  button turns around the point under the cursor, which stays there, and does nothing when
  pressed over space; tilting stops where the camera looks straight down, which for a point off
  the centre of the view comes before the eye is above it. As in Google Earth, a small reticle
  marks the point that the mouse orbits or zooms around while it does, then fades over half a
  second (`globe_navigation::focus`, drawn by the OpenGL sample's overlay). A throw keeps the globe turning about its centre at
  the rate of the pan's last tenth of a second, measured on the clock of the frames, so that it
  starts at the speed of the hand, however the mouse events and frames fall, and slows down
  smoothly: its decay is integrated exactly, so that it goes as far in uneven frames as in even
  ones, and the OpenGL sample times navigation from one update to the next, so that the time
  spent updating and drawing counts too. Zooms and throws keep the height they set while the
  globe turns, so that zooms reach the same height in uneven frames as in even ones. The arrow
  keys and WASD move the camera over the ground at up to four view heights per second, the same
  forward as sideways, at a steady height, instead of moving the point at the centre of the view
  by pixels, which was faster forward than sideways when tilted. The speed eases in over about
  two seconds and stops within about 0.15 seconds, as a critically damped spring solved exactly
  for each frame. WASD move as the arrows do.
- The wireframe debug view shows the mesh's grid cells, without the diagonals that split them
  into triangles, in bright cyan, which shows against dark and light ground. Lines are a pixel
  wide and box-filtered. Where cells are closer than four pixels, it draws every second, fourth,
  eighth line and so on, blending from one step to the next, so that a readable grid shows at
  every distance, and it always draws the tiles' edges. It is drawn in the shading pass from the texture
  coordinates rather than as a second pass in line polygon mode, so it also works on OpenGL
  ES, and the backend no longer changes the polygon mode.
- The OpenGL sample's overlay, with Dear ImGui 1.92.9b (downloaded at a pinned hash, sample
  only): the context's statistics with memory against the budgets and a frame-time graph, the
  credits, the camera, the debug views, a frozen selection with the frozen frustum outlined,
  and the screen-space error. Its check draws once more with every debug view on.
- Statistics of imagery: `textures_decoding` and `textures_waiting` (wanted by selected tiles
  and not yet created by the consumer), appended to `vgl_context_statistics`. The samples wait
  for them before they check a view.
- The OpenGL backend draws up to four imagery layers per tile, with mip-mapped, anisotropic
  sampling, and restores the texture units, samplers and unpack state it uses. The OpenGL,
  pure-C and headless samples draw Blue Marble; the OpenGL check adds a view of the Sahara that
  only imagery colours sand.

- Coarser meshes at globe scale: a tile that can refine takes every second, fourth, eighth or
  sixteenth sample where every sample stays within half its geometric error of the coarser
  mesh, so within about a pixel. A view of the globe from orbit draws 4,500 triangles instead
  of 237,000.
- Meshes carry their full-resolution samples, a height and a normal each
  (`vgl_mesh_data::samples`, `VGL_TERRAIN_SAMPLE_SIZE`; render interface specification,
  section 6), and the OpenGL backend shades from them, so height colours, contours, the sea
  surface's coastline and lighting keep their detail on coarser meshes. The backend uses
  texture unit 4 for them, and restores it.
- The OpenGL backend's wireframe draws the tiles' boundaries in yellow, about two pixels wide,
  apart from the cyan grid cells.
- The samples read their data from `--data`, or else from the environment variable
  `VGL_SAMPLE_DATA`, or else from `samples/data` in the source tree, as before. `--pack`,
  repeated, loads the packs named instead of the sample set, later ones winning where they
  overlap. The sample tests name the sample data explicitly.
- Statistics of the last frame's meshes: `vertices_drawn` and `triangles_drawn`, appended to
  `vgl_context_statistics`, shown in the OpenGL sample's overlay and printed by its check.

### Changed

- The sample detail pack starts at level 7 instead of 6, so that it holds only its region: its
  level-6 tile covered twice the region, and showed level-6 detail south of it. Levels 7 to 11
  are unchanged.
- **Breaking:** `vgl_mesh_data` has the mesh's samples appended, so consumers stepping through
  the list of meshes must be built against the new header.
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
- The defaults module's allocation helpers moved to `src/common/host_memory.hpp`, shared with
  the backends. The samples share their camera math in `samples/common/sample_camera.hpp`.
- C API conventions, sections 10.1 and 13: libraries working on behalf of a context use its
  allocator and its log.
- The baker needs GDAL 3.7 or later, for warping one band of a multi-band source.
- Pack format, section 9: the imagery defaults are settled: 256-pixel tiles with a border of 1,
  JPEG with PNG for partial tiles, and level 7 as the deepest level of Blue Marble.
- Render interface specification, sections 4, 5 and 7: how textures are made, shared, chosen
  for layers and evicted, and that the consumer's texture formats keep their order.
- Render interface specification, section 8: the depth conventions are an enumeration, and for
  the logarithmic one the host passes the far distance when drawing, instead of the frame
  reporting a coefficient.
- C API conventions, section 12: a context may be destroyed while I/O requests are
  outstanding. It cancels them, and the host completes them later, as before; the completions
  are discarded. Previously this was an error.

### Fixed

- A finishing decode or mesh task could notify a condition variable of the context's shared
  state after `vgl_context_destroy` had freed it. ThreadSanitizer found it in the headless
  sample; the task now notifies while it holds the lock.
