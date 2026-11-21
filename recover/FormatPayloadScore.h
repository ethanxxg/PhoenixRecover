#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// How well a recovered byte buffer matches its declared container format.
// Used to rank candidate FAT cluster chains without golden hashes.
enum class FormatPayloadScore : int {
    Invalid  = -1, // Wrong magic / broken structure
    Weak     = 0,  // Unknown or insufficient data
    Partial  = 1,  // Header/structure plausible (file may open partially)
    Complete = 2,  // End marker or size field indicates a whole file
};

namespace FormatPayloadScoreUtil {

// Score payload bytes for a known extension (lower-case ext without dot).
FormatPayloadScore Score(const std::wstring& extensionLower,
                         const uint8_t* data,
                         size_t length);

// True if we attempt format-aware FAT fragment stitching for this extension.
bool SupportsFragmentStitch(const std::wstring& extensionLower);

// Infer extension from magic at the start of the buffer (lower-case, no dot).
std::wstring ProbeExtension(const uint8_t* data, size_t length);

} // namespace FormatPayloadScoreUtil
