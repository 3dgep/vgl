# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Data providers: tile-keyed access to packs, layering by priority, shard
# lookup. The pack format readers work on bytes; reading them through the
# host's I/O callback is added with the context in milestone M2.

target_sources(vgl_objects PRIVATE
    pack_format.hpp
    pack_reader.cpp
    pack_reader.hpp)
