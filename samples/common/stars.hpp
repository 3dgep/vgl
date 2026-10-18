// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The samples' stars, since vgl ships no star catalogue: a generated star field, a deterministic
// random sky with about as many stars of each magnitude as the real one, down to 6.5, the
// faintest seen by eye, coloured from blue-white to orange, and more of the faint ones towards
// a band where the Milky Way lies, but not the real constellations; or, with --stars, the real
// sky from a copy of the Yale Bright Star Catalogue that the user downloads, which the
// repository does not hold.

#ifndef VGL_SAMPLES_STARS_HPP
#define VGL_SAMPLES_STARS_HPP

#include <vgl/render.h>

#include <string>
#include <vector>

namespace sample
{

// The generated stars, the same on every platform.
std::vector<vgl_star> generated_stars();

// Reads the Yale Bright Star Catalogue, 5th revised edition, in its original text form: the
// file "catalog" of CDS catalogue V/50 (https://cdsarc.cds.unistra.fr/ftp/V/50/catalog.gz,
// decompressed), with the J2000 positions, the visual magnitudes and the B-V colours of its
// 9,110 entries; entries without a position or magnitude are left out. False, with a message
// in error, if the file cannot be read or holds no stars.
bool catalog_stars(const std::string& path, std::vector<vgl_star>& out, std::string& error);

// The stars the samples draw: those of the catalogue that stars_load read, or else the
// generated ones.
const std::vector<vgl_star>& sky_stars();

// Loads a catalogue for sky_stars (catalog_stars).
bool stars_load(const std::string& path, std::string& error);

// The Earth rotation angle in radians for a time of day in hours (UTC) on a day of the year,
// from 1 on 1 January, in a year (vgl_earth_rotation_angle_get).
double sidereal_angle(float time_of_day, int day_of_year, int year);

// The system clock's time now, in UTC: the time of day in hours, the day of the year from 1 on
// 1 January, and the year.
void current_time_get(float& time_of_day, int& day_of_year, int& year);

} // namespace sample

#endif // VGL_SAMPLES_STARS_HPP
