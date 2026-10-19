# vgl (Virtual Globe Library)

[![CI](https://github.com/3dgep/vgl/actions/workflows/ci.yml/badge.svg)](https://github.com/3dgep/vgl/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

vgl is a lightweight, open-source C++ library for rendering a virtual globe inside an existing
application. It streams level-of-detail terrain and imagery on the WGS84 ellipsoid and draws it
through backends for OpenGL / OpenGL ES, Direct3D 11 and Ogre, on Windows, Linux, macOS, Android
and iOS, and in web browsers on WebGL 2. The public interface is a C API, so the library embeds as
easily in a C code base as in a C++ one.

> **Status:** version 1.0. The C ABI stays compatible within major version 1
> ([C API conventions](docs/c-api.md), section 14). The Ogre backend covers OpenGL 3+ and
> Direct3D 11 on Windows and Linux; Ogre on Vulkan, macOS and mobile follows
> ([changelog](CHANGELOG.md), Known limitations).

## Goals

- **Render a globe inside your application.** The host owns the window, main loop, camera and
  render target. vgl draws into the host's current render pass and never presents, swaps buffers
  or clears the render target.
- **C first.** The stable ABI is a C API of opaque handles and result codes. A header-only C++
  wrapper adds RAII, spans and enum classes on top of it, without a second ABI.
- **Small and predictable.** Minimal runtime dependencies (stb_image, Zstandard), no global
  state, no threads or network connections created by the library, all allocations routable
  through host allocator callbacks, and CPU and GPU memory bounded by budgets you set.
- **Several renderers, one core.** A renderer-independent core feeds backends for OpenGL 3.3 /
  OpenGL ES 3.0, Direct3D 11 and classic Ogre (1.x). A host with its own renderer can use the core alone.
- **Portable.** The runtime uses only the C and C++ standard libraries, with files, threads and
  network left to the host, so it runs the same on desktop and mobile. The baker runs on Windows
  and Linux.
- **Precise from orbit to the ground.** Double-precision CPU geometry, tile-relative vertices and
  reversed-Z (or logarithmic) depth, so there is no jitter or z-fighting at any distance.
- **Offline data pipeline.** A bundled baking tool turns free global datasets (Copernicus DEM,
  GEBCO) and your own imagery into sharded, range-readable tile packs. The runtime reads them
  from local disk or from any web server that supports HTTP range requests, such as an object
  store or the reference server configuration for Caddy ([hosting guide](docs/hosting.md)). A
  disk cache keeps what was read, so that areas seen once load again offline.
- **In the browser too.** An Emscripten build runs the OpenGL sample on WebGL 2, with threads,
  streaming packs from any server that allows it ([web sample](samples/gl/web/README.md)).
- **Easy to extend.** Backends, data providers, decoders, task scheduling, I/O, allocators and
  logging are all public interfaces. The built-in implementations use the same interfaces.

## Non-goals

These are out of scope for the first releases, and the architecture is not shaped around them:

- 3D Tiles and glTF streaming
- Text and label rendering
- Vector tile styling
- Time-dynamic data
- Clients for online services (Cesium ion, Google, Bing)
- General map reprojection at runtime
- An HTTP stack in the core (the host supplies I/O through a callback; the optional defaults
  module has an HTTP reader on libcurl)
- Direct3D 12, and OpenGL ES 2.0

## Quickstart

Try the [live demo](https://3dgep.github.io/vgl/demo/vgl-gl.html) in a browser first, or browse the
[project site](https://3dgep.github.io/vgl/) and its [API reference](https://3dgep.github.io/vgl/api/).
The [integrator's guide](https://github.com/3dgep/vgl/wiki) covers each step in detail, starting
with [Getting started](https://github.com/3dgep/vgl/wiki/Getting-Started).

**1. Get vgl.** Download a release archive from
[GitHub releases](https://github.com/3dgep/vgl/releases) and point `CMAKE_PREFIX_PATH` at it,
use the vcpkg port or the Conan recipe ([packaging](packaging/README.md)), or build from source
(below).

**2. See it run.** Build with a workflow preset and start the OpenGL sample on the sample data:

```sh
cmake --workflow --preset linux-gcc-static-release   # or windows-msvc-static-release, macos-clang-static-release
build/linux-gcc-static/bin/Release/vgl-gl
```

**3. Link it.**

```cmake
find_package(vgl 1 REQUIRED)
target_link_libraries(my_app PRIVATE vgl::vgl vgl::defaults vgl::gl)
```

**4. Draw a globe.** Your application keeps its window, loop and camera:

1. Create a thread pool and a file reader (`vgl_thread_pool_create`, `vgl_file_reader_create`)
   and pass their task and I/O interfaces to `vgl_context_create`.
2. Add packs with `vgl_pack_create`: elevation, the geoid and imagery, such as those in
   `samples/data/packs`. Bake your own with `vgl-bake` ([baker](tools/baker/README.md)) and
   serve them from disk or any web server ([hosting](docs/hosting.md)).
3. Create a backend on your device, for example `vgl_gl_backend_create` with your OpenGL
   function loader.
4. Every frame: `vgl_context_view_set` with your camera, `vgl_context_update`, then the
   backend's draw (`vgl_gl_backend_draw`) inside your render pass. Show the lines of
   `vgl_context_attribution_get` while their data is in view.
5. Destroy the backend, packs and context, then the reader and the pool.

## Building

vgl uses CMake 3.24 or later and a C++20 compiler.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

`CMakePresets.json` (CMake 3.25 or later) has presets for the configurations CI builds: MSVC with
the Visual Studio generator on Windows, GCC and Clang with Ninja Multi-Config on Linux, each as a
static, shared or no-exceptions build, and Apple Clang on macOS. Cross-build presets cover Android
(`android-arm64`, with `ANDROID_NDK` pointing at an NDK), iOS (`ios-arm64`, on a Mac) and
WebAssembly (`emscripten`, with `EMSDK` pointing at an activated emsdk; the tests run on Node.js).
A workflow preset configures, builds and tests in one step:

```sh
cmake --list-presets=all
cmake --workflow --preset linux-gcc-static-debug
```

| Option | Default | Description |
|---|---|---|
| `BUILD_SHARED_LIBS` | `OFF` | Build vgl as a shared library |
| `VGL_BUILD_TESTS` | `ON` (top level) | Build the tests |
| `VGL_BUILD_SAMPLES` | `ON` (top level) | Build the samples |
| `VGL_BUILD_BAKER` | `OFF` | Build the baking tool (requires GDAL) |
| `VGL_BUILD_DEFAULTS` | `ON` | Build the optional defaults module: a thread pool, a file reader and a disk cache, and with `VGL_DEFAULTS_HTTP` an HTTP reader |
| `VGL_DEFAULTS_HTTP` | `OFF` | Give the defaults module an HTTP reader: an installed libcurl, or else curl 8.11.1 downloaded and built with HTTP and HTTPS only |
| `VGL_BACKEND_GL` | `ON` | Build the OpenGL / OpenGL ES backend |
| `VGL_BACKEND_D3D11` | `ON` (Windows) | Build the Direct3D 11 backend |
| `VGL_BACKEND_OGRE` | `OFF` | Build the Ogre backend (classic Ogre 14): an installed Ogre, or else Ogre 14.6.1 downloaded and built statically |
| `VGL_DISABLE_EXCEPTIONS` | `OFF` | Build the library without exceptions and RTTI |
| `VGL_WARNINGS_AS_ERRORS` | `OFF` | Treat compiler warnings as errors |
| `VGL_FETCH_DEPENDENCIES` | `ON` | Download the dependencies that are not installed: Zstandard and stb_image, curl for the HTTP reader, Ogre for its backend, nlohmann/json for the baker, and GLFW and Dear ImGui for the samples |
| `VGL_TEST_GL` | `OFF` | Run the OpenGL samples' checks as tests (needs a display) |
| `VGL_TEST_BROWSER` | `OFF` | With Emscripten, check the OpenGL sample's page in headless Chrome, served by Caddy (`VGL_CHROME`, `VGL_CADDY`) |
| `VGL_INSTALL` | `ON` (top level) | Generate install rules |

The Direct3D 11 backend needs the Windows SDK's HLSL compiler, `fxc`, which compiles its shaders
at build time. The Ogre backend (`VGL_BACKEND_OGRE`) draws on Ogre's OpenGL 3+ and Direct3D 11
render systems so far; building it the first time also builds Ogre, unless one is installed.

The samples run on the sample data in `samples/data`: `vgl-headless` without a GPU, `vgl-gl`
in a window (`--es` for OpenGL ES, `--depth reversed` for reversed-Z), `vgl-d3d11`, the same on
Direct3D 11 (reversed-Z by default, `--warp` for the software rasterizer), `vgl-ogre`, the same in
an Ogre scene (`--rs gl` or `--rs d3d11`), and `vgl-c`, the OpenGL sample's core in C. `vgl-gl` navigates like Google Earth: drag to grab the globe (from space, to spin it like a
disk about its centre), scroll or double-click to
zoom to the cursor, drag with Shift or the middle button to orbit and tilt, with Ctrl to look
around, and with the right button to zoom; arrows or WASD move, Shift with them orbits, Page
Up and Page Down zoom, and N, U and R turn north up, look straight down, or both. 1 to 7 fly
to places. Those keys move the camera smoothly, on an s-curve, rising over long flights.
Its panel (F1) shows the runtime statistics and the
debug views: wireframe (F2), colours by tile level (F3), the imagery source (F4), a frozen
selection (F5) to inspect level of detail and culling from outside, the heightmap (F6),
contour lines (F7), a sea surface (F8), the atmosphere (F9) with clouds, and generated stars, with the sun
and the sky set by the time of day, the day of the year and the year, or by the current time. `--stars <file>` draws the real sky
instead, from a copy of the Yale Bright Star Catalogue that you download yourself, since its
licence keeps it out of the repository: CDS catalogue V/50's
[catalog.gz](https://cdsarc.cds.unistra.fr/ftp/V/50/catalog.gz), decompressed.

The samples find `samples/data` in the source tree by default; `--data <directory>` or the
environment variable `VGL_SAMPLE_DATA` points them at a copy elsewhere, and `--pack <uri>`,
repeated, loads other packs instead of the sample set, later ones winning where they overlap:

```
vgl-gl --pack samples/data/packs/gebco --pack /data/packs/glo30-norway \
       --pack samples/data/packs/egm2008 --pack samples/data/packs/bluemarble
```

### Using vgl

vgl is packaged for vcpkg and Conan, and released as archives (`packaging/README.md`). After
`cmake --install build`, or with a package, consume vgl through its CMake package:

```cmake
find_package(vgl REQUIRED)
target_link_libraries(my_app PRIVATE vgl::vgl vgl::gl)
```

or through pkg-config:

```sh
cc main.c $(pkg-config --cflags --libs vgl)
```

## Repository layout

| Path | Contents |
|---|---|
| `include/vgl/` | Public C API headers of the core and defaults, and the header-only C++ wrapper |
| `src/` | Core modules: foundation, geodesy, tiling, cache, providers, decoders, mesh, render, defaults |
| `backends/` | OpenGL / GLES, Direct3D 11 and Ogre backends, each with its headers in `include/vgl/` |
| `tools/baker/` | Offline baking tool |
| `tools/server/` | Reference server for packs: a Caddy configuration and its check |
| `samples/` | Sample applications |
| `tests/` | Tests, and the consumer project that checks an installed vgl (`tests/install`) |
| `packaging/` | vcpkg port, Conan recipe, and how releases are made |
| `docs/` | Design document and specifications |
| `site/` | The project site on GitHub Pages: landing page, live demo and API reference |
| `wiki/` | The [GitHub wiki](https://github.com/3dgep/vgl/wiki), as a git submodule |

## Documentation

- [Design document](docs/design.md): requirements, architecture, data pipeline and roadmap
- [C API conventions](docs/c-api.md): naming, types, errors, versioned structures, threading and ABI rules
- [Pack format](docs/pack-format.md): tiling scheme, manifest, shards and tile encodings
- [Hosting packs](docs/hosting.md): object stores and the reference server, deployment and security
- [Packaging](packaging/README.md): vcpkg, Conan, release archives and the release steps
- [Contributing](CONTRIBUTING.md): coding style, sign-off and review process
- [Security policy](SECURITY.md): reporting vulnerabilities privately
- [Project site](https://3dgep.github.io/vgl/): the live demo and the API reference (`site/README.md`)
- [Changelog](CHANGELOG.md)
- [Wiki](https://github.com/3dgep/vgl/wiki): guides and notes alongside the specifications

## Data attribution

The vgl code is MIT-licensed. Baked datasets carry the attribution requirements of their
sources, independently of the code license. For example, packs built from Copernicus DEM must
credit "produced using Copernicus WorldDEM-30 © DLR e.V.". vgl reports the attribution of the
data currently visible so the host can display it.

## License

vgl is released under the [MIT license](LICENSE). Copyright (c) 2026 Jeremiah van Oosten.
