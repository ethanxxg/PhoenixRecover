#pragma once

#include "CarveCommon.h"
#include "../../disk/SectorReader.h"

class CarveMp4
{
public:
    // Parse ISO-BMFF boxes sequentially to estimate file end and confidence.
    // Works for MP4/M4V/MOV/3GP-like containers that start with size+ftyp.
    static CarveResult Carve(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset);
};

