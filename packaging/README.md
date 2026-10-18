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
4. vcpkg: in a fork of microsoft/vcpkg, copy the port without its `VGL_PORT_SOURCE_DIR`
   branch, set `SHA512` to the hash of the tag's source archive (vcpkg prints it on the first
   install with `SHA512 0`), run `vcpkg x-add-version vgl`, and open a pull request.
5. Conan: in a fork of conan-io/conan-center-index, add a recipe whose `source()` downloads
   the tag's source archive (listed in `conandata.yml` with its hash) instead of exporting the
   working tree, with the test package made self-contained, and open a pull request.
