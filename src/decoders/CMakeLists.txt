# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Decoders: elevation and image decoding, pluggable so a host can substitute its
# own. The elevation codec is shared with the baker's encoder. Imagery decodes
# with stb_image, whose implementation is the object library vgl_stb_objects
# (cmake/vgl_dependencies.cmake), compiled from stb_image.c here.

target_sources(vgl_objects PRIVATE
    elevation_codec.hpp
    image_decoder.cpp
    image_decoder.hpp
    zstd_decompressor.cpp
    zstd_decompressor.hpp)
