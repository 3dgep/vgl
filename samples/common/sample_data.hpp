// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// Where the C++ samples find their packs. With --pack, the packs named, in the order given, each
// winning over the ones before it where they overlap. Otherwise the sample set
// (samples/data/README.md) in a directory: the one named with --data, or else the one in the
// environment variable VGL_SAMPLE_DATA, or else the source tree's samples/data, built in.

#ifndef VGL_SAMPLES_SAMPLE_DATA_HPP
#define VGL_SAMPLES_SAMPLE_DATA_HPP

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace sample
{

// The environment variable that names the directory of the sample set.
inline constexpr const char* data_variable = "VGL_SAMPLE_DATA";

// A pack to add, with its priority (vgl_pack_create_info::priority).
struct pack_source
{
    std::string uri;
    std::int32_t priority = 0;
};

// The value of an environment variable, or an empty string if it is not set.
inline std::string environment(const char* name)
{
#ifdef _MSC_VER
    // MSVC deprecates std::getenv in favour of _dupenv_s.
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
    {
        return {};
    }
    std::string s(value);
    std::free(value); // NOLINT(cppcoreguidelines-no-malloc): _dupenv_s allocates with malloc.
    return s;
#else
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read at start-up.
    return value != nullptr ? value : "";
#endif
}

// The directory of the sample set: the one given with --data, or else VGL_SAMPLE_DATA, or else
// the built-in one.
inline std::string data_directory(const std::string& given, const char* built_in)
{
    if (!given.empty())
    {
        return given;
    }
    std::string from_environment = environment(data_variable);
    return from_environment.empty() ? built_in : from_environment;
}

// The sample set in a directory: GEBCO, the GLO-30 detail above it, the geoid and Blue Marble.
inline std::vector<pack_source> sample_packs(const std::string& directory)
{
    return {{directory + "/packs/gebco", 0},
            {directory + "/packs/glo30-sognefjord", 1},
            {directory + "/packs/egm2008", 0},
            {directory + "/packs/bluemarble", 0}};
}

// The packs named with --pack, all at one priority, so that each wins over the ones before it
// (vgl_pack_create_info::priority).
inline std::vector<pack_source> named_packs(const std::vector<std::string>& uris)
{
    std::vector<pack_source> sources;
    sources.reserve(uris.size());
    for (const std::string& uri : uris)
    {
        sources.push_back({uri, 0});
    }
    return sources;
}

} // namespace sample

#endif // VGL_SAMPLES_SAMPLE_DATA_HPP
