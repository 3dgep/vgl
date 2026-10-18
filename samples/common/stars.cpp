// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten

#include "stars.hpp"

#include <vgl/geodesy.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <numbers>

namespace sample
{

namespace
{

constexpr double pi = std::numbers::pi;

// SplitMix64: the same numbers on every platform, which the standard distributions are not.
class random
{
public:
    explicit random(std::uint64_t seed) : state_(seed) {}

    // Uniform in [0, 1).
    double next()
    {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30u)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27u)) * 0x94D049BB133111EBull;
        z ^= z >> 31u;
        return static_cast<double>(z >> 11u) * (1.0 / 9007199254740992.0);
    }

private:
    std::uint64_t state_;
};

// The colour of a star of a B-V colour index, as linear RGB with the brightest channel at 1:
// its temperature (Ballesteros, 2012), then a blackbody's colour from a few stops, softened
// towards white, as the eye sees stars.
std::array<float, 3> star_color(double bv)
{
    const double t = 4600.0 * (1.0 / (0.92 * bv + 1.7) + 1.0 / (0.92 * bv + 0.62));
    struct stop
    {
        double kelvin;
        std::array<double, 3> rgb;
    };
    constexpr std::array<stop, 7> stops = {{{3000.0, {1.0, 0.60, 0.35}},
                                            {4000.0, {1.0, 0.75, 0.55}},
                                            {5000.0, {1.0, 0.87, 0.75}},
                                            {6000.0, {1.0, 0.95, 0.90}},
                                            {7000.0, {0.95, 0.95, 1.0}},
                                            {10000.0, {0.75, 0.82, 1.0}},
                                            {20000.0, {0.65, 0.73, 1.0}}}};
    std::array<double, 3> c = stops.front().rgb;
    for (std::size_t k = 1; k < stops.size(); ++k)
    {
        const double f = std::clamp(
            (t - stops[k - 1].kelvin) / (stops[k].kelvin - stops[k - 1].kelvin), 0.0, 1.0);
        for (std::size_t i = 0; i < 3; ++i)
        {
            c[i] += (stops[k].rgb[i] - c[i]) * f;
        }
    }
    std::array<float, 3> out{};
    for (std::size_t i = 0; i < 3; ++i)
    {
        out[i] = static_cast<float>(0.6 * c[i] + 0.4);
    }
    return out;
}

} // namespace

std::vector<vgl_star> generated_stars()
{
    // About as many stars brighter than a magnitude m as the sky has: 10^(0.5 m + 0.68), from
    // one at -1.4, like Sirius, to 8,800 at 6.5.
    constexpr int count = 8800;
    // The galactic north pole in the celestial frame (RA 192.86, Dec 27.13 degrees).
    const double ra = 192.85948 * pi / 180.0;
    const double dec = 27.12825 * pi / 180.0;
    const std::array<double, 3> pole = {
        std::cos(dec) * std::cos(ra), std::cos(dec) * std::sin(ra), std::sin(dec)};

    random rng(0x76676C2D73746172ull); // "vgl-star"
    std::vector<vgl_star> stars;
    stars.reserve(count);
    for (int i = 0; i < count; ++i)
    {
        vgl_star s{};
        s.magnitude = static_cast<float>(2.0 * (std::log10(i + 1.0) - 0.68));
        // A direction, uniform over the sphere; fainter stars are kept more often near the
        // galactic plane, where the Milky Way's stars crowd.
        std::array<double, 3> d{};
        for (;;)
        {
            const double z = 2.0 * rng.next() - 1.0;
            const double phi = 2.0 * pi * rng.next();
            const double r = std::sqrt(std::max(1.0 - z * z, 0.0));
            d = {r * std::cos(phi), r * std::sin(phi), z};
            const double b =
                std::asin(std::clamp(d[0] * pole[0] + d[1] * pole[1] + d[2] * pole[2], -1.0, 1.0));
            const double crowd =
                s.magnitude < 3.0 ? 1.0 : 0.35 + 0.65 * std::exp(-(b / 0.3) * (b / 0.3));
            if (rng.next() < crowd)
            {
                break;
            }
        }
        for (std::size_t k = 0; k < 3; ++k)
        {
            s.direction[k] = static_cast<float>(d[k]);
        }
        // Colour indices: a third blue-white, a third white to yellow, a third orange.
        const double pick = rng.next();
        const double u = rng.next();
        double bv = 0.9 + 0.7 * u;
        if (pick < 0.33)
        {
            bv = -0.2 + 0.4 * u;
        }
        else if (pick < 0.66)
        {
            bv = 0.3 + 0.6 * u;
        }
        const std::array<float, 3> c = star_color(bv);
        std::copy(c.begin(), c.end(), &s.color[0]);
        stars.push_back(s);
    }
    return stars;
}

