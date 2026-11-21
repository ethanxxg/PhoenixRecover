#pragma once

// ---------------------------------------------------------------------------
// VirtualRaid -- A1-4a read-only virtual RAID reassembly.
//
// WHAT THIS IS
//   A pure-logic layer that maps a virtual LBA onto one of N member byte
//   sources and rebuilds missing members from parity. It implements the
//   existing phoenix::IVirtualDiskReader contract, so a member may be a raw
//   image, a VirtualDiskSource-backed container (VHD/VHDX/VMDK/...), an
//   in-memory fixture, or ANOTHER VirtualRaid (nested RAID50 / RAID60).
//
// STRICTLY READ-ONLY
//   Members are held as `const IVirtualDiskReader*`. There is no write method,
//   no file handle, no Qt, no Win32, and no device I/O in this header. A
//   failed member is NEVER silently zero-filled: unrecoverable reads return
//   false so callers (SectorReader) can apply their own bad-sector policy.
//
// SUPPORTED
//   JBOD, RAID0, RAID1, RAID4, RAID5, RAID6, RAID10 (near=2 layout).
//   RAID5 layouts follow Linux md raid5.c `raid5_compute_sector`:
//     left  -> pd_idx = (N-1) - (stripe mod N)
//     right -> pd_idx = (stripe mod N)
//     symmetric   -> data disk = (pd_idx + 1 + dd_idx) mod N
//     asymmetric  -> data disk = dd_idx >= pd_idx ? dd_idx+1 : dd_idx
//     parity-first / parity-last -> fixed parity disk (also used for RAID4).
//   RAID6 uses P = XOR(D_i) and Q = XOR(g^i * D_i) over GF(2^8) with the
//   Linux lib/raid6 polynomial 0x11D and generator g = 0x02; coefficients
//   are indexed by the LOGICAL data-chunk number inside the stripe, not by
//   the physical member number. P/Q rotate together as an adjacent pair.
//   Single-failure rebuild uses P; two-failure rebuild solves the 2x2
//   system over GF(2^8). Missing both parity disks does not hurt data reads.
//
// DELIBERATELY NOT CLAIMED (see ROADMAP A1-4b)
//   * NO automatic geometry/order/chunk/rotation discovery. The caller must
//     supply a RaidConfig (or run its own probe on top of VerifyStripe()).
//   * NO on-disk metadata parsing (mdadm superblock, SNIA DDF, Intel RIS,
//     LVM2), NO vendor-specific layouts (HP SmartArray delayed parity,
//     Promise wide-pace Q, ZFS RAIDZ, far/offset RAID10), NO 4Kn members.
//   * NO compatibility claim against real hardware arrays until real-array
//     golden images are added. Synthetic fixtures prove the arithmetic is
//     self-consistent; they do NOT prove controller compatibility.
//   A wrong config yields wrong bytes, not a clear error. VerifyStripe() and
//   a known-good first sector (MBR/GPT/NTFS boot sector) are the intended
//   guards; exposing this in a UI without them is unsafe.
//
// Header-only on purpose: no new .cpp, no root CMakeLists.txt change.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "VirtualDiskSource.h"   // phoenix::IVirtualDiskReader

