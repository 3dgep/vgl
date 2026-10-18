# vgl (Virtual Globe Library)

vgl is a lightweight, open-source C++ library for rendering a virtual globe, with backends for OpenGL / OpenGL ES, Direct3D 11 and Ogre 14 (the classic line). The runtime targets Windows, Linux, macOS, Android and iOS and must stay platform-independent; the baker runs on Windows and Linux only. It is maintained by Jeremiah van Oosten at github.com/3dgep/vgl under the MIT license.

## Source of truth

Read `docs/design.md` before starting any task. It records the requirements, architecture and decisions made during planning. If a task conflicts with the design document, stop and ask rather than diverging silently. When a decision changes, update the design document in the same change.

## Status

Planning is complete, and milestone M0 (design document, section 14) is done: all four steps below. The test bake report is `docs/test-bake.md`. The baker (`tools/baker`) already covers much of M1. M2 has started: the context (`vgl/context.h`) loads packs, including the EGM2008 geoid pack, through the I/O interface, decodes tiles on the task interface, keeps them within a CPU budget, answers height queries, selects tiles for the host's view and builds their meshes. The defaults module (`vgl/defaults.h`) provides a thread pool, a file reader and, with `VGL_DEFAULTS_HTTP` (off by default, on libcurl), an HTTP reader for packs on a web server. The reference server for packs is a Caddy configuration (`tools/server`, `docs/hosting.md`), checked in CI by `tools/server/check.sh`. The core side of the render interface (`vgl/render.h`, specified in `docs/render-interface.md`) hands frames of resources and draw lists to consumers. `vgl_context_statistics_get` and the headless sample (`samples/headless`, with sample data in `samples/data`) and attribution (`vgl_context_attribution_get`) complete the GPU-independent part. The OpenGL / OpenGL ES backend (`vgl/gl.h`, `backends/gl`) draws the terrain with imagery (decoded by the core with stb_image into textures with mip levels) and every depth convention, and the OpenGL and pure-C samples (`samples/gl`, `samples/c`) check it on the Blue Marble sample imagery in `samples/data`, which the baker wrote, with an optional sea surface. The Direct3D 11 backend (`vgl/d3d11.h`, `backends/d3d11`, shaders compiled at build time with `fxc`) draws the same, checked by the Direct3D 11 sample (`samples/d3d11`) on WARP where there is no GPU. macOS, Android and iOS already build in CI, which completes M2. M3 has started with the Ogre backend (`vgl/ogre.hpp`, `backends/ogre`, `VGL_BACKEND_OGRE`, off by default; Ogre 14.6.1 is downloaded and built when none is installed), drawing on Ogre's OpenGL 3+ and Direct3D 11 render systems with unified shaders and checked by the Ogre sample (`samples/ogre`); OpenGL ES 3, Vulkan, Metal and the mobile platforms follow. Imagery packs are layers whose opacity and priority the host changes at run time (`vgl_pack_opacity_set`, `vgl_pack_priority_set`), shown in the samples with Sentinel-2 cloudless 2016 over the Sognefjord above Blue Marble. Picking (`vgl_context_ray_query`) intersects rays with the loaded terrain, and the samples' navigation grabs the terrain under the cursor with it. Every backend draws an optional atmosphere (sky, limb glow and haze, from `backends/common/atmosphere.glsl`), the stars a host gives it, as quads behind the sky (`backends/common/stars.hpp`), and a cloud shell from a host's coverage image (`backends/common/clouds.hpp`). The M0 steps were:

1. Repository setup: MIT `LICENSE`, `README.md` (goals and non-goals), `CONTRIBUTING.md` (coding style, DCO sign-off, review process), issue templates, `.clang-format`, `.clang-tidy`, a CMake skeleton following the module layout in section 6.2, and a GitHub Actions pipeline for Windows and Linux.
2. Two specifications, written before implementation, in `docs/`: the C API conventions (section 7.1) and the pack format (sections 8.4 and 8.5).
3. Foundation, geodesy and tiling modules, with tests against GeographicLib reference values.
4. A minimal baker and a regional test bake (Copernicus GLO-30 over Norway, plus the global GEBCO grid) to settle tile size, deepest level and shard level.

## Conventions

- **Language:** C++20 for the implementation. The public ABI is the C API; a header-only C++ wrapper sits on top of it.
- **C API:** every public declaration follows `docs/c-api.md`; check new API against its review checklist (section 17).
- **Render interface:** the core, the backends and the samples follow `docs/render-interface.md`; a change to the interface updates the specification.
- **Pack format:** the baker and runtime follow `docs/pack-format.md`. A change to the format updates the specification and, if it affects readers, its format version.
- **Naming:** C functions use the `vgl_` prefix in snake case, object before action (`vgl_context_create`). Types use `vgl_` (`vgl_context`). Constants and macros use `VGL_`, then the type name, then the value (`VGL_RESULT_OUT_OF_MEMORY`). The C++ wrapper lives in namespace `vgl`.
- **Library rules:** no exceptions across the API boundary, buildable without exceptions and RTTI, no global state, no threads or network connections created by the library, all allocations routable through host allocator callbacks.
- **Dependencies:** keep runtime dependencies minimal (stb_image, Zstandard). libcurl is allowed only in the defaults module's optional HTTP reader, and GDAL only in the baking tool.
- **Portability:** the runtime core uses only the C and C++ standard libraries: no operating system APIs, `std::filesystem` or `std::thread` (design document, section 4).
- **License headers:** every source file starts with `SPDX-License-Identifier: MIT` and `Copyright (c) 2026 Jeremiah van Oosten`.
- **Commits:** sign off every commit (`git commit -s`) for the Developer Certificate of Origin.
- **Checks:** after a change, build and test on Windows only, running the tests from the Release build (`ctest -C Release`), which is about four times faster than Debug. Run the Linux builds (GCC and Clang) and clang-tidy only before a commit, to keep small changes quick.
- **Writing style:** never use em dashes in any text: code comments, documentation, commit messages or replies.

## Former open questions

The questions that waited on the client are settled with recommended choices (design document, section 16): Ogre 14 (14.6.0 when decided); the OpenGL / OpenGL ES backend first, then Direct3D 11; Windows 10, macOS 11 (universal), Android 7.0 (API 24, 64-bit only) and iOS 15; Blue Marble and Sentinel-2 cloudless 2016 imagery; bathymetry with an optional sea surface; hosting on an object store without egress fees. Change them only by updating the design document.
