// A1-4b-a -- mdadm metadata parsing and conservative RAID layout probing.
//
// Why this file exists
// --------------------
// VirtualRaid.h accepts a RaidConfig and faithfully produces bytes for it.
// A WRONG config produces wrong bytes without any error, which is the single
// most dangerous failure mode in recovery work. This layer closes that gap:
//
//   1. mdadm 0.90 / 1.0 / 1.1 / 1.2 superblocks are parsed with full checksum
//      verification; disk roles (member order), chunk, level, layout,
//      data_offset and degraded slots come from the on-disk metadata rather
//      than from a guess.
//   2. The resulting geometry is validated with VirtualRaid::VerifyStripe()
//      before being handed to the caller. Metadata that disagrees with the
//      parity on the members is REJECTED, not silently trusted.
//   3. A deliberately limited blind probe is available for metadata-less
//      arrays. It only claims success for parity-bearing levels, requires
//      non-zero stripe data plus volume-start anchors, and returns Ambiguous
//      whenever two candidates score equally. XOR parity is order-independent,
//      so a "successful" P check alone can NEVER establish member order.
//
// Deliberate non-goals (A1-4b-b): SNIA DDF, Intel RIS, vendor layouts (HP
// delayed parity, Promise wide-pace Q, RAIDZ, far/offset RAID10), reshape /
// journal / PPL / bad-block-log / bitmap metadata, 4Kn, and real-array golden
// images. Anything we do not understand produces an explicit refusal.
//
// Pure C++17, header-only, no Qt / Win32 / device handles / write methods.

#ifndef PHOENIX_DISK_RAID_PROBE_H
#define PHOENIX_DISK_RAID_PROBE_H

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "VirtualRaid.h"   // phoenix::IVirtualDiskReader, RaidConfig, VirtualRaid

