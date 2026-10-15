# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Tile mesh builder: terrain meshes from heightmaps, skirts, normals, imagery texture coordinates.

target_sources(vgl_objects PRIVATE
    tile_mesh.cpp
    tile_mesh.hpp)