namespace phoenix {

constexpr uint32_t kRaidMaxMembers   = 32;
constexpr uint32_t kRaidSectorBytes  = 512;
constexpr uint32_t kRaidMaxChunk     = 64u * 1024u * 1024u;

enum class RaidLevel : uint8_t
{
    Jbod, Raid0, Raid1, Raid4, Raid5, Raid6, Raid10
};

enum class RaidLayout : uint8_t
{
    LeftSymmetric,
    LeftAsymmetric,
    RightSymmetric,
    RightAsymmetric,
    ParityFirst,
    ParityLast
};

enum class RaidOpenResult : uint8_t
{
    Ok,
    EmptyMembers,
    TooFewMembers,
    TooManyMembers,
    BadChunk,
    BadDataOffset,
    BadMemberSize,
    MissingReader,
    TooManyMissing,
    UnsupportedLayout,
    SizeMismatch,
    BadConfig,
    NotOpen,
    OutOfRange
};

inline const char* RaidLevelName(RaidLevel v)
{
    switch (v) {
    case RaidLevel::Jbod:  return "JBOD/SPAN";
    case RaidLevel::Raid0: return "RAID 0";
    case RaidLevel::Raid1: return "RAID 1";
    case RaidLevel::Raid4: return "RAID 4";
    case RaidLevel::Raid5: return "RAID 5";
    case RaidLevel::Raid6: return "RAID 6";
    case RaidLevel::Raid10: return "RAID 10";
    }
    return "unknown";
}

inline const char* RaidLayoutName(RaidLayout v)
{
    switch (v) {
    case RaidLayout::LeftSymmetric:    return "left-symmetric";
    case RaidLayout::LeftAsymmetric:   return "left-asymmetric";
    case RaidLayout::RightSymmetric:   return "right-symmetric";
    case RaidLayout::RightAsymmetric:  return "right-asymmetric";
    case RaidLayout::ParityFirst:      return "parity-first";
    case RaidLayout::ParityLast:       return "parity-last";
    }
    return "unknown";
}

inline const char* RaidOpenDetail(RaidOpenResult r)
{
    switch (r) {
    case RaidOpenResult::Ok:                return "ok";
    case RaidOpenResult::EmptyMembers:      return "member list is empty";
    case RaidOpenResult::TooFewMembers:     return "too few members for this level";
    case RaidOpenResult::TooManyMembers:    return "more than 32 members";
    case RaidOpenResult::BadChunk:          return "chunk size must be a multiple of 512 and <= 64 MiB";
    case RaidOpenResult::BadDataOffset:     return "data offset must be sector aligned";
    case RaidOpenResult::BadMemberSize:     return "member too small for data offset + member bytes";
    case RaidOpenResult::MissingReader:     return "a non-degraded member has no reader";
    case RaidOpenResult::TooManyMissing:    return "too many missing members for this level";
    case RaidOpenResult::UnsupportedLayout: return "layout not defined for this RAID level";
    case RaidOpenResult::SizeMismatch:      return "explicit memberBytes is not chunk aligned";
    case RaidOpenResult::BadConfig:         return "invalid configuration";
    case RaidOpenResult::NotOpen:           return "array is not open";
    case RaidOpenResult::OutOfRange:        return "read outside virtual volume";
    }
    return "unknown";
}

struct RaidConfig
{
    RaidLevel   level       = RaidLevel::Raid5;
    RaidLayout  layout      = RaidLayout::LeftSymmetric;
    uint32_t    members     = 0;              // must match the reader vector
    uint32_t    chunkBytes  = 64 * 1024;      // per-member chunk
    uint64_t    dataOffset  = 0;              // per-member start offset (bytes)
    uint64_t    memberBytes = 0;              // 0 = derive from smallest reader
    std::vector<uint32_t> missing;            // degraded member indices
};

struct RaidInfo
{
    RaidLevel   level       = RaidLevel::Jbod;
    RaidLayout  layout      = RaidLayout::LeftSymmetric;
    uint32_t    members     = 0;
    uint32_t    dataDisks   = 0;
    uint32_t    parityDisks = 0;
    uint32_t    chunkBytes  = 0;
    uint64_t    dataOffset  = 0;
    uint64_t    memberBytes = 0;
    uint64_t    stripes     = 0;
    uint64_t    virtualSize = 0;
    uint32_t    missing     = 0;
    std::string detail;
};

enum class RaidVerifyResult : uint8_t
{
    Ok,
    NotApplicable,
    MismatchP,
    MismatchQ,
    ReadFailed,
    Partial,
    BadStripe,
    NotOpen
};

struct RaidVerifyStats
{
    uint64_t sectorsChecked = 0;
    uint32_t mismatchP      = 0;
    uint32_t mismatchQ      = 0;
    uint32_t unreadable     = 0;
};

namespace raid_detail {

struct GfTables
{
    uint8_t exp[512];
    uint8_t log[256];

    GfTables()
    {
        std::memset(exp, 0, sizeof(exp));
        std::memset(log, 0, sizeof(log));
        uint32_t x = 1;
        for (uint32_t i = 0; i < 255; ++i) {
            exp[i] = static_cast<uint8_t>(x);
            log[x] = static_cast<uint8_t>(i);
            x <<= 1;
            if (x & 0x100u) x ^= 0x11Du;   // x^8+x^4+x^3+x^2+1
        }
        for (uint32_t i = 255; i < 512; ++i) exp[i] = exp[i - 255];
    }
};

inline const GfTables& Gf()
{
    static const GfTables t;
    return t;
}

inline uint8_t GfMul(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) return 0;
    const GfTables& t = Gf();
    return t.exp[static_cast<uint32_t>(t.log[a]) + t.log[b]];
}

inline uint8_t GfDiv(uint8_t a, uint8_t b)
{
    if (a == 0 || b == 0) return 0;
    const GfTables& t = Gf();
    int e = static_cast<int>(t.log[a]) - static_cast<int>(t.log[b]);
    if (e < 0) e += 255;
    return t.exp[static_cast<uint32_t>(e)];
}

// g^k, k is the logical data-chunk index inside a stripe.
inline uint8_t GfPow(uint32_t k)
{
    return Gf().exp[k % 255u];
}

inline void XorInto(uint8_t* dst, const uint8_t* src, size_t n)
{
    for (size_t i = 0; i < n; ++i) dst[i] ^= src[i];
}

inline void GfMulInto(uint8_t* dst, uint8_t coef,
                      const uint8_t* src, size_t n)
{
    if (coef == 1) { XorInto(dst, src, n); return; }
    for (size_t i = 0; i < n; ++i) dst[i] ^= GfMul(coef, src[i]);
}

inline void GfScale(uint8_t* buf, uint8_t coef, size_t n)
{
    if (coef == 1) return;
    for (size_t i = 0; i < n; ++i) buf[i] = GfMul(coef, buf[i]);
}

inline void GfUnscale(uint8_t* buf, uint8_t coef, size_t n)
{
    if (coef == 1) return;
    for (size_t i = 0; i < n; ++i) buf[i] = GfDiv(buf[i], coef);
}

} // namespace raid_detail