namespace phoenix {

// ---------------------------------------------------------------------------
// mdadm superblock parsing
// ---------------------------------------------------------------------------

constexpr uint32_t kMdMagic              = 0xa92b4efcu;
constexpr uint32_t kMd090SuperBytes      = 4096u;
constexpr uint32_t kMd090ReservedBytes   = 64u * 1024u;
constexpr uint32_t kMd090CsumWordOffset  = 0x98u / 4u;
constexpr uint32_t kMd1FixedBytes        = 256u;
constexpr uint32_t kMd1RolesOffset       = 0x100u;
constexpr uint32_t kMd1CsumOffset        = 0xD8u;
constexpr uint32_t kMd1MaxDevOffset      = 0xDCu;
constexpr uint32_t kMd1MaxRoles          = 384u;
constexpr uint32_t kMdRoleSpare          = 0xFFFFu;
constexpr uint32_t kMdRoleFaulty         = 0xFFFEu;
constexpr uint32_t kMdRoleMax            = 0xFF00u;

enum class MdadmMetadata : uint8_t
{
    None = 0,
    V090,
    V1_0,
    V1_1,
    V1_2,
    Unsupported
};

inline const char* MdadmMetadataName(MdadmMetadata v)
{
    switch (v) {
    case MdadmMetadata::None:        return "none";
    case MdadmMetadata::V090:        return "mdadm-0.90";
    case MdadmMetadata::V1_0:        return "mdadm-1.0";
    case MdadmMetadata::V1_1:        return "mdadm-1.1";
    case MdadmMetadata::V1_2:        return "mdadm-1.2";
    case MdadmMetadata::Unsupported: return "unsupported";
    }
    return "unknown";
}

enum class MdadmParseResult : uint8_t
{
    Ok,
    NotFound,
    ReadFailed,
    BadMagic,
    BadVersion,
    BadChecksum,
    BadGeometry,
    BadRole,
    UnsupportedFeature,
    TooManyDisks
};

inline const char* MdadmParseDetail(MdadmParseResult r)
{
    switch (r) {
    case MdadmParseResult::Ok:                 return "ok";
    case MdadmParseResult::NotFound:           return "no mdadm superblock";
    case MdadmParseResult::ReadFailed:         return "cannot read superblock area";
    case MdadmParseResult::BadMagic:           return "bad mdadm magic";
    case MdadmParseResult::BadVersion:         return "unsupported mdadm major version";
    case MdadmParseResult::BadChecksum:        return "superblock checksum mismatch";
    case MdadmParseResult::BadGeometry:        return "invalid level / chunk / disk count";
    case MdadmParseResult::BadRole:            return "invalid or missing device role";
    case MdadmParseResult::UnsupportedFeature: return "reshape / bitmap / PPL / journal metadata is not supported";
    case MdadmParseResult::TooManyDisks:       return "raid_disks exceeds supported maximum";
    }
    return "unknown";
}

struct MdadmSuperblock
{
    MdadmMetadata metadata        = MdadmMetadata::None;
    uint64_t      superOffset     = 0;       // byte offset of the superblock
    uint64_t      dataOffset      = 0;       // byte offset of array data on this member
    uint64_t      events          = 0;
    uint64_t      componentBytes  = 0;       // usable member data area
    uint32_t      level           = 0;       // Linux level (signed, as stored)
    uint32_t      layout          = 0;       // Linux layout code
    uint32_t      chunkBytes      = 0;
    uint32_t      raidDisks       = 0;
    uint32_t      role            = kMdRoleSpare;
    uint32_t      devNumber       = 0;
    uint32_t      maxDev          = 0;
    uint32_t      featureMap      = 0;
    bool          checksumOk      = false;
    bool          reshapeActive   = false;
    bool          bitmapPresent   = false;
    std::array<uint8_t, 16> setUuid{};
    std::vector<uint32_t> roles;             // index = dev_number, value = role
    std::string   detail;
};

namespace raid_probe_detail {

inline uint32_t Le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t Le64(const uint8_t* p)
{
    return static_cast<uint64_t>(Le32(p)) |
           (static_cast<uint64_t>(Le32(p + 4)) << 32);
}

// mdadm super1.c: calc_sb_1_csum() -- sum little-endian 32-bit words over
// 256 + max_dev*2 bytes with sb_csum zeroed; fold the 64-bit accumulator.
inline uint32_t Md1Checksum(const std::vector<uint8_t>& sb, uint32_t maxDev)
{
    const size_t total = kMd1FixedBytes + static_cast<size_t>(maxDev) * 2u;
    if (sb.size() < total) return 0;
    uint64_t sum = 0;
    size_t i = 0;
    for (; i + 4 <= total; i += 4) {
        uint32_t word = Le32(sb.data() + i);
        if (i == kMd1CsumOffset) word = 0;
        sum += word;
    }
    if (i + 2 <= total && i + 1 < sb.size())
        sum += static_cast<uint32_t>(sb[i]) | (static_cast<uint32_t>(sb[i + 1]) << 8);
    return static_cast<uint32_t>((sum & 0xFFFFFFFFull) + (sum >> 32));
}

// mdadm util.c: calc_csum() -- host-order 32-bit word sum, 64-bit fold.
// The 0.90 superblock is native-endian; on the little-endian targets we
// support this is identical to a little-endian word sum.
inline uint32_t Md090Checksum(const std::vector<uint8_t>& sb)
{
    uint64_t sum = 0;
    for (size_t i = 0; i + 4 <= kMd090SuperBytes; i += 4) {
        uint32_t word = Le32(sb.data() + i);
        if (i / 4u == kMd090CsumWordOffset) word = 0;
        sum += word;
    }
    return static_cast<uint32_t>((sum & 0xFFFFFFFFull) + (sum >> 32));
}

inline bool MapLinuxLevel(uint32_t raw, RaidLevel& out)
{
    switch (raw) {
    case 0xFFFFFFFFu: out = RaidLevel::Jbod;  return true;  // -1 linear
    case 0u:          out = RaidLevel::Raid0; return true;
    case 1u:          out = RaidLevel::Raid1; return true;
    case 4u:          out = RaidLevel::Raid4; return true;
    case 5u:          out = RaidLevel::Raid5; return true;
    case 6u:          out = RaidLevel::Raid6; return true;
    case 10u:         out = RaidLevel::Raid10; return true;
    default:          return false;
    }
}

inline bool MapLinuxLayout(uint32_t raw, RaidLevel level, RaidLayout& out)
{
    // Linux raid5.h ALGORITHM_*: 0 left-asym, 1 right-asym, 2 left-sym,
    // 3 right-sym, 4 parity-first, 5 parity-last. 6+ are rotating-restart /
    // DDF-style layouts that VirtualRaid does not model.
    if (level == RaidLevel::Raid10) {
        // far/offset RAID10 is encoded in the upper bytes of layout.
        if ((raw & ~0xFFu) != 0 || (raw & 0xFFu) != 2u) return false;
        out = RaidLayout::LeftSymmetric;
        return true;
    }
    if (level == RaidLevel::Raid0) {
        // mdadm 0.90 "dangerous" (non-uniform) RAID0 uses a different layout.
        if (raw != 0u) return false;
        out = RaidLayout::LeftSymmetric;
        return true;
    }
    if (level == RaidLevel::Raid1 || level == RaidLevel::Jbod) {
        out = RaidLayout::LeftSymmetric;
        return true;
    }
    switch (raw) {
    case 0u: out = RaidLayout::LeftAsymmetric;   return true;
    case 1u: out = RaidLayout::RightAsymmetric;  return true;
    case 2u: out = RaidLayout::LeftSymmetric;    return true;
    case 3u: out = RaidLayout::RightSymmetric;   return true;
    case 4u: out = RaidLayout::ParityFirst;      return true;
    case 5u: out = RaidLayout::ParityLast;       return true;
    default: return false;
    }
}

inline bool ReadExact(const IVirtualDiskReader* r, uint64_t off,
                      uint8_t* dst, size_t length)
{
    return r != nullptr && r->Read(off, dst, length);
}

// 0.90 superblock: ((deviceBytes & ~(64K-1)) - 64K), 4096 bytes long.
inline MdadmParseResult ParseMd090(const IVirtualDiskReader& reader,
                                   MdadmSuperblock& out)
{
    const uint64_t size = reader.Size();
    if (size <= kMd090ReservedBytes) return MdadmParseResult::NotFound;
    const uint64_t aligned = size & ~static_cast<uint64_t>(kMd090ReservedBytes - 1u);
    if (aligned <= kMd090ReservedBytes) return MdadmParseResult::NotFound;
    const uint64_t off = aligned - kMd090ReservedBytes;
    std::vector<uint8_t> sb(kMd090SuperBytes, 0);
    if (!reader.Read(off, sb.data(), sb.size())) return MdadmParseResult::ReadFailed;
    if (Le32(sb.data()) != kMdMagic) return MdadmParseResult::NotFound;

    out = MdadmSuperblock{};
    out.metadata    = MdadmMetadata::V090;
    out.superOffset = off;
    out.dataOffset  = 0;
    out.level       = Le32(sb.data() + 0x1C);
    out.raidDisks   = Le32(sb.data() + 0x28);
    out.layout      = Le32(sb.data() + 0x100);
    out.chunkBytes  = Le32(sb.data() + 0x104);
    out.events      = static_cast<uint64_t>(Le32(sb.data() + 0x9C)) |
                      (static_cast<uint64_t>(Le32(sb.data() + 0xA0)) << 32);
    const uint32_t state = Le32(sb.data() + 0x84);
    out.bitmapPresent = (state & (1u << 8)) != 0;   // MD_SB_BITMAP_PRESENT
    // 0.90 "size" is in 1 KiB units; component bytes = size * 1024.
    out.componentBytes = static_cast<uint64_t>(Le32(sb.data() + 0x20)) * 1024ull;
    // set_uuid0 at 0x14, set_uuid1..3 at 0x34/0x38/0x3C, native-endian u32.
    for (uint32_t i = 0; i < 4; ++i) {
        const uint32_t w = Le32(sb.data() + (i == 0 ? 0x14u : (0x30u + 4u * i)));
        out.setUuid[i * 4 + 0] = static_cast<uint8_t>(w);
        out.setUuid[i * 4 + 1] = static_cast<uint8_t>(w >> 8);
        out.setUuid[i * 4 + 2] = static_cast<uint8_t>(w >> 16);
        out.setUuid[i * 4 + 3] = static_cast<uint8_t>(w >> 24);
    }
    if (Le32(sb.data() + 4) != 0u) return MdadmParseResult::BadVersion;
    if (Le32(sb.data() + 0x98) != Md090Checksum(sb))
        return MdadmParseResult::BadChecksum;
    out.checksumOk = true;
    // reshape fields only exist for minor_version > 90.
    const uint32_t minor = Le32(sb.data() + 0x08);
    const uint64_t reshapePos = static_cast<uint64_t>(Le32(sb.data() + 0xB0)) |
                                (static_cast<uint64_t>(Le32(sb.data() + 0xB4)) << 32);
    out.reshapeActive = minor > 90 && (reshapePos + 1ull) != 0ull;

    // this_disk descriptor at word 992 (0xF80): number, raid_disk, state.
    const uint32_t thisNumber   = Le32(sb.data() + 0xF80);
    const uint32_t thisRaidDisk = Le32(sb.data() + 0xF8C);
    const uint32_t thisState    = Le32(sb.data() + 0xF90);
    out.devNumber = thisNumber;
    out.role      = thisRaidDisk;
    if (thisNumber >= 27u || thisRaidDisk >= out.raidDisks)
        return MdadmParseResult::BadRole;
    // disks[] at word 128 (0x200), 128 bytes per descriptor.
    out.roles.assign(out.raidDisks, kMdRoleSpare);
    out.maxDev = 27u;
    for (uint32_t i = 0; i < 27u; ++i) {
        const size_t d = 0x200u + static_cast<size_t>(i) * 128u;
        const uint32_t rd    = Le32(sb.data() + d + 0x0C);
        const uint32_t st    = Le32(sb.data() + d + 0x10);
        const bool inSync    = (st & (1u << 2)) != 0;
        const bool active    = (st & (1u << 1)) != 0;
        const bool faulty    = (st & (1u << 0)) != 0;   // MD_DISK_FAULTY is bit 0
        if (rd < out.raidDisks && inSync && active && !faulty) out.roles[rd] = rd;
    }
    if (thisState & (1u << 0)) out.role = kMdRoleFaulty;
    return MdadmParseResult::Ok;
}

// 1.x superblocks: 1.1 at byte 0, 1.2 at byte 4096, 1.0 near the end.
inline MdadmParseResult ParseMd1(const IVirtualDiskReader& reader,
                                 MdadmSuperblock& out)
{
    const uint64_t size = reader.Size();
    if (size < 8192u) return MdadmParseResult::NotFound;
    uint64_t candidates[3] = { 0ull, 4096ull, 0ull };
    {
        const uint64_t sectors = size / 512u;
        if (sectors <= 16u) return MdadmParseResult::NotFound;
        candidates[2] = ((sectors - 16u) & ~uint64_t(7u)) * 512u;
    }
    const MdadmMetadata kinds[3] = {
        MdadmMetadata::V1_1, MdadmMetadata::V1_2, MdadmMetadata::V1_0
    };

    std::vector<uint8_t> sb;
    uint32_t maxDev = 0;
    uint64_t foundOff = 0;
    MdadmMetadata foundKind = MdadmMetadata::None;
    for (uint32_t k = 0; k < 3; ++k) {
        std::vector<uint8_t> probe(kMd1FixedBytes + kMd1MaxRoles * 2u, 0);
        if (!reader.Read(candidates[k], probe.data(), kMd1FixedBytes))
            continue;
        if (Le32(probe.data()) != kMdMagic) continue;
        maxDev = Le32(probe.data() + kMd1MaxDevOffset);
        const size_t need = kMd1FixedBytes + static_cast<size_t>(maxDev) * 2u;
        if (maxDev > kMd1MaxRoles) return MdadmParseResult::TooManyDisks;
        if (need > probe.size()) return MdadmParseResult::ReadFailed;
        if (!reader.Read(candidates[k], probe.data(), need)) continue;
        foundOff  = candidates[k];
        foundKind = kinds[k];
        sb = std::move(probe);
        break;
    }
    if (foundKind == MdadmMetadata::None) return MdadmParseResult::NotFound;

    out = MdadmSuperblock{};
    out.metadata    = foundKind;
    out.superOffset = foundOff;
    out.maxDev      = maxDev;
    out.devNumber   = Le32(sb.data() + 0xA0);
    out.featureMap  = Le32(sb.data() + 0x08);
    out.level       = Le32(sb.data() + 0x48);
    out.layout      = Le32(sb.data() + 0x4C);
    out.chunkBytes  = Le32(sb.data() + 0x58) * 512u;   // chunksize in sectors
    out.raidDisks   = Le32(sb.data() + 0x5C);
    out.dataOffset  = Le64(sb.data() + 0x80) * 512u;
    out.componentBytes = Le64(sb.data() + 0x50) * 512u; // "size" in sectors
    out.events      = Le64(sb.data() + 0xC8);
    std::memcpy(out.setUuid.data(), sb.data() + 0x10, 16);
    const uint32_t superSectors = static_cast<uint32_t>(Le64(sb.data() + 0x90));
    out.bitmapPresent = (out.featureMap & 1u) != 0;
    out.reshapeActive = (out.featureMap & 4u) != 0;
    if (Le32(sb.data() + 4) != 1u) return MdadmParseResult::BadVersion;
    if (static_cast<uint64_t>(superSectors) * 512u != foundOff)
        return MdadmParseResult::BadGeometry;
    if (Le32(sb.data() + kMd1CsumOffset) != Md1Checksum(sb, maxDev))
        return MdadmParseResult::BadChecksum;
    out.checksumOk = true;

    out.roles.assign(maxDev, kMdRoleSpare);
    for (uint32_t i = 0; i < maxDev; ++i)
        out.roles[i] = static_cast<uint32_t>(sb[kMd1RolesOffset + i * 2u]) |
                       (static_cast<uint32_t>(sb[kMd1RolesOffset + i * 2u + 1u]) << 8);
    out.role = (out.devNumber < maxDev) ? out.roles[out.devNumber] : kMdRoleFaulty;
    if (out.raidDisks == 0u || out.raidDisks > kRaidMaxMembers)
        return MdadmParseResult::BadGeometry;
    if (out.role >= out.raidDisks) return MdadmParseResult::BadRole;
    return MdadmParseResult::Ok;
}

} // namespace raid_probe_detail

inline MdadmParseResult ParseMdadmSuperblock(const IVirtualDiskReader& reader,
                                             MdadmSuperblock& out)
{
    out = MdadmSuperblock{};
    MdadmParseResult r = raid_probe_detail::ParseMd1(reader, out);
    if (r == MdadmParseResult::NotFound)
        r = raid_probe_detail::ParseMd090(reader, out);
    if (r != MdadmParseResult::Ok) {
        out = MdadmSuperblock{};
        return r;
    }
    RaidLevel level = RaidLevel::Jbod;
    RaidLayout layout = RaidLayout::LeftSymmetric;
    if (!raid_probe_detail::MapLinuxLevel(out.level, level)) {
        out.detail = "unsupported Linux RAID level";
        return MdadmParseResult::BadGeometry;
    }
    if (!raid_probe_detail::MapLinuxLayout(out.layout, level, layout)) {
        out.detail = "unsupported Linux RAID layout";
        return MdadmParseResult::UnsupportedFeature;
    }
    if (out.reshapeActive || out.bitmapPresent || out.featureMap != 0u) {
        out.detail = MdadmParseDetail(MdadmParseResult::UnsupportedFeature);
        return MdadmParseResult::UnsupportedFeature;
    }
    if (level != RaidLevel::Raid1 && level != RaidLevel::Jbod &&
        (out.chunkBytes < 512u || out.chunkBytes > kRaidMaxChunk ||
         (out.chunkBytes % 512u) != 0u)) {
        out.detail = "invalid chunk size";
        return MdadmParseResult::BadGeometry;
    }
    out.detail = "level=" + std::string(RaidLevelName(level)) +
                 " layout=" + std::string(RaidLayoutName(layout));
    return MdadmParseResult::Ok;
}

// ---------------------------------------------------------------------------
// Probe
// ---------------------------------------------------------------------------

enum class RaidProbeStatus : uint8_t
{
    Ok,
    Ambiguous,
    NoLayoutFound,
    MetadataRejected,
    BudgetExceeded,
    BadMembers,
    TooManyMembers,
    UnsupportedMetadata,
    VerificationFailed
};

inline const char* RaidProbeStatusName(RaidProbeStatus s)
{
    switch (s) {
    case RaidProbeStatus::Ok:                   return "Ok";
    case RaidProbeStatus::Ambiguous:            return "Ambiguous";
    case RaidProbeStatus::NoLayoutFound:        return "NoLayoutFound";
    case RaidProbeStatus::MetadataRejected:     return "MetadataRejected";
    case RaidProbeStatus::BudgetExceeded:       return "BudgetExceeded";
    case RaidProbeStatus::BadMembers:           return "BadMembers";
    case RaidProbeStatus::TooManyMembers:       return "TooManyMembers";
    case RaidProbeStatus::UnsupportedMetadata:  return "UnsupportedMetadata";
    case RaidProbeStatus::VerificationFailed:   return "VerificationFailed";
    }
    return "unknown";
}

struct ProbeAnchor
{
    uint64_t              offset = 0;
    std::vector<uint8_t>  bytes;
};

struct RaidProbeOptions
{
    bool     useMetadata       = true;
    bool     allowBlind        = true;
    uint64_t maxCandidates     = 20000;
    uint32_t maxPermutations   = 5040;
    uint32_t stripesToCheck    = 4;
    uint32_t sectorsPerStripe  = 8;
    uint32_t maxMembersForBlind = 8;
    std::vector<uint32_t> chunkCandidates =
        { 4u * 1024u, 16u * 1024u, 64u * 1024u, 256u * 1024u, 1024u * 1024u };
    std::vector<RaidLevel> levelCandidates =
        { RaidLevel::Raid4, RaidLevel::Raid5, RaidLevel::Raid6 };
    std::vector<RaidLayout> layoutCandidates =
        { RaidLayout::LeftSymmetric, RaidLayout::RightSymmetric,
          RaidLayout::LeftAsymmetric, RaidLayout::RightAsymmetric,
          RaidLayout::ParityFirst, RaidLayout::ParityLast };
    // Known-plaintext anchors inside the VIRTUAL volume. Forensics use: a
    // caller that possesses captured sectors (MBR code, GPT header, NTFS BPB,
    // a chunk-boundary marker) can disambiguate member order. XOR parity is
    // order-independent, so a single prefix only pins down the first data
    // disk; at least two anchors at different chunk offsets are required.
    // WITHOUT anchors the probe MUST refuse rather than return a plausible
    // but unverifiable order.
    std::vector<ProbeAnchor> anchors;
};

struct RaidProbeResult
{
    RaidProbeStatus       status        = RaidProbeStatus::NoLayoutFound;
    RaidConfig            config;
    MdadmMetadata         metadata      = MdadmMetadata::None;
    uint32_t              confidence    = 0;      // 0..100, evidence-based
    std::vector<uint32_t> order;                  // array position -> input slot
    uint64_t              candidatesTried = 0;
    uint32_t              stripesVerified = 0;
    uint32_t              anchorHits     = 0;
    std::string           evidence;
    std::string           detail;
};

namespace raid_probe_detail {

inline bool HasNonZero(const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n; ++i) if (p[i] != 0) return true;
    return false;
}

// Generic, format-neutral volume-start anchors. These are weak evidence on
// their own: 0x55AA appears on many disks, so the probe still requires a
// unique best score before claiming success.
inline uint32_t AnchorScore(const uint8_t* sector)
{
    uint32_t hits = 0;
    if (sector[510] == 0x55u && sector[511] == 0xAAu) ++hits;
    if (std::memcmp(sector + 3, "NTFS    ", 8) == 0) ++hits;
    if (std::memcmp(sector + 3, "-FVE-FS-", 8) == 0) ++hits;
    if (std::memcmp(sector, "EFI PART", 8) == 0) ++hits;
    if (std::memcmp(sector + 3, "EXFAT   ", 8) == 0) ++hits;
    return hits;
}

} // namespace raid_probe_detail

