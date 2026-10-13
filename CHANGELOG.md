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
- `vgl/base.h`: export and deprecation macros, `vgl_bool`, double-precision vector and matrix
  types, the `vgl_result` codes, `vgl_result_name_get` and `vgl_version_get`.
- C++ wrapper basics: `vgl::result`, `vgl::expected`, `vgl::init` and `vgl::version_get`.
