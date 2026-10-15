# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# The context: packs, loading through the host's I/O interface, decoding on its
# task interface, the CPU cache budget and height queries (vgl/context.h).

target_sources(vgl_objects PRIVATE
    context.cpp
    context_impl.hpp)
