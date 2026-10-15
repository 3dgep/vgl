# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Tiling: the geographic quadtree of docs/pack-format.md, section 2, and the
# geometry of tile selection: frustum and horizon culling and screen-space error.

target_sources(vgl_objects PRIVATE
    selection.cpp
    selection.hpp
    tile.cpp
    tile.hpp)
