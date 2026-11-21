#pragma once

#include "CarveCommon.h"
#include "../../disk/SectorReader.h"
#include <string>

namespace CarveImage {

// Structure-aware size for common graphic formats (CFReDS file-carving-images).
CarveResult Carve(SectorReader& reader, uint64_t startOffset, uint64_t maxEnd,
                  const std::wstring& ext);

} // namespace CarveImage
