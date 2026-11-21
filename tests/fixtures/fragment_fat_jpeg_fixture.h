#pragma once

#include "fragment_fat_structured_fixture.h"

#include <vector>

// Backward-compatible wrappers around FragmentFatStructuredFixture.
namespace FragmentFatJpegFixture {

using FatChainMode = FragmentFatStructuredFixture::FatChainMode;

inline void BuildFat16JpegVolume(std::vector<uint8_t>& image, FatChainMode mode)
{
    FragmentFatStructuredFixture::BuildFat16StructuredVolume(
        image, FragmentFatStructuredFixture::StructuredFormat::Jpeg, mode);
}

} // namespace FragmentFatJpegFixture
