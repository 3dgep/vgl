# vgl (Virtual Globe Library)

vgl is a lightweight, open-source C++ library for rendering a virtual globe, with backends for OpenGL / OpenGL ES, Direct3D 11 and Ogre (classic, 1.x). The runtime targets Windows, Linux, macOS, Android and iOS and must stay platform-independent; the baker runs on Windows and Linux only. It is maintained by Jeremiah van Oosten at github.com/3dgep/vgl under the MIT license.

## Source of truth

Read `docs/design.md` before starting any task. It records the requirements, architecture and decisions made during planning. If a task conflicts with the design document, stop and ask rather than diverging silently. When a decision changes, update the design document in the same change.

## Status

Planning is complete, and milestone M0 (design document, section 14) is done: all four steps below. The test bake report is `docs/test-bake.md`. The baker (`tools/baker`) already covers much of M1. M2 has started: the context (`vgl/context.h`) loads packs, including the EGM2008 geoid pack, through the I/O interface, decodes tiles on the task interface, keeps them within a CPU budget, answers height queries, selects tiles for the host's view and builds their meshes. The defaults module (`vgl/defaults.h`) provides a thread pool and a file reader. The core side of the render interface (`vgl/render.h`, specified in `docs/render-interface.md`) hands frames of resources and draw lists to consumers; the backends come next. The M0 steps were:

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
- **Dependencies:** keep runtime dependencies minimal (stb_image, Zstandard). GDAL is allowed only in the baking tool.
- **Portability:** the runtime core uses only the C and C++ standard libraries: no operating system APIs, `std::filesystem` or `std::thread` (design document, section 4).
- **License headers:** every source file starts with `SPDX-License-Identifier: MIT` and `Copyright (c) 2026 Jeremiah van Oosten`.
- **Commits:** sign off every commit (`git commit -s`) for the Developer Certificate of Origin.
- **Writing style:** never use em dashes in any text: code comments, documentation, commit messages or replies.

## Open questions

These are waiting on answers from a prospective user of the library (see design document, section 16). Do not assume answers to them:

- Ogre: classic Ogre (1.x) is confirmed; the exact release and the render systems per platform (Metal or OpenGL on macOS and iOS) are open
- Graphics API of their C project, which decides the first backend
- Platforms: Windows, Linux, macOS, Android and iOS are confirmed; minimum OS versions, Intel Macs, and 32-bit ARM are open
- Imagery: source, resolution, coverage and format
- Oceans: render bathymetry, or a flat sea surface
- Hosting for the web-served data
