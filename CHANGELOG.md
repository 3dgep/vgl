# Changelog

All notable changes to vgl are documented in this file. The project follows
[Semantic Versioning](https://semver.org): within a major version, the C ABI stays compatible
(C API conventions, section 14), and breaking changes are listed here under **Breaking**.

The pack format has its own version number, independent of the library version.

## [Unreleased]

## [1.0.0] - 2026-10-19

The first release. vgl is a lightweight library for rendering a virtual globe inside an
existing application, behind a C API with a header-only C++ wrapper. The development history
before it is in the repository's commits.

### Runtime

- **Context** (`vgl/context.h`): packs of elevation, imagery and the geoid, loaded through the
  host's I/O interface and layered by priority; imagery layers whose opacity and priority the
  host changes at run time; tile selection by screen-space error with frustum and horizon
  culling; tile meshes with skirts, built on the host's task interface; CPU, GPU and upload
  budgets; height queries above the ellipsoid or mean sea level; picking
  (`vgl_context_ray_query`); screen projection (`vgl_view_project`); statistics; and the
  attribution of the data in view.
- **Render interface** (`vgl/render.h`, `docs/render-interface.md`): frames of resources and
  draw lists with double-precision, camera-relative transforms, for the built-in backends and
  for hosts with their own renderer. Logarithmic, reversed-Z and conventional depth.
- **Foundation and geodesy** (`vgl/foundation.h`, `vgl/geodesy.h`): host allocators, logging,
  I/O and tasks; WGS84 and other ellipsoids, geodetic and ECEF conversions, local ENU frames and
  the Earth rotation angle, tested against GeographicLib.
- **Defaults module** (`vgl/defaults.h`, `vgl::defaults`): a thread pool, a file reader, an
  optional HTTP reader on libcurl (`VGL_DEFAULTS_HTTP`, the browser's fetch on WebAssembly), and
  a disk cache that keeps packs seen once available offline.
- **Library rules:** no exceptions across the API, buildable without exceptions and RTTI, no
  global state, no threads or connections created by the core, and every allocation through
  the host's allocator.

### Backends

- **OpenGL / OpenGL ES** (`vgl/gl.h`, `vgl::gl`): OpenGL 3.3, OpenGL ES 3.0 and WebGL 2.
- **Direct3D 11** (`vgl/d3d11.h`, `vgl::d3d11`): feature level 11_0, with shaders compiled at
  build time.
- **Ogre** (`vgl/ogre.hpp`, `vgl::ogre`): classic Ogre 14 on its OpenGL 3+ and Direct3D 11
  render systems, on Windows and Linux.
- Every backend draws up to four imagery layers per tile, an optional sea surface at the
  geoid, an atmosphere (sky, limb glow and haze), the host's stars and a cloud shell, with
  debug views (wireframe, level colours, imagery source, height colours) and contour lines.

### Data and tools

- **Pack format 1.0** (`docs/pack-format.md`): sharded, range-readable tile packs for
  elevation, imagery and the geoid, readable from local disk or any web server with HTTP range
  requests.
- **Baker** (`vgl-bake`, Windows and Linux, on GDAL 3.7 or later): deterministic, parallel and
  resumable bakes of elevation (with datum conversion and sea blending) and imagery, plus
  `inspect`, `validate`, `query` and `check`, which checks a hosted pack as vgl and browsers
  read it.
- **Reference server** (`tools/server`): a Caddy configuration for serving packs, with the
  hosting guide (`docs/hosting.md`). The project hosts no data: consumers bake and host their
  own packs.

### Platforms, samples and packaging

- Windows 10, Linux, macOS 11 (universal), Android 7.0 (API 24, arm64), iOS 15 and WebAssembly
  with threads (Emscripten).
- Samples: OpenGL (also as a web page), Direct3D 11, Ogre, pure C and headless, with
  Google Earth style navigation, on small sample packs of Blue Marble, Sentinel-2 cloudless
  2016, Copernicus GLO-30, GEBCO and EGM2008 (`samples/data`).
- Release archives for Windows, Linux and macOS, shared and static; a vcpkg port and a Conan
  recipe (`packaging`); a CMake package (`find_package(vgl)`) and pkg-config files.
- Documentation: the design document and specifications in `docs`, and an integrator's guide
  in the [wiki](https://github.com/3dgep/vgl/wiki).

### Known limitations

- The Ogre backend's Vulkan render system, macOS and the mobile platforms come after version 1
  (design document, milestone M3b).
- Imagery is stored as JPEG or PNG; WebP and compressed GPU textures (BC1, BC7) are reserved in
  the pack format but not supported yet.
- The Android and iOS builds are built in CI, but not yet run on a device, emulator or
  simulator.
- The release archives leave out the HTTP reader, which needs libcurl: use vcpkg, Conan or a
  source build for it. The static Windows archive links only into Release builds.
