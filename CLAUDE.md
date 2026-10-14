# vgl (Virtual Globe Library)

vgl is a lightweight, open-source C++ library for rendering a virtual globe, with backends for OpenGL / OpenGL ES, Direct3D 11 and Ogre-Next. It is maintained by Jeremiah van Oosten at github.com/3dgep/vgl under the MIT license.

## Source of truth

Read `docs/design.md` before starting any task. It records the requirements, architecture and decisions made during planning. If a task conflicts with the design document, stop and ask rather than diverging silently. When a decision changes, update the design document in the same change.

## Status

Planning is complete and work is at milestone M0 (design document, section 14). Steps 1 and 2 are done. Step 3 is done. The geoid is an EGM2008 5′ geoid pack (pack format, section 6.4): the baker produces it in step 4, and the runtime loads it with the context in M2, as it does the task and I/O callback interfaces of the foundation module. The steps, in order:

1. Repository setup: MIT `LICENSE`, `README.md` (goals and non-goals), `CONTRIBUTING.md` (coding style, DCO sign-off, review process), issue templates, `.clang-format`, `.clang-tidy`, a CMake skeleton following the module layout in section 6.2, and a GitHub Actions pipeline for Windows and Linux.
2. Two specifications, written before implementation, in `docs/`: the C API conventions (section 7.1) and the pack format (sections 8.4 and 8.5).
3. Foundation, geodesy and tiling modules, with tests against GeographicLib reference values.
4. A minimal baker and a regional test bake (Copernicus GLO-30 over Norway, plus the global GEBCO grid) to settle tile size, deepest level and shard level.

## Conventions

- **Language:** C++20 for the implementation. The public ABI is the C API; a header-only C++ wrapper sits on top of it.
- **C API:** every public declaration follows `docs/c-api.md`; check new API against its review checklist (section 17).
- **Pack format:** the baker and runtime follow `docs/pack-format.md`. A change to the format updates the specification and, if it affects readers, its format version.
- **Naming:** C functions use the `vgl_` prefix in snake case, object before action (`vgl_context_create`). Types use `vgl_` (`vgl_context`). Constants and macros use `VGL_`, then the type name, then the value (`VGL_RESULT_OUT_OF_MEMORY`). The C++ wrapper lives in namespace `vgl`.
- **Library rules:** no exceptions across the API boundary, buildable without exceptions and RTTI, no global state, no threads or network connections created by the library, all allocations routable through host allocator callbacks.
- **Dependencies:** keep runtime dependencies minimal (stb_image, Zstandard). GDAL is allowed only in the baking tool.
- **License headers:** every source file starts with `SPDX-License-Identifier: MIT` and `Copyright (c) 2026 Jeremiah van Oosten`.
- **Commits:** sign off every commit (`git commit -s`) for the Developer Certificate of Origin.
- **Writing style:** never use em dashes in any text: code comments, documentation, commit messages or replies.

## Open questions

These are waiting on answers from a prospective user of the library (see design document, section 16). Do not assume answers to them:

- Ogre version (Ogre-Next is assumed for now)
- Graphics API of their C project, which decides the first backend
- Operating systems: Windows only, or Linux as well
- Imagery: source, resolution, coverage and format
- Oceans: render bathymetry, or a flat sea surface
- Hosting for the web-served data
