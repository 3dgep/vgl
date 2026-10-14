# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Jeremiah van Oosten

# Geodesy: parameterized ellipsoid (WGS84 by default), coordinate conversions,
# bounding volumes. The geoid model is not implemented yet.

target_sources(vgl_objects PRIVATE
    bounds.cpp
    bounds.hpp
    ellipsoid.cpp
    ellipsoid.hpp
    geodesy_api.cpp)