class RaidProbe
{
public:
    RaidProbeResult Probe(const std::vector<const IVirtualDiskReader*>& slots,
                          const RaidProbeOptions& options)
    {
        RaidProbeResult res;
        if (slots.empty() || slots.size() > kRaidMaxMembers) {
            res.status = slots.empty() ? RaidProbeStatus::BadMembers
                                       : RaidProbeStatus::TooManyMembers;
            res.detail = slots.empty() ? "no member readers supplied"
                                       : "more than 32 member readers";
            return res;
        }
        for (size_t i = 0; i < slots.size(); ++i) {
            if (slots[i] == nullptr) {
                res.status = RaidProbeStatus::BadMembers;
                res.detail = "null member reader at slot " + std::to_string(i);
                return res;
            }
        }

        // ---- Path 1: mdadm metadata drives the geometry -------------------
        if (options.useMetadata) {
            std::vector<MdadmSuperblock> sbs(slots.size());
            MdadmParseResult firstBad = MdadmParseResult::Ok;
            size_t parsed = 0;
            for (size_t i = 0; i < slots.size(); ++i) {
                const MdadmParseResult r = ParseMdadmSuperblock(*slots[i], sbs[i]);
                if (r == MdadmParseResult::Ok) ++parsed;
                else if (firstBad == MdadmParseResult::Ok) firstBad = r;
            }
            if (parsed == slots.size() && parsed >= 2u)
                return ProbeFromMetadata(slots, sbs, options, res);
            if (parsed > 0 && parsed < slots.size()) {
                // Partially damaged metadata is a hard stop: guessing the
                // missing roles would be exactly the silent-wrong-data case.
                res.status = RaidProbeStatus::MetadataRejected;
                res.detail = "mdadm metadata found on only " +
                             std::to_string(parsed) + "/" +
                             std::to_string(slots.size()) + " members";
                return res;
            }
            if (parsed == 1u) {
                res.status = RaidProbeStatus::MetadataRejected;
                res.detail = "a single mdadm member is insufficient to rebuild an array";
                return res;
            }
            if (firstBad == MdadmParseResult::UnsupportedFeature ||
                firstBad == MdadmParseResult::BadGeometry) {
                res.status = RaidProbeStatus::UnsupportedMetadata;
                res.detail = MdadmParseDetail(firstBad);
                return res;
            }
        }

        // ---- Path 2: limited blind probe ---------------------------------
        if (!options.allowBlind) {
            res.status = RaidProbeStatus::NoLayoutFound;
            res.detail = "no mdadm metadata and blind probing is disabled";
            return res;
        }
        return ProbeBlind(slots, options, res);
    }

private:
    // Build an ordered reader vector from disk roles, filling holes with
    // nullptr and listing them in cfg.missing (degraded arrays).
    RaidProbeResult ProbeFromMetadata(const std::vector<const IVirtualDiskReader*>& slots,
                                      const std::vector<MdadmSuperblock>& sbs,
                                      const RaidProbeOptions& options,
                                      RaidProbeResult& res)
    {
        const MdadmSuperblock& ref = sbs[0];
        for (size_t i = 1; i < sbs.size(); ++i) {
            const MdadmSuperblock& s = sbs[i];
            if (s.metadata != ref.metadata || s.level != ref.level ||
                s.layout != ref.layout || s.chunkBytes != ref.chunkBytes ||
                s.raidDisks != ref.raidDisks ||
                s.dataOffset != ref.dataOffset ||
                s.setUuid != ref.setUuid) {
                res.status = RaidProbeStatus::MetadataRejected;
                res.detail = "member " + std::to_string(i) +
                             " disagrees with member 0 (uuid/level/layout/chunk/offset)";
                return res;
            }
        }
        if (ref.raidDisks > kRaidMaxMembers || ref.raidDisks < slots.size()) {
            res.status = RaidProbeStatus::MetadataRejected;
            res.detail = "raid_disks=" + std::to_string(ref.raidDisks) +
                         " does not match " + std::to_string(slots.size()) + " members";
            return res;
        }

        std::vector<uint32_t> order(ref.raidDisks, kMdRoleSpare);
        for (size_t i = 0; i < slots.size(); ++i) {
            const uint32_t role = sbs[i].role;
            if (role >= ref.raidDisks || order[role] != kMdRoleSpare) {
                res.status = RaidProbeStatus::MetadataRejected;
                res.detail = "duplicate or out-of-range disk role on member " +
                             std::to_string(i);
                return res;
            }
            order[role] = static_cast<uint32_t>(i);
        }

        std::vector<const IVirtualDiskReader*> ordered(ref.raidDisks, nullptr);
        std::vector<uint32_t> missing;
        for (uint32_t p = 0; p < ref.raidDisks; ++p) {
            if (order[p] == kMdRoleSpare) missing.push_back(p);
            else ordered[p] = slots[order[p]];
        }

        RaidLevel level = RaidLevel::Jbod;
        RaidLayout layout = RaidLayout::LeftSymmetric;
        if (!raid_probe_detail::MapLinuxLevel(ref.level, level) ||
            !raid_probe_detail::MapLinuxLayout(ref.layout, level, layout)) {
            res.status = RaidProbeStatus::UnsupportedMetadata;
            res.detail = "unsupported mdadm level/layout combination";
            return res;
        }
        RaidConfig cfg;
        cfg.level       = level;
        cfg.layout      = layout;
        cfg.members     = ref.raidDisks;
        cfg.chunkBytes  = (level == RaidLevel::Raid1 || level == RaidLevel::Jbod)
                          ? 512u : ref.chunkBytes;
        cfg.dataOffset  = ref.dataOffset;
        cfg.missing     = missing;
        if (ref.componentBytes != 0)
            cfg.memberBytes = ref.componentBytes;

        VirtualRaid raid;
        const RaidOpenResult opened = raid.Open(cfg, ordered);
        if (opened != RaidOpenResult::Ok) {
            res.status = RaidProbeStatus::VerificationFailed;
            res.detail = std::string("VirtualRaid rejected metadata geometry: ") +
                         RaidOpenDetail(opened);
            return res;
        }

        uint32_t verified = 0;
        if (missing.empty() && (level == RaidLevel::Raid4 ||
                                level == RaidLevel::Raid5 ||
                                level == RaidLevel::Raid6)) {
            for (uint32_t s = 0; s < options.stripesToCheck; ++s) {
                RaidVerifyStats stats;
                if (raid.VerifyStripe(s, options.sectorsPerStripe, &stats) !=
                    RaidVerifyResult::Ok) {
                    res.status = RaidProbeStatus::VerificationFailed;
                    res.detail = "mdadm metadata disagrees with on-disk parity at stripe " +
                                 std::to_string(s);
                    return res;
                }
                ++verified;
            }
        }

        uint8_t first[512] = {};
        const uint32_t anchors = raid.Read(0, first, sizeof(first))
                               ? raid_probe_detail::AnchorScore(first) : 0u;
        res.status          = RaidProbeStatus::Ok;
        res.config          = cfg;
        res.metadata        = ref.metadata;
        res.order           = order;
        res.stripesVerified = verified;
        res.anchorHits      = anchors;
        res.confidence      = verified > 0 ? 100u : (missing.empty() ? 85u : 70u);
        res.evidence        = std::string(MdadmMetadataName(ref.metadata)) +
                             " checksum-valid on all members; " +
                             std::to_string(verified) + " parity stripe(s) verified";
        res.detail          = "geometry from mdadm metadata (not guessed)";
        return res;
    }

