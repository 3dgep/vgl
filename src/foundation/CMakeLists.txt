# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Foundation: allocator hooks, logging callback, double-precision math, and the
# task and I/O callback interfaces with the queue their completions go through.

target_sources(vgl_objects PRIVATE
    allocator.cpp
    allocator.hpp
    async.cpp
    async.hpp
    buffer.hpp
    byte_reader.hpp
    containers.hpp
    crc32c.cpp
    crc32c.hpp
    log.cpp
    log.hpp
    math.hpp
    result.cpp
    version.cpp)