class VirtualRaid : public IVirtualDiskReader
{
public:
    VirtualRaid() = default;

    RaidOpenResult Open(const RaidConfig& cfg,
                        const std::vector<const IVirtualDiskReader*>& members)
    {
        Reset();
        m_cfg     = cfg;
        m_members = members;

        if (members.empty())                     return Fail(RaidOpenResult::EmptyMembers);
        if (members.size() > kRaidMaxMembers)    return Fail(RaidOpenResult::TooManyMembers);
        const uint32_t n = static_cast<uint32_t>(members.size());
        m_cfg.members = n;

        if (!ValidateLevel(n))                   return Fail(RaidOpenResult::TooFewMembers);
        if (!ValidateLayout())                   return Fail(RaidOpenResult::UnsupportedLayout);
        if (cfg.chunkBytes < kRaidSectorBytes ||
            cfg.chunkBytes > kRaidMaxChunk ||
            (cfg.chunkBytes % kRaidSectorBytes) != 0)
                                                 return Fail(RaidOpenResult::BadChunk);
        if ((cfg.dataOffset % kRaidSectorBytes) != 0)
                                                 return Fail(RaidOpenResult::BadDataOffset);

        m_missing.assign(n, 0);
        for (uint32_t idx : cfg.missing) {
            if (idx >= n)                        return Fail(RaidOpenResult::BadConfig);
            if (m_missing[idx])                  return Fail(RaidOpenResult::BadConfig);
            m_missing[idx] = 1;
        }
        const uint32_t missingCount = static_cast<uint32_t>(cfg.missing.size());
        if (!ValidateMissing(n, missingCount))  return Fail(RaidOpenResult::TooManyMissing);

        const uint64_t unbounded = (std::numeric_limits<uint64_t>::max)();
        uint64_t smallest = unbounded;
        for (uint32_t i = 0; i < n; ++i) {
            if (m_missing[i]) continue;
            if (members[i] == nullptr)           return Fail(RaidOpenResult::MissingReader);
            const uint64_t sz = members[i]->Size();
            if (sz <= cfg.dataOffset)            return Fail(RaidOpenResult::BadMemberSize);
            smallest = std::min(smallest, sz - cfg.dataOffset);
        }
        if (smallest == unbounded)               return Fail(RaidOpenResult::BadConfig);

        const uint64_t align = (cfg.level == RaidLevel::Jbod)
                               ? kRaidSectorBytes : cfg.chunkBytes;
        if (cfg.memberBytes == 0) {
            m_memberBytes = (smallest / align) * align;
        } else {
            if ((cfg.memberBytes % align) != 0) return Fail(RaidOpenResult::SizeMismatch);
            if (cfg.memberBytes > smallest)      return Fail(RaidOpenResult::BadMemberSize);
            m_memberBytes = cfg.memberBytes;
        }
        if (m_memberBytes == 0)                 return Fail(RaidOpenResult::BadMemberSize);
        m_cfg.memberBytes = m_memberBytes;

        m_stripes = m_memberBytes / cfg.chunkBytes;
        if (cfg.level == RaidLevel::Jbod) m_stripes = m_memberBytes / kRaidSectorBytes;
        if (m_stripes == 0)                      return Fail(RaidOpenResult::BadMemberSize);

        m_dataDisks   = DataDisks(n, cfg.level);
        m_parityDisks = n - m_dataDisks;
        if (cfg.level == RaidLevel::Jbod) {
            m_virtualSize = m_memberBytes * n;
        } else {
            if (m_stripes > (std::numeric_limits<uint64_t>::max)() / m_dataDisks)
                                                 return Fail(RaidOpenResult::BadConfig);
            m_virtualSize = m_stripes * m_dataDisks * cfg.chunkBytes;
        }
        if (m_virtualSize == 0)                  return Fail(RaidOpenResult::BadMemberSize);

        m_open = true;
        m_info.level       = cfg.level;
        m_info.layout      = cfg.layout;
        m_info.members     = n;
        m_info.dataDisks   = m_dataDisks;
        m_info.parityDisks = m_parityDisks;
        m_info.chunkBytes  = cfg.chunkBytes;
        m_info.dataOffset  = cfg.dataOffset;
        m_info.memberBytes = m_memberBytes;
        m_info.stripes     = m_stripes;
        m_info.virtualSize = m_virtualSize;
        m_info.missing     = missingCount;
        m_info.detail      = std::string(RaidLevelName(cfg.level)) + " / " +
                             RaidLayoutName(cfg.layout) + ", " +
                             std::to_string(n) + " members, " +
                             std::to_string(cfg.chunkBytes) + "-byte chunk" +
                             (missingCount ? ", degraded" : "");
        return RaidOpenResult::Ok;
    }

