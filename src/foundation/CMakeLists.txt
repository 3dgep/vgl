# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Foundation: allocator hooks, logging callback, double-precision math. The task
# and I/O callback interfaces are added with the context (milestone M2).

target_sources(vgl_objects PRIVATE
    allocator.cpp
    allocator.hpp
    buffer.hpp
    byte_reader.hpp
    crc32c.cpp
    crc32c.hpp
    log.cpp
    log.hpp
    math.hpp
    result.cpp
    version.cpp)
