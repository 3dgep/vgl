# vgl (Virtual Globe Library)

[![CI](https://github.com/3dgep/vgl/actions/workflows/ci.yml/badge.svg)](https://github.com/3dgep/vgl/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

vgl is a lightweight, open-source C++ library for rendering a virtual globe inside an existing
application. It streams level-of-detail terrain and imagery on the WGS84 ellipsoid and draws it
through backends for OpenGL / OpenGL ES, Direct3D 11 and Ogre-Next. The public interface is a C
API, so the library embeds as easily in a C code base as in a C++ one.

> **Status:** early development. Planning is complete and work is at milestone M0 (foundations).
> Nothing is usable yet, and the API will change without notice before version 1.0.

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
  OpenGL ES 3.0, Direct3D 11 and Ogre-Next. A host with its own renderer can use the core alone.
- **Precise from orbit to the ground.** Double-precision CPU geometry, tile-relative vertices and
  reversed-Z (or logarithmic) depth, so there is no jitter or z-fighting at any distance.
- **Offline data pipeline.** A bundled baking tool turns free global datasets (Copernicus DEM,
  GEBCO) and your own imagery into sharded, range-readable tile packs. The runtime reads them
  from local disk or from any web server that supports HTTP range requests.
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
- A built-in HTTP stack (the host supplies I/O through a callback)
- Direct3D 12, and OpenGL ES 2.0

## Building

vgl uses CMake 3.24 or later and a C++20 compiler.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build
```

| Option | Default | Description |
|---|---|---|
| `BUILD_SHARED_LIBS` | `OFF` | Build vgl as a shared library |
| `VGL_BUILD_TESTS` | `ON` (top level) | Build the tests |
| `VGL_BUILD_SAMPLES` | `ON` (top level) | Build the samples |
| `VGL_BUILD_BAKER` | `OFF` | Build the baking tool (requires GDAL) |
| `VGL_BUILD_DEFAULTS` | `ON` | Build the optional thread pool and filesystem reader |
| `VGL_BACKEND_GL` | `ON` | Build the OpenGL / OpenGL ES backend |
| `VGL_BACKEND_D3D11` | `ON` (Windows) | Build the Direct3D 11 backend |
| `VGL_BACKEND_OGRE` | `OFF` | Build the Ogre-Next backend |
| `VGL_DISABLE_EXCEPTIONS` | `OFF` | Build the library without exceptions and RTTI |
| `VGL_WARNINGS_AS_ERRORS` | `OFF` | Treat compiler warnings as errors |
| `VGL_INSTALL` | `ON` (top level) | Generate install rules |

Most modules, backends, samples and the baker are still empty placeholders.

### Using vgl

After `cmake --install build`, consume vgl through its CMake package:

```cmake
find_package(vgl REQUIRED)
target_link_libraries(my_app PRIVATE vgl::vgl)
```

or through pkg-config:

```sh
cc main.c $(pkg-config --cflags --libs vgl)
```

## Repository layout

| Path | Contents |
|---|---|
| `include/vgl/` | Public C API headers and the header-only C++ wrapper |
| `src/` | Core modules: foundation, geodesy, tiling, cache, providers, decoders, mesh, render, defaults |
| `backends/` | OpenGL / GLES, Direct3D 11 and Ogre-Next backends |
| `tools/baker/` | Offline baking tool |
| `samples/` | Sample applications |
| `tests/` | Tests |
| `docs/` | Design document and specifications |

## Documentation

- [Design document](docs/design.md): requirements, architecture, data pipeline and roadmap
- [C API conventions](docs/c-api.md): naming, types, errors, versioned structures, threading and ABI rules
- [Contributing](CONTRIBUTING.md): coding style, sign-off and review process
- [Changelog](CHANGELOG.md)

## Data attribution

The vgl code is MIT-licensed. Baked datasets carry the attribution requirements of their
sources, independently of the code license. For example, packs built from Copernicus DEM must
credit "produced using Copernicus WorldDEM-30 © DLR e.V.". vgl reports the attribution of the
data currently visible so the host can display it.

## License

vgl is released under the [MIT license](LICENSE). Copyright (c) 2026 Jeremiah van Oosten.
