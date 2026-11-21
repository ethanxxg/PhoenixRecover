#pragma once

#include <cstdint>
#include <vector>

namespace FragmentNtfsStructuredFixture {

enum class StructuredFormat { Jpeg };

enum class NtfsChainMode {
    Intact,
    BrokenBraid,
};

constexpr uint32_t kStructuredFileSize = 1200;

void BuildNtfsStructuredVolume(std::vector<uint8_t>& image,
                               StructuredFormat format,
                               NtfsChainMode mode);

const wchar_t* FormatExtension(StructuredFormat format);

} // namespace FragmentNtfsStructuredFixture