    void Close()
    {
        Reset();
    }

    bool IsOpen()         const { return m_open; }
    bool IsDegraded()     const { return m_info.missing != 0; }
    const RaidInfo& Info() const { return m_info; }
    const RaidConfig& Config() const { return m_cfg; }
    uint32_t MissingCount() const { return m_info.missing; }
    bool IsMissing(uint32_t disk) const
    {
        return disk < m_missing.size() && m_missing[disk] != 0;
    }

    uint64_t Size() const override { return m_open ? m_virtualSize : 0; }

    bool Read(uint64_t offset, void* dst, size_t length) const override
    {
        if (!m_open) return false;
        if (length == 0) return true;
        if (dst == nullptr) return false;
        if (offset > m_virtualSize || length > m_virtualSize - offset) return false;

        uint8_t* out = static_cast<uint8_t*>(dst);
        uint8_t sector[kRaidSectorBytes];
        uint64_t cur = offset;
        size_t left = length;
        while (left > 0) {
            const uint64_t vs = cur / kRaidSectorBytes;
            const size_t   in = static_cast<size_t>(cur % kRaidSectorBytes);
            const size_t take = std::min(left, kRaidSectorBytes - in);
            if (!ReadVirtualSector(vs, sector)) return false;
            std::memcpy(out, sector + in, take);
            out  += take;
            cur  += take;
            left -= take;
        }
        return true;
    }

    // Verify P (and Q for RAID6) for up to `maxSectors` sectors of one stripe.
    // Used to validate a guessed geometry without touching the source disks.
    RaidVerifyResult VerifyStripe(uint64_t stripe, uint32_t maxSectors,
                                  RaidVerifyStats* stats = nullptr) const
    {
        if (!m_open) return RaidVerifyResult::NotOpen;
        if (m_parityDisks == 0) return RaidVerifyResult::NotApplicable;
        if (stripe >= m_stripes) return RaidVerifyResult::BadStripe;
        if (maxSectors == 0) return RaidVerifyResult::Ok;

        const uint32_t n = m_cfg.members;
        const uint64_t chunkSectors = m_cfg.chunkBytes / kRaidSectorBytes;
        const uint64_t sectors = std::min<uint64_t>(maxSectors, chunkSectors);
        RaidVerifyResult worst = RaidVerifyResult::Ok;
        RaidSlot roles[kRaidMaxMembers];
        ComputeRoles(stripe, roles);

        for (uint64_t s = 0; s < sectors; ++s) {
            const uint64_t memberSector = stripe * chunkSectors + s;
            uint8_t blocks[kRaidMaxMembers][kRaidSectorBytes];
            bool complete = true;
            for (uint32_t d = 0; d < n; ++d) {
                if (m_missing[d] || m_members[d] == nullptr) { complete = false; continue; }
                if (!m_members[d]->Read(m_cfg.dataOffset +
                                        memberSector * kRaidSectorBytes,
                                        blocks[d], kRaidSectorBytes)) {
                    complete = false;
                }
            }
            if (stats) ++stats->sectorsChecked;
            if (!complete) {
                if (stats) ++stats->unreadable;
                worst = RaidVerifyResult::Partial;
                continue;
            }
            uint8_t pCalc[kRaidSectorBytes] = {};
            uint8_t qCalc[kRaidSectorBytes] = {};
            for (uint32_t d = 0; d < n; ++d) {
                if (roles[d].role != RaidSlotRole::Data) continue;
                raid_detail::XorInto(pCalc, blocks[d], kRaidSectorBytes);
                raid_detail::GfMulInto(qCalc,
                                       raid_detail::GfPow(roles[d].index),
                                       blocks[d], kRaidSectorBytes);
            }
            for (uint32_t d = 0; d < n; ++d) {
                if (roles[d].role == RaidSlotRole::ParityP) {
                    if (std::memcmp(pCalc, blocks[d], kRaidSectorBytes) != 0) {
                        if (stats) ++stats->mismatchP;
                        return RaidVerifyResult::MismatchP;
                    }
                } else if (roles[d].role == RaidSlotRole::ParityQ) {
                    if (std::memcmp(qCalc, blocks[d], kRaidSectorBytes) != 0) {
                        if (stats) ++stats->mismatchQ;
                        return RaidVerifyResult::MismatchQ;
                    }
                }
            }
        }
        return worst;
    }

private:
    enum class RaidSlotRole : uint8_t { Unused, Data, Mirror, ParityP, ParityQ };

