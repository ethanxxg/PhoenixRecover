#pragma once

#include <cstdint>
#include <vector>

// Synthetic FAT16 volumes with deleted structured files (jpg/png/pdf/zip).
namespace FragmentFatStructuredFixture {

enum class StructuredFormat { Jpeg, Png, Pdf, Zip };

enum class FatChainMode {
    Intact,
    BrokenWithDecoy,
};

constexpr uint32_t kStructuredFileSize = 600;

void BuildFat16StructuredVolume(std::vector<uint8_t>& image,
                                StructuredFormat format,
                                FatChainMode mode);

const wchar_t* FormatExtension(StructuredFormat format);

} // namespace FragmentFatStructuredFixture