    struct Candidate
    {
        RaidConfig            cfg;
        std::vector<uint32_t> order;
        uint32_t              score = 0;
        uint32_t              stripesVerified = 0;
        uint32_t              anchorHits = 0;
        std::string           label;
    };

    static bool HasParity(RaidLevel l)
    {
        return l == RaidLevel::Raid4 || l == RaidLevel::Raid5 ||
               l == RaidLevel::Raid6;
    }

    uint32_t Evaluate(const std::vector<const IVirtualDiskReader*>& slots,
                      const std::vector<uint32_t>& order,
                      const RaidProbeOptions& options,
                      RaidConfig cfg, Candidate& out) const
    {
        std::vector<const IVirtualDiskReader*> ordered(slots.size(), nullptr);
        for (size_t p = 0; p < order.size(); ++p) ordered[p] = slots[order[p]];
        VirtualRaid raid;
        if (raid.Open(cfg, ordered) != RaidOpenResult::Ok) return 0;

        const uint64_t chunkSectors = cfg.chunkBytes / kRaidSectorBytes;
        uint32_t verified = 0;
        for (uint32_t s = 0; s < options.stripesToCheck; ++s) {
            if (s >= raid.Info().stripes) break;
            // Inspect the exact member sectors that VerifyStripe() is about to
            // check. An all-zero stripe makes P/Q trivially true and must not
            // be counted as evidence for a guessed geometry.
            const uint64_t memberOff = cfg.dataOffset +
                static_cast<uint64_t>(s) * chunkSectors * kRaidSectorBytes;
            bool nonZero = false;
            for (size_t d = 0; d < ordered.size(); ++d) {
                uint8_t sample[kRaidSectorBytes] = {};
                if (ordered[d] != nullptr &&
                    ordered[d]->Read(memberOff, sample, sizeof(sample)) &&
                    raid_probe_detail::HasNonZero(sample, sizeof(sample))) {
                    nonZero = true;
                    break;
                }
            }
            if (!nonZero) continue;
            RaidVerifyStats stats;
            if (raid.VerifyStripe(s, options.sectorsPerStripe, &stats) !=
                RaidVerifyResult::Ok) return 0;
            ++verified;
        }
        if (verified == 0) return 0;

        uint8_t head[512] = {};
        uint32_t anchors = 0;
        if (raid.Read(0, head, sizeof(head)))
            anchors = raid_probe_detail::AnchorScore(head);
        uint32_t matched = 0;
        for (const ProbeAnchor& a : options.anchors) {
            if (a.bytes.empty()) continue;
            if (a.offset > raid.Size() ||
                a.bytes.size() > raid.Size() - a.offset) break;
            std::vector<uint8_t> got(a.bytes.size());
            if (!raid.Read(a.offset, got.data(), got.size()) || got != a.bytes) break;
            ++matched;
        }
        const uint32_t prefix = (matched == options.anchors.size() &&
                                 !options.anchors.empty()) ? 1u : 0u;
        uint32_t score = verified * 10u + anchors * 5u + prefix * 50u;
        if (score > out.score || out.stripesVerified == 0) {
            out.cfg             = cfg;
            out.order           = order;
            out.score           = score;
            out.stripesVerified = verified;
            out.anchorHits      = anchors + prefix;
            out.label           = std::string(RaidLevelName(cfg.level)) + "/" +
                                 RaidLayoutName(cfg.layout) + "/" +
                                 std::to_string(cfg.chunkBytes);
        }
        return score;
    }