    struct RaidSlot
    {
        RaidSlotRole role  = RaidSlotRole::Unused;
        uint32_t     index = 0;
    };

    struct SectorMap
    {
        uint32_t targetDisk  = 0;
        uint64_t memberSector = 0;
        uint64_t stripe      = 0;
        uint32_t dataIndex   = 0;
    };

    static uint32_t DataDisks(uint32_t n, RaidLevel level)
    {
        switch (level) {
        case RaidLevel::Jbod:                   return 1;
        case RaidLevel::Raid0:                  return n;
        case RaidLevel::Raid1:                  return 1;
        case RaidLevel::Raid4:
        case RaidLevel::Raid5:                  return n - 1;
        case RaidLevel::Raid6:                  return n - 2;
        case RaidLevel::Raid10:                 return n / 2;
        }
        return 0;
    }

    bool ValidateLevel(uint32_t n) const
    {
        switch (m_cfg.level) {
        case RaidLevel::Jbod:                   return n >= 1;
        case RaidLevel::Raid0:
        case RaidLevel::Raid1:                  return n >= 2;
        case RaidLevel::Raid4:
        case RaidLevel::Raid5:                  return n >= 3;
        case RaidLevel::Raid6:                  return n >= 4;
        case RaidLevel::Raid10:                 return n >= 4 && (n % 2) == 0;
        }
        return false;
    }

    bool ValidateLayout() const
    {
        if (m_cfg.level == RaidLevel::Raid4) {
            return m_cfg.layout == RaidLayout::ParityFirst ||
                   m_cfg.layout == RaidLayout::ParityLast;
        }
        if (m_cfg.level == RaidLevel::Raid6) {
            // RAID6 asymmetric dual-parity rotation is not defined here.
            return m_cfg.layout == RaidLayout::LeftSymmetric ||
                   m_cfg.layout == RaidLayout::RightSymmetric ||
                   m_cfg.layout == RaidLayout::ParityFirst ||
                   m_cfg.layout == RaidLayout::ParityLast;
        }
        return true;
    }

    bool ValidateMissing(uint32_t n, uint32_t missingCount) const
    {
        switch (m_cfg.level) {
        case RaidLevel::Jbod:
        case RaidLevel::Raid0:                  return missingCount == 0;
        case RaidLevel::Raid1:                  return missingCount < n;
        case RaidLevel::Raid4:
        case RaidLevel::Raid5:                  return missingCount <= 1;
        case RaidLevel::Raid6:                  return missingCount <= 2;
        case RaidLevel::Raid10: {
            if (missingCount > n / 2) return false;
            for (uint32_t p = 0; p < n / 2; ++p) {
                if (m_missing[2 * p] && m_missing[2 * p + 1]) return false;
            }
            return true;
        }
        }
        return false;
    }

    RaidOpenResult Fail(RaidOpenResult r)
    {
        m_open = false;
        m_info.detail = RaidOpenDetail(r);
        return r;
    }

    void Reset()
    {
        m_open = false;
        m_cfg = RaidConfig();
        m_info = RaidInfo();
        m_members.clear();
        m_missing.clear();
        m_memberBytes = 0;
        m_stripes = 0;
        m_virtualSize = 0;
        m_dataDisks = 0;
        m_parityDisks = 0;
    }

    uint32_t ParityDisk5(uint64_t stripe) const
    {
        const uint32_t n = m_cfg.members;
        const uint32_t rot = static_cast<uint32_t>(stripe % n);
        if (m_cfg.layout == RaidLayout::ParityFirst) return 0;
        if (m_cfg.layout == RaidLayout::ParityLast)  return n - 1;
        const bool left = (m_cfg.layout == RaidLayout::LeftSymmetric ||
                           m_cfg.layout == RaidLayout::LeftAsymmetric);
        return left ? (n - 1u - rot) : rot;
    }

