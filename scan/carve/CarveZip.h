#pragma once

#include "CarveCommon.h"
#include "../../disk/SectorReader.h"

class CarveZip
{
public:
    // Determine ZIP end via EOCD (PK 05 06) and validate central directory.
    // maxEndOffset: hard cap (exclusive).
    static CarveResult Carve(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset);
};

