#include "CarveMp4.h"
#include <vector>
#include <cstring>
#include <algorithm>

namespace {
inline uint32_t BE32(const uint8_t* p) { return (uint32_t)((p[0]<<24) | (p[1]<<16) | (p[2]<<8) | p[3]); }
inline uint64_t BE64(const uint8_t* p) {
    uint64_t hi = BE32(p);
    uint64_t lo = BE32(p + 4);
    return (hi << 32) | lo;
}
static bool IsTag(const uint8_t* p, const char* t) { return std::memcmp(p, t, 4) == 0; }
}

CarveResult CarveMp4::Carve(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset)
{
    CarveResult r{};
    if (maxEndOffset <= startOffset + 16) {
        r.reason = L"cap too small";
        return r;
    }

    uint8_t head[16]{};
    if (!reader.ReadBytes(startOffset, head, sizeof(head))) {
        r.reason = L"head read failed";
        return r;
    }

    uint32_t size0 = BE32(head);
    if (size0 < 8) {
        r.reason = L"invalid first box size";
        return r;
    }

    int conf = 55;
    if (IsTag(head + 4, "ftyp"))
        conf += 15;
    else if (IsTag(head + 4, "moov"))
        conf += 12;
    else if (size0 == 8 && IsTag(head + 8, "wide"))
        conf += 8;
    else
        conf += 5;

    uint64_t pos      = startOffset;
    bool seenMoov     = false;
    bool seenMdat     = false;
    bool seenFtyp     = false;

    auto readBoxHeader = [&](uint64_t at, uint8_t out[16]) -> bool {
        return reader.ReadBytes(at, out, 16);
    };

    while (pos + 8 <= maxEndOffset) {
        uint8_t h[16]{};
        if (!readBoxHeader(pos, h))
            break;

        uint64_t boxSize = BE32(h);
        const uint8_t* type = h + 4;
        if (boxSize == 1) {
            if (pos + 16 > maxEndOffset)
                break;
            boxSize = BE64(h + 8);
        } else if (boxSize == 0) {
            boxSize = maxEndOffset - pos;
        }

        if (boxSize < 8)
            break;
        if (pos + boxSize > maxEndOffset) {
            pos = maxEndOffset;
            break;
        }

        if (IsTag(type, "ftyp"))
            seenFtyp = true;
        if (IsTag(type, "moov"))
            seenMoov = true;
        if (IsTag(type, "mdat"))
            seenMdat = true;

        pos += boxSize;

        if (seenMoov && seenMdat && (pos - startOffset) > 256 * 1024)
            break;
    }

    bool tailMoov = false;
    if (!seenMoov && maxEndOffset > startOffset + 1024 * 1024) {
        uint64_t tailSize = (std::min<uint64_t>)(maxEndOffset - startOffset, 4ull * 1024 * 1024);
        uint64_t tailOff  = maxEndOffset - tailSize;
        std::vector<uint8_t> tail((size_t)tailSize);
        if (reader.ReadBytes(tailOff, tail.data(), (uint32_t)tail.size())) {
            for (size_t i = 0; i + 16 <= tail.size(); ++i) {
                uint32_t sz = BE32(tail.data() + i);
                const uint8_t* tp = tail.data() + i + 4;
                if (sz >= 8 && IsTag(tp, "moov")) {
                    uint64_t moovAbs = tailOff + i;
                    uint64_t moovEnd = moovAbs + sz;
                    if (moovEnd <= maxEndOffset) {
                        tailMoov = true;
                        if (moovEnd > pos && moovEnd <= maxEndOffset)
                            pos = moovEnd;
                        break;
                    }
                }
            }
        }
    }

    if (seenFtyp)
        conf += 5;
    if (seenMoov || tailMoov)
        conf += 20;
    if (seenMdat)
        conf += 10;
    if ((seenMoov || tailMoov) && seenMdat)
        conf += 5;

    if (conf < 0)
        conf = 0;
    if (conf > 100)
        conf = 100;

    r.ok         = (pos > startOffset);
    r.endOffset  = pos;
    r.confidence = conf;
    if (seenMoov)
        r.reason = L"box chain ok (moov)";
    else if (tailMoov)
        r.reason = L"box chain ok (tail moov)";
    else
        r.reason = L"box chain ok";
    return r;
}