    void ParityDisks6(uint64_t stripe, uint32_t& pDisk, uint32_t& qDisk) const
    {
        const uint32_t n = m_cfg.members;
        if (m_cfg.layout == RaidLayout::ParityFirst) { pDisk = 0;     qDisk = 1;     return; }
        if (m_cfg.layout == RaidLayout::ParityLast)  { pDisk = n - 2; qDisk = n - 1; return; }
        const uint32_t rot = static_cast<uint32_t>(stripe % n);
        if (m_cfg.layout == RaidLayout::LeftSymmetric) {
            pDisk = n - 1u - rot;
            qDisk = (pDisk + n - 1u) % n;
        } else {
            pDisk = rot;
            qDisk = (pDisk + 1u) % n;
        }
    }

    void ComputeRoles(uint64_t stripe, RaidSlot* out) const
    {
        const uint32_t n = m_cfg.members;
        for (uint32_t i = 0; i < n; ++i) out[i] = RaidSlot();

        if (m_cfg.level == RaidLevel::Jbod ||
            m_cfg.level == RaidLevel::Raid0 ||
            m_cfg.level == RaidLevel::Raid1) {
            for (uint32_t i = 0; i < n; ++i) out[i] = {RaidSlotRole::Data, i};
            return;
        }
        if (m_cfg.level == RaidLevel::Raid10) {
            for (uint32_t p = 0; p < n / 2; ++p) {
                out[2 * p]     = {RaidSlotRole::Data,   p};
                out[2 * p + 1] = {RaidSlotRole::Mirror, p};
            }
            return;
        }

        uint32_t pDisk = 0;
        uint32_t qDisk = 0;
        const bool hasQ = (m_cfg.level == RaidLevel::Raid6);
        if (hasQ) {
            ParityDisks6(stripe, pDisk, qDisk);
        } else if (m_cfg.level == RaidLevel::Raid4) {
            pDisk = (m_cfg.layout == RaidLayout::ParityFirst) ? 0 : n - 1;
        } else {
            pDisk = ParityDisk5(stripe);
        }
        out[pDisk] = {RaidSlotRole::ParityP, 0};
        if (hasQ) out[qDisk] = {RaidSlotRole::ParityQ, 0};

        bool symmetric = (m_cfg.layout == RaidLayout::LeftSymmetric ||
                          m_cfg.layout == RaidLayout::RightSymmetric);
        if (m_cfg.level == RaidLevel::Raid4 ||
            m_cfg.layout == RaidLayout::ParityFirst ||
            m_cfg.layout == RaidLayout::ParityLast) {
            symmetric = false;
        }
        const uint32_t start = symmetric ? ((pDisk + 1u) % n) : 0;
        uint32_t k = 0;
        for (uint32_t s = 0; s < n; ++s) {
            const uint32_t d = (start + s) % n;
            if (d == pDisk || (hasQ && d == qDisk)) continue;
            out[d] = {RaidSlotRole::Data, k++};
        }
    }

    bool MapSector(uint64_t vs, SectorMap& map) const
    {
        const uint32_t n = m_cfg.members;
        const uint64_t chunkSectors = m_cfg.chunkBytes / kRaidSectorBytes;

        if (m_cfg.level == RaidLevel::Jbod) {
            const uint64_t memberSectors = m_memberBytes / kRaidSectorBytes;
            map.targetDisk   = static_cast<uint32_t>(vs / memberSectors);
            map.memberSector = vs % memberSectors;
            map.stripe       = map.memberSector;
            map.dataIndex    = 0;
            return map.targetDisk < n;
        }
        if (m_cfg.level == RaidLevel::Raid1) {
            map.targetDisk   = FirstAvailableDisk();
            map.memberSector = vs;
            map.stripe       = vs / chunkSectors;
            map.dataIndex    = 0;
            return map.targetDisk < n;
        }

        const uint32_t dataDisks = (m_cfg.level == RaidLevel::Raid0) ? n
                                 : (m_cfg.level == RaidLevel::Raid10) ? n / 2
                                 : m_dataDisks;
        const uint64_t perStripe = static_cast<uint64_t>(dataDisks) * chunkSectors;
        map.stripe       = vs / perStripe;
        const uint64_t inStripe = vs % perStripe;
        map.dataIndex    = static_cast<uint32_t>(inStripe / chunkSectors);
        const uint64_t sectorInChunk = inStripe % chunkSectors;
        map.memberSector = map.stripe * chunkSectors + sectorInChunk;

        if (m_cfg.level == RaidLevel::Raid0) {
            map.targetDisk = map.dataIndex;
        } else if (m_cfg.level == RaidLevel::Raid10) {
            map.targetDisk = map.dataIndex * 2;
        } else {
            RaidSlot roles[kRaidMaxMembers];
            ComputeRoles(map.stripe, roles);
            for (uint32_t d = 0; d < n; ++d) {
                if (roles[d].role == RaidSlotRole::Data &&
                    roles[d].index == map.dataIndex) {
                    map.targetDisk = d;
                    return true;
                }
            }
            return false;
        }
        return map.targetDisk < n;
    }

