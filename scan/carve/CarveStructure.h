#pragma once

#include "CarveCommon.h"
#include "../../disk/SectorReader.h"

// Structure-aware carving for common DFTT / PhotoRec formats (CFB, RIFF, ASF, GIF).
class CarveStructure
{
public:
    static CarveResult CarveCfb(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset);
    static CarveResult CarveRiff(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset);
    static CarveResult CarveAsf(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset);
    static CarveResult CarveGif(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset);
    static CarveResult CarvePdf(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset);
};