namespace
{

// The loaded catalogue's stars, once loaded.
std::vector<vgl_star>& loaded()
{
    static std::vector<vgl_star> stars;
    return stars;
}

// A fixed-width field of a catalogue line, from column first to last, counting from 1: its
// number, or false if it is blank.
bool field(const std::string& line, std::size_t first, std::size_t last, double& out)
{
    if (line.size() < last)
    {
        return false;
    }
    const std::string text = line.substr(first - 1, last - first + 1);
    if (text.find_first_not_of(' ') == std::string::npos)
    {
        return false;
    }
    char* end = nullptr;
    out = std::strtod(text.c_str(), &end);
    return end != text.c_str();
}

} // namespace

bool catalog_stars(const std::string& path, std::vector<vgl_star>& out, std::string& error)
{
    std::ifstream in(path);
    if (!in)
    {
        error = "cannot open " + path;
        return false;
    }
    out.clear();
    std::string line;
    while (std::getline(in, line))
    {
        // Byte-by-byte description in the catalogue's ReadMe: RA h m s in columns 76 to 83,
        // the sign of Dec in 84, Dec d m s in 85 to 90, V in 103 to 107, B-V in 110 to 114.
        double rh = 0.0;
        double rm = 0.0;
        double rs = 0.0;
        double dd = 0.0;
        double dm = 0.0;
        double ds = 0.0;
        double v = 0.0;
        if (!field(line, 76, 77, rh) || !field(line, 78, 79, rm) || !field(line, 80, 83, rs) ||
            !field(line, 85, 86, dd) || !field(line, 87, 88, dm) || !field(line, 89, 90, ds) ||
            !field(line, 103, 107, v))
        {
            continue;
        }
        double bv = 0.6; // Without a colour, about the Sun's.
        static_cast<void>(field(line, 110, 114, bv));
        const double ra = (rh + rm / 60.0 + rs / 3600.0) * 15.0 * pi / 180.0;
        double dec = (dd + dm / 60.0 + ds / 3600.0) * pi / 180.0;
        if (line[83] == '-')
        {
            dec = -dec;
        }
        vgl_star s{};
        s.direction[0] = static_cast<float>(std::cos(dec) * std::cos(ra));
        s.direction[1] = static_cast<float>(std::cos(dec) * std::sin(ra));
        s.direction[2] = static_cast<float>(std::sin(dec));
        s.magnitude = static_cast<float>(v);
        const std::array<float, 3> c = star_color(std::clamp(bv, -0.4, 2.0));
        std::copy(c.begin(), c.end(), &s.color[0]);
        out.push_back(s);
    }
    if (out.empty())
    {
        error = path + " holds no stars in the Yale Bright Star Catalogue's format";
        return false;
    }
    return true;
}

const std::vector<vgl_star>& sky_stars()
{
    static const std::vector<vgl_star> generated = generated_stars();
    return loaded().empty() ? generated : loaded();
}

bool stars_load(const std::string& path, std::string& error)
{
    return catalog_stars(path, loaded(), error);
}

double earth_rotation_angle(float time_of_day, int day_of_year, int year)
{
    // The Julian date of 1 January of the year at 00:00, from its days since 1 January 1970,
    // which began at Julian date 2440587.5.
    using namespace std::chrono;
    const sys_days first{std::chrono::year{year} / January / 1};
    const double julian_date = 2440587.5 + static_cast<double>(first.time_since_epoch().count()) +
                               (day_of_year - 1) + time_of_day / 24.0;
    double angle = 0.0;
    static_cast<void>(vgl_earth_rotation_angle_get(julian_date, &angle));
    return angle;
}

void current_time_get(float& time_of_day, int& day_of_year, int& year)
{
    using namespace std::chrono;
    const system_clock::time_point now = system_clock::now();
    const sys_days today = floor<days>(now);
    const year_month_day date{today};
    const sys_days first{date.year() / January / 1};
    day_of_year = static_cast<int>((today - first).count()) + 1;
    year = static_cast<int>(date.year());
    time_of_day = duration<float, std::ratio<3600>>(now - today).count();
}

} // namespace sample
