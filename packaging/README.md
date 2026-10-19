# Packaging

vgl is released in three forms (design document, section 13.4), all built from the same install
rules (`cmake/vgl_install.cmake`) and checked by the same consumer project (`tests/install`),
which builds the C API test and the C++ wrapper test against the package and runs them.

| Form | Where | Contents |
|---|---|---|
| Release archives | GitHub releases, built by `.github/workflows/release.yml` | Windows x64, Linux x64 and macOS (universal), each shared and static: the core, the defaults module without the HTTP reader, the OpenGL backend, and the Direct3D 11 backend on Windows |
| vcpkg port | `packaging/vcpkg/ports/vgl` | Features `defaults`, `gl` and `d3d11` (Windows) by default, plus `http` and `ogre` |
| Conan recipe | `packaging/conan` | Options `with_defaults`, `with_gl` and `with_d3d11` (Windows) on by default, plus `with_http`. ConanCenter has no Ogre 14, so the Ogre backend is left out |

Every form installs the CMake package (`find_package(vgl)`, with the targets `vgl::vgl`,
`vgl::defaults`, `vgl::gl` and `vgl::d3d11`, and `vgl::ogre` from the vcpkg port's `ogre`
feature or a source build against an installed Ogre) and, except on Windows, the pkg-config
modules `vgl`, `vgl-defaults` and `vgl-gl`. The licenses of vgl and of the code compiled into
it (stb_image, Zstandard in the archives, and a downloaded curl in a shared build with the
HTTP reader) are in `share/doc/vgl`.

## Using the packages

vcpkg, until the port is in the vcpkg registry, as an overlay port:

```sh
vcpkg install vgl[http] --overlay-ports=<vgl>/packaging/vcpkg/ports
```

Conan, until the recipe is in ConanCenter, from the repository:

```sh
conan create <vgl>/packaging/conan -s compiler.cppstd=20 -o "vgl/*:with_http=True"
```

A release archive is an install prefix: point `CMAKE_PREFIX_PATH` (or `PKG_CONFIG_PATH`, at
its `lib/pkgconfig`) at the extracted folder. The archives are built in Release. A static
library from MSVC links only into Release builds, since MSVC does not mix the Debug and
Release runtimes, so Debug builds on Windows use the shared archive, vcpkg or Conan.

## Testing a change

The vcpkg port builds the working tree instead of the release when `VGL_PORT_SOURCE_DIR`
names it. vcpkg caches a port's binaries by its port files, not the sources, so a changed tree
is reinstalled without the cache:

```sh
export VGL_PORT_SOURCE_DIR=<vgl> VCPKG_KEEP_ENV_VARS=VGL_PORT_SOURCE_DIR
vcpkg remove vgl --overlay-ports=<vgl>/packaging/vcpkg/ports
vcpkg install vgl[http] --no-binarycaching --overlay-ports=<vgl>/packaging/vcpkg/ports
cmake -S <vgl>/tests/install -B consumer -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build consumer && ctest --test-dir consumer
```

The Conan recipe always builds its working tree, and `conan create` runs the consumer project
as its test package.

## Releasing

1. Set the version in `CMakeLists.txt` and in `packaging/vcpkg/ports/vgl/vcpkg.json` (the
   Conan recipe reads `CMakeLists.txt`), and give `CHANGELOG.md` a section `## [<version>]`
   for it; the release notes are that section.
2. Push the tag `v<version>`. The release workflow checks that the tag matches both
   versions, builds and tests each archive, checks it with the consumer project, and creates
   a draft release with the archives and `SHA256SUMS`.
3. Review the draft and publish it.
4. vcpkg: update the registry port in `vcpkg-registry/ports/vgl` (below) to the version and
   the SHA512 of the tag's source archive
   (`https://github.com/3dgep/vgl/archive/v<version>.tar.gz`), copy it into a fork of microsoft/vcpkg without its license header, run
   `vcpkg format-manifest` and `vcpkg x-add-version`, and open a pull request.
5. Conan: update the recipe in `conan-center/recipes/vgl` (below) to the version and the
   SHA-256 of the same archive, copy it into a fork of conan-io/conan-center-index without the
   license headers, check it with `conan create all --version <version>`, and open a pull
   request.

## Registry submissions

`vcpkg-registry` and `conan-center` hold the port and the recipe in the forms the vcpkg
registry and ConanCenter take: they download the tag's source archive instead of building the
working tree, and the recipe's test package is self-contained. Both were tested on 1.0.0:
the port on x64-windows, x64-windows-static and x64-linux, with its `ogre` feature on
x64-windows; the recipe with GCC on Linux and MSVC on Windows, static and shared.

- **vcpkg:** not submitted yet. The registry takes new ports of projects with a release at
  least six months old or six months of public development, so vgl waits until about
  April 2027. Its port name is also still to settle: other projects are known as vgl
  (VirtualGL, VXL's geometry library), and the registry always accepts the `Owner-Project`
  form, `3dgep-vgl`. The port name does not change `find_package(vgl)` or the `vgl::` targets.
- **ConanCenter:** not submitted yet. First-time contributors sign its CLA.