    uint32_t FirstAvailableDisk() const
    {
        for (uint32_t i = 0; i < m_cfg.members; ++i) {
            if (!m_missing[i] && m_members[i] != nullptr) return i;
        }
        return kRaidMaxMembers;
    }

    bool ReadMember(uint32_t disk, uint64_t memberSector, uint8_t* out) const
    {
        if (disk >= m_cfg.members || m_missing[disk] ||
            m_members[disk] == nullptr) return false;
        return m_members[disk]->Read(
            m_cfg.dataOffset + memberSector * kRaidSectorBytes,
            out, kRaidSectorBytes);
    }

    bool ReadVirtualSector(uint64_t vs, uint8_t* out) const
    {
        SectorMap map;
        if (!MapSector(vs, map)) return false;
        if (!m_missing[map.targetDisk] && m_members[map.targetDisk] != nullptr) {
            if (ReadMember(map.targetDisk, map.memberSector, out)) return true;
            // A read error on the target chunk (bad sector, unstable member)
            // is treated as a transient loss of that chunk: fall back to
            // parity reconstruction where the level has enough redundancy.
            // Never zero-fill -- if recovery is impossible, return false.
        }
        return RecoverSector(map, out);
    }

    bool RecoverSector(const SectorMap& map, uint8_t* out) const
    {
        switch (m_cfg.level) {
        case RaidLevel::Jbod:
        case RaidLevel::Raid0:
            return false;      // no redundancy: never fabricate data
        case RaidLevel::Raid1: {
            const uint32_t n = m_cfg.members;
            for (uint32_t d = 0; d < n; ++d) {
                if (d != map.targetDisk &&
                    ReadMember(d, map.memberSector, out)) return true;
            }
            return false;
        }
        case RaidLevel::Raid10: {
            const uint32_t partner = (map.targetDisk % 2 == 0)
                                     ? map.targetDisk + 1 : map.targetDisk - 1;
            return ReadMember(partner, map.memberSector, out);
        }
        case RaidLevel::Raid4:
        case RaidLevel::Raid5:
            return RecoverXor(map, out);
        case RaidLevel::Raid6:
            return RecoverGf(map, out);
        }
        return false;
    }

    // XOR of every other member (data + P) reproduces the missing member.
    bool RecoverXor(const SectorMap& map, uint8_t* out) const
    {
        const uint32_t n = m_cfg.members;
        RaidSlot roles[kRaidMaxMembers];
        ComputeRoles(map.stripe, roles);
        uint8_t tmp[kRaidSectorBytes];
        std::memset(out, 0, kRaidSectorBytes);
        bool haveParity = false;
        for (uint32_t d = 0; d < n; ++d) {
            if (d == map.targetDisk) continue;
            if (!ReadMember(d, map.memberSector, tmp)) return false;
            if (roles[d].role == RaidSlotRole::ParityP) haveParity = true;
            raid_detail::XorInto(out, tmp, kRaidSectorBytes);
        }
        if (!haveParity) return false;
        return true;
    }

