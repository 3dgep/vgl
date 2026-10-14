# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Decoders: elevation and image decoding, pluggable so a host can substitute its
# own. The elevation codec is shared with the baker's encoder. Image decoding
# (stb_image) is added with imagery in milestone M2.

target_sources(vgl_objects PRIVATE
    elevation_codec.hpp
    zstd_decompressor.cpp
    zstd_decompressor.hpp)
