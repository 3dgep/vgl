// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jeremiah van Oosten
//
// The inspect and validate commands (design document, section 8.2).

#ifndef VGL_BAKER_INSPECT_HPP
#define VGL_BAKER_INSPECT_HPP

#include <cstdint>
#include <filesystem>
#include <ostream>

namespace vgl::bake
{

// Prints a pack's manifest and per-level tile counts and sizes.
void inspect(const std::filesystem::path& pack_directory, std::ostream& out);

struct validation_report
{
    std::uint64_t tiles = 0;
    std::uint64_t border_mismatches = 0;      // Shared border samples that differ (section 2.3).
    std::uint64_t pole_mismatches = 0;        // Pole rows that are not uniform.
    std::uint64_t child_mask_mismatches = 0;  // Child mask bits that disagree with the pack.
    std::uint64_t child_bound_violations = 0; // Children outside their parent's recorded bounds.
    std::uint64_t image_errors = 0;           // Imagery that does not decode to its size.

    [[nodiscard]] bool ok() const noexcept
    {
        return border_mismatches == 0 && pole_mismatches == 0 && child_mask_mismatches == 0 &&
               child_bound_violations == 0 && image_errors == 0;
    }
};

// Reads and decodes every tile, which checks every CRC and structure, and checks the
// properties the format requires of the data: for heights, shared borders, poles, child masks
// and child bounds; for imagery, image sizes, child masks, and that border pixels repeat their
// neighbours' pixels (within the losses of JPEG). Prints problems and a summary to out.
// Throws std::runtime_error if the pack cannot be read at all.
validation_report validate(const std::filesystem::path& pack_directory, std::ostream& out);

} // namespace vgl::bake

#endif // VGL_BAKER_INSPECT_HPP