    // RAID6: solve P / Q equations over GF(2^8) for one or two lost chunks.
    bool RecoverGf(const SectorMap& map, uint8_t* out) const
    {
        const uint32_t n = m_cfg.members;
        RaidSlot roles[kRaidMaxMembers];
        ComputeRoles(map.stripe, roles);

        uint8_t blocks[kRaidMaxMembers][kRaidSectorBytes];
        bool have[kRaidMaxMembers] = {};
        uint32_t unavailable = 0;
        for (uint32_t d = 0; d < n; ++d) {
            // Missing, unreadable and the requested chunk itself all count as
            // unknowns. RAID6 can solve any two; more than two is hopeless.
            if (d == map.targetDisk || m_missing[d] ||
                m_members[d] == nullptr) {
                ++unavailable;
                continue;
            }
            if (!m_members[d]->Read(
                    m_cfg.dataOffset + map.memberSector * kRaidSectorBytes,
                    blocks[d], kRaidSectorBytes)) {
                ++unavailable;
                continue;
            }
            have[d] = true;
        }
        if (unavailable > 2) return false;

        uint8_t pVal[kRaidSectorBytes] = {};
        uint8_t qVal[kRaidSectorBytes] = {};
        uint8_t xorKnown[kRaidSectorBytes] = {};
        uint8_t qKnown[kRaidSectorBytes] = {};
        bool haveP = false, haveQ = false;
        uint32_t missingIdx[2] = {0, 0};
        uint32_t missingCount = 0;

        for (uint32_t d = 0; d < n; ++d) {
            if (!have[d]) continue;
            if (roles[d].role == RaidSlotRole::ParityP) {
                std::memcpy(pVal, blocks[d], kRaidSectorBytes);
                haveP = true;
            } else if (roles[d].role == RaidSlotRole::ParityQ) {
                std::memcpy(qVal, blocks[d], kRaidSectorBytes);
                haveQ = true;
            }
        }
        for (uint32_t d = 0; d < n; ++d) {
            if (roles[d].role != RaidSlotRole::Data) continue;
            if (!have[d]) {
                if (missingCount >= 2) return false;
                missingIdx[missingCount++] = roles[d].index;
            } else {
                raid_detail::XorInto(xorKnown, blocks[d], kRaidSectorBytes);
                raid_detail::GfMulInto(qKnown, raid_detail::GfPow(roles[d].index),
                                       blocks[d], kRaidSectorBytes);
            }
        }
        if (missingCount == 0 || missingCount > 2) return false;

        if (missingCount == 1) {
            const uint32_t a = missingIdx[0];
            if (haveP) {
                std::memcpy(out, pVal, kRaidSectorBytes);
                raid_detail::XorInto(out, xorKnown, kRaidSectorBytes);
                return true;
            }
            if (!haveQ) return false;           // lost P and Q and a data disk
            uint8_t t[kRaidSectorBytes];
            std::memcpy(t, qVal, kRaidSectorBytes);
            raid_detail::XorInto(t, qKnown, kRaidSectorBytes);
            raid_detail::GfUnscale(t, raid_detail::GfPow(a), kRaidSectorBytes);
            std::memcpy(out, t, kRaidSectorBytes);
            return true;
        }

        // Two lost data chunks a,b:
        //   D_a ^ D_b             = P' = P ^ xorKnown
        //   g^a D_a ^ g^b D_b    = Q' = Q ^ qKnown
        //   => D_b = (Q' ^ g^a P') / (g^a ^ g^b)
        if (!haveP || !haveQ) return false;
        const uint32_t a = missingIdx[0];
        const uint32_t b = missingIdx[1];
        const uint8_t ca = raid_detail::GfPow(a);
        const uint8_t cb = raid_detail::GfPow(b);
        const uint8_t denom = static_cast<uint8_t>(ca ^ cb);
        if (denom == 0) return false;

        uint8_t pPrime[kRaidSectorBytes];
        uint8_t qPrime[kRaidSectorBytes];
        uint8_t scaled[kRaidSectorBytes];
        uint8_t db[kRaidSectorBytes];
        uint8_t da[kRaidSectorBytes];
        std::memcpy(pPrime, pVal, kRaidSectorBytes);
        raid_detail::XorInto(pPrime, xorKnown, kRaidSectorBytes);
        std::memcpy(qPrime, qVal, kRaidSectorBytes);
        raid_detail::XorInto(qPrime, qKnown, kRaidSectorBytes);
        std::memcpy(scaled, pPrime, kRaidSectorBytes);
        raid_detail::GfScale(scaled, ca, kRaidSectorBytes);
        std::memcpy(db, qPrime, kRaidSectorBytes);
        raid_detail::XorInto(db, scaled, kRaidSectorBytes);
        raid_detail::GfUnscale(db, denom, kRaidSectorBytes);
        std::memcpy(da, pPrime, kRaidSectorBytes);
        raid_detail::XorInto(da, db, kRaidSectorBytes);

        const uint8_t* chosen = (a == map.dataIndex) ? da : db;
        if (b != map.dataIndex && a != map.dataIndex) return false;
        std::memcpy(out, chosen, kRaidSectorBytes);
        return true;
    }

    RaidConfig   m_cfg;
    RaidInfo     m_info;
    std::vector<const IVirtualDiskReader*> m_members;
    std::vector<uint8_t> m_missing;
    uint64_t     m_memberBytes = 0;
    uint64_t     m_stripes     = 0;
    uint64_t     m_virtualSize = 0;
    uint32_t     m_dataDisks   = 0;
    uint32_t     m_parityDisks = 0;
    bool         m_open        = false;
};

} // namespace phoenix
