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
- Pack format specification, format version 1.0 (`docs/pack-format.md`).
- `CMakePresets.json` with configure, build, test and workflow presets for the CI platforms
  (MSVC, and GCC and Clang with Ninja Multi-Config). CI builds through these presets.
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
- Geoid packs in the pack format (layer type 3, section 6.4): the EGM2008 5' grid, baked to
  level 5, provides heights above mean sea level once loaded. The design document records the
  decision.
- Test harness without dependencies (`tests/harness`), and unit tests for the three modules.

### Changed

- **Breaking:** the C++ wrapper for `vgl/base.h` moved from `vgl/vgl.hpp` to `vgl/base.hpp`.
  `vgl/vgl.hpp` now includes every core wrapper header.
- The allocator interface entry `free` in the C API conventions is now `deallocate`.
- The core modules build into an object library, `vgl_objects`, from which `vgl` is made.
- On Linux, `vgl` links the math library, and a static `vgl` also the C++ runtime, so C hosts
  can link it with the C linker.
- `vgl/base.h`: export and deprecation macros, `vgl_bool`, double-precision vector and matrix
  types, the `vgl_result` codes, `vgl_result_name_get` and `vgl_version_get`.
- C++ wrapper basics: `vgl::result`, `vgl::expected`, `vgl::init` and `vgl::version_get`.