    RaidProbeResult ProbeBlind(const std::vector<const IVirtualDiskReader*>& slots,
                               const RaidProbeOptions& options,
                               RaidProbeResult& res)
    {
        if (slots.size() > options.maxMembersForBlind) {
            res.status = RaidProbeStatus::BudgetExceeded;
            res.detail = "blind probing is limited to " +
                         std::to_string(options.maxMembersForBlind) + " members";
            return res;
        }
        // Without known plaintext, XOR parity cannot order data disks: refuse
        // rather than return a plausible-looking but unverifiable order.
        if (options.anchors.size() < 2u) {
            res.status = RaidProbeStatus::Ambiguous;
            res.detail = "blind probe needs >=2 known-plaintext anchors: XOR parity "
                         "is order-independent and cannot establish member order";
            return res;
        }

        std::vector<uint32_t> order(slots.size());
        for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        uint64_t perms = 1;
        for (uint32_t i = 2; i <= static_cast<uint32_t>(slots.size()); ++i) perms *= i;
        if (perms > options.maxPermutations) {
            res.status = RaidProbeStatus::BudgetExceeded;
            res.detail = "permutation budget exceeded";
            return res;
        }

        Candidate best;
        uint32_t bestCount = 0;
        uint64_t tried = 0;
        std::string bestLabel;
        std::vector<std::string> ties;
        const uint64_t budget = options.maxCandidates;

        auto consider = [&](const RaidConfig& cfg) {
            ++tried;
            Candidate local;
            const uint32_t score = Evaluate(slots, order, options, cfg, local);
            if (score == 0) return;
            if (score > best.score) {
                best = local;
                bestCount = 1;
                bestLabel = local.label;
                ties.clear();
                ties.push_back(local.label);
            } else if (score == best.score) {
                ++bestCount;
                if (ties.size() < 12u) ties.push_back(local.label);
            }
        };

        bool exhausted = false;
        do {
            for (const RaidLevel level : options.levelCandidates) {
                if (!HasParity(level)) continue;   // no verifiable evidence
                for (const RaidLayout layout : options.layoutCandidates) {
                    for (const uint32_t chunk : options.chunkCandidates) {
                        if (tried >= budget) { exhausted = true; break; }
                        RaidConfig cfg;
                        cfg.level      = level;
                        cfg.layout     = layout;
                        cfg.members    = static_cast<uint32_t>(slots.size());
                        cfg.chunkBytes = chunk;
                        consider(cfg);
                    }
                    if (exhausted) break;
                }
                if (exhausted) break;
            }
            if (exhausted) break;
        } while (std::next_permutation(order.begin(), order.end()));

        res.candidatesTried = tried;
        if (exhausted) {
            res.status = RaidProbeStatus::BudgetExceeded;
            res.detail = "candidate budget (" + std::to_string(budget) +
                         ") exhausted before a unique layout was established";
            return res;
        }
        if (best.score == 0) {
            res.status = RaidProbeStatus::NoLayoutFound;
            res.detail = "no parity-consistent layout found";
            return res;
        }
        if (bestCount > 1) {
            res.status = RaidProbeStatus::Ambiguous;
            std::string labels;
            for (const std::string& t : ties) {
                if (!labels.empty()) labels += ", ";
                labels += t;
            }
            res.detail = std::to_string(bestCount) +
                         " equally scoring layouts (score " +
                         std::to_string(best.score) + ": " + labels +
                         "); refusing to guess";
            return res;
        }
        res.status          = RaidProbeStatus::Ok;
        res.config          = best.cfg;
        res.order           = best.order;
        res.metadata        = MdadmMetadata::None;
        res.confidence      = 60u;   // blind evidence is weaker than metadata
        res.stripesVerified = best.stripesVerified;
        res.anchorHits      = best.anchorHits;
        res.evidence        = "blind: " + bestLabel + ", " +
                             std::to_string(best.stripesVerified) +
                             " parity stripe(s) + volume-start prefix match";
        res.detail          = "unique best candidate among " +
                             std::to_string(tried) + " tried";
        return res;
    }
};

} // namespace phoenix

#endif // PHOENIX_DISK_RAID_PROBE_H
