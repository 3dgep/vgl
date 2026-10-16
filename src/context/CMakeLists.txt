# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# The context: packs, loading through the host's I/O interface, decoding on its
# task interface, the CPU cache budget, height queries and tile selection (vgl/context.h).

target_sources(vgl_objects PRIVATE
    attribution.cpp
    build_meshes.cpp
    context.cpp
    context_impl.hpp
    frame.cpp
    select_tiles.cpp
    statistics.cpp)
