# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Tiling: the geographic quadtree of docs/pack-format.md, section 2. Screen-space
# error, culling and load prioritization are added in milestone M2.

target_sources(vgl_objects PRIVATE
    tile.cpp
    tile.hpp)
