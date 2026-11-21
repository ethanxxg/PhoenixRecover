#pragma once

// ---------------------------------------------------------------------------
// BootSectorRepair -- repair *planners* aligned with TestDisk 7.2 semantics.
//
// Reference implementations (GPLv2+, read for behaviour only, no code copied):
//   src/fat32.c   "Backup BS" / "Org. BS" -- copy sector 6..8 over sector 0
//   src/ntfs_fix.c repair_MFT             -- pick the good MFT / MFTMirr copy
//   src/ntfs.c    recover_NTFS(backup=1)  -- rebuild boot sector from the copy
//                                          stored in the volume's last sector
//   src/ext2_sb.c                         -- locate the alternate superblock
//
// Design rules kept from this project's A0 invariants:
//   * Header-only, zero Qt / Win32 / I/O. Reads go through an injected
//     ByteReader, so a planner can run against a device, an image or a test
//     buffer with the same code.
//   * Planners NEVER write. They return a RepairPlan carrying the exact
//     before/after images; applying it is the caller's job through
//     WriteGuard -> WriteTransaction -> DeviceWriter.
//   * Every candidate source is *validated* before it is trusted. A wrong
//     plan is silent corruption, so an unvalidated backup yields no plan.
//   * Confidence is explicit: a high-confidence plan may be offered as a
//     one-click repair, a low-confidence one must stay behind the expert gate
//     (mirror-to-MFT overwrites the only surviving copy when the guess is
//     wrong, exactly like TestDisk's "DANGEROUS NON REVERSIBLE" prompt).
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace phoenix {
namespace repair {

// (offset, buffer, length) -> true when the full range was read.
using ByteReader = std::function<bool(uint64_t, uint8_t*, size_t)>;

enum class RepairKind : uint8_t
{
    None = 0,
    Fat32BootFromBackup,    // sector 6..8 -> sector 0..2
    FatTableFromMirror,     // FAT#2 -> FAT#1 (FAT#1 corrupt)
    FatTableMirrorUpdate,   // FAT#1 -> FAT#2 (mirror stale)
    NtfsBootFromBackup,     // last sector -> sector 0
    NtfsMftMirrorFromMft,   // $MFT head -> $MFTMirr
    NtfsMftFromMirror,      // $MFTMirr -> $MFT (low confidence, expert only)
    ExtSuperblockFromBackup // alternate SB -> primary SB
};

const char* RepairKindName(RepairKind k);

struct RepairPlan
{
    RepairKind           kind       = RepairKind::None;
    uint64_t             offsetBytes = 0;   // absolute in the source
    uint32_t             lengthBytes = 0;
    std::vector<uint8_t> before;
    std::vector<uint8_t> after;
    int                  confidence = 0;    // 0..100
    std::string          reason;            // stable English text for logs
    std::string          followUp;          // e.g. "run fsck to rebuild GDT"

    bool IsValid() const
    {
        return kind != RepairKind::None &&
               lengthBytes > 0 &&
               after.size() == lengthBytes &&
               before.size() == lengthBytes;
    }
};

// ---------------------------------------------------------------------------
// Small little-endian helpers (no <bit>, GCC 7.3 / MSVC 2017 friendly).
// ---------------------------------------------------------------------------
inline uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t Le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t Le64(const uint8_t* p)
{
    return static_cast<uint64_t>(Le32(p)) | (static_cast<uint64_t>(Le32(p + 4)) << 32);
}

inline bool IsPow2(uint32_t v) { return v != 0 && (v & (v - 1)) == 0; }

// Reads `len` bytes at `off` into a fresh vector; empty on failure.
inline std::vector<uint8_t> ReadBytes(const ByteReader& read, uint64_t off, size_t len)
{
    std::vector<uint8_t> buf(len, 0);
    if (len == 0) return buf;
    if (!read(off, buf.data(), len)) buf.clear();
    return buf;
}

// ---------------------------------------------------------------------------
// Validity predicates -- the same checks the parsers use, kept here so a
// planner never proposes a "repair" taken from a sector that is also garbage.
// ---------------------------------------------------------------------------
inline bool ValidFat32Boot(const uint8_t* sec, uint32_t sectorSize,
                           uint32_t* outSpc = nullptr,
                           uint64_t* outTotalSectors = nullptr,
                           uint32_t* outReserved = nullptr,
                           uint32_t* outFatSize32 = nullptr)
{
    if (sec[0x1FE] != 0x55 || sec[0x1FF] != 0xAA) return false;
    const uint32_t bps = Le16(sec + 0x0B);
    if (bps != sectorSize && bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return false;
    if (!IsPow2(bps)) return false;
    const uint32_t spc = sec[0x0D];
    if (!IsPow2(spc) || spc == 0 || spc > 128) return false;
    const uint32_t reserved = Le16(sec + 0x0E);
    if (reserved == 0) return false;
    if (sec[0x10] == 0 || sec[0x10] > 4) return false;              // FAT count
    if (Le16(sec + 0x11) != 0) return false;                        // root entries must be 0
    if (Le16(sec + 0x13) != 0) return false;                        // total16 must be 0
    const uint32_t fat32 = Le32(sec + 0x24);
    const uint32_t total32 = Le32(sec + 0x20);
    if (fat32 == 0 || total32 == 0) return false;
    if (std::memcmp(sec + 0x52, "FAT32   ", 8) != 0) return false;

    if (outSpc)          *outSpc = spc;
    if (outTotalSectors) *outTotalSectors = total32;
    if (outReserved)     *outReserved = reserved;
    if (outFatSize32)    *outFatSize32 = fat32;
    return true;
}

inline bool ValidNtfsBoot(const uint8_t* sec, uint32_t sectorSize,
                          uint32_t* outSpc = nullptr,
                          uint64_t* outMftLcn = nullptr,
                          uint64_t* outMftMirrLcn = nullptr,
                          uint16_t* outReservedSectors = nullptr)
{
    if (sec[0x1FE] != 0x55 || sec[0x1FF] != 0xAA) return false;
    if (std::memcmp(sec + 0x03, "NTFS    ", 8) != 0) return false;
    const uint32_t bps = Le16(sec + 0x0B);
    if (!IsPow2(bps) || bps < 256 || bps > 4096) return false;
    const uint32_t spc = sec[0x0D];
    if (!IsPow2(spc) || spc == 0 || spc > 128) return false;
    const uint64_t mft = Le64(sec + 0x30);
    const uint64_t mirr = Le64(sec + 0x38);
    if (mft == 0 || mirr == 0) return false;

    if (outSpc)             *outSpc = spc;
    if (outMftLcn)          *outMftLcn = mft;
    if (outMftMirrLcn)      *outMftMirrLcn = mirr;
    if (outReservedSectors) *outReservedSectors = Le16(sec + 0x0E);
    return true;
}

inline bool ValidExtSuperblock(const uint8_t* sb, uint32_t* outBlockSize = nullptr)
{
    if (Le16(sb + 0x38) != 0xEF53) return false;                    // s_magic
    const uint32_t logBlock = Le32(sb + 0x18);                      // s_log_block_size
    if (logBlock > 6) return false;                                 // 1 KiB .. 64 KiB
    const uint32_t bpg = Le32(sb + 0x20);                           // s_blocks_per_group
    const uint32_t ipg = Le32(sb + 0x28);                           // s_inodes_per_group
    if (bpg == 0 || ipg == 0) return false;
    if (Le32(sb + 0x00) == 0) return false;                         // s_inodes_count
    const uint16_t inodeSize = Le16(sb + 0x58);
    if (inodeSize != 0 && (inodeSize < 128 || inodeSize > 1024 || !IsPow2(inodeSize))) return false;
    if (outBlockSize) *outBlockSize = 1024u << logBlock;
    return true;
}

// ---------------------------------------------------------------------------
// FAT32: rebuild the primary boot sector from the copy TestDisk calls
// "backup boot sector" -- sectors 6,7,8 of the volume (boot sector, FSInfo,
// reserved). Only the 3-sector window is copied; the rest of sector 0's tail
// (0x1FE signature) is part of the copied image.
// ---------------------------------------------------------------------------
inline RepairPlan PlanFat32BootFromBackup(const ByteReader& read,
                                          uint64_t partOffset,
                                          uint32_t sectorSize,
                                          uint64_t partSectors)
{
    RepairPlan plan;
    const uint64_t primaryLen = static_cast<uint64_t>(sectorSize) * 3;
    if (partSectors != 0 && partSectors * sectorSize < primaryLen + 6ull * sectorSize) return plan;

    std::vector<uint8_t> primary = ReadBytes(read, partOffset, static_cast<size_t>(primaryLen));
    if (primary.empty()) return plan;
    if (ValidFat32Boot(primary.data(), sectorSize)) return plan;    // nothing to repair

    std::vector<uint8_t> backup = ReadBytes(read, partOffset + 6ull * sectorSize,
                                            static_cast<size_t>(primaryLen));
    if (backup.empty()) return plan;
    if (!ValidFat32Boot(backup.data(), sectorSize)) return plan;

    // The backup must agree with the volume it sits in, otherwise we would
    // restore a stale geometry from a reformatted volume.
    uint32_t bspc = 0, breserved = 0, bfat = 0;
    uint64_t btotal = 0;
    ValidFat32Boot(backup.data(), sectorSize, &bspc, &btotal, &breserved, &bfat);
    if (partSectors != 0 && btotal != 0 && btotal > partSectors) return plan;

    plan.kind        = RepairKind::Fat32BootFromBackup;
    plan.offsetBytes = partOffset;
    plan.lengthBytes = static_cast<uint32_t>(primaryLen);
    plan.before      = primary;
    plan.after       = backup;
    plan.confidence  = 85;
    plan.reason      = "primary FAT32 boot sector invalid; backup at sector 6 is valid";
    plan.followUp    = "verify BPB geometry (spc/reserved/fat size) matches the volume before writing";
    return plan;
}

// ---------------------------------------------------------------------------
// FAT12/16/32: compare FAT#1 and FAT#2. TestDisk rebuilds a table only when
// one copy is self-consistent; we refuse to guess when both look plausible and
// simply differ, because either copy may be the newer one.
// ---------------------------------------------------------------------------
inline RepairPlan PlanFatTableMirror(const ByteReader& read,
                                     uint64_t partOffset,
                                     uint32_t sectorSize,
                                     uint32_t reservedSectors,
                                     uint32_t fatSizeSectors,
                                     uint32_t fatCount)
{
    RepairPlan plan;
    if (fatCount < 2 || fatSizeSectors == 0 || reservedSectors == 0) return plan;

    const uint64_t fatBytesAll = static_cast<uint64_t>(fatSizeSectors) * sectorSize;
    const uint64_t window = fatBytesAll < (256u * 1024u) ? fatBytesAll : (256ull * 1024ull);
    if (window < 512) return plan;

    const uint64_t fat1 = partOffset + static_cast<uint64_t>(reservedSectors) * sectorSize;
    const uint64_t fat2 = fat1 + fatBytesAll;
    std::vector<uint8_t> a = ReadBytes(read, fat1, static_cast<size_t>(window));
    std::vector<uint8_t> b = ReadBytes(read, fat2, static_cast<size_t>(window));
    if (a.empty() || b.empty()) return plan;
    if (std::memcmp(a.data(), b.data(), static_cast<size_t>(window)) == 0) return plan;

    // A FAT table's first byte is the media descriptor of the BPB and the
    // following bytes are 0xFF for FAT12/16 (0x0F/0xFF for FAT32 high nibble).
    auto plausible = [](const std::vector<uint8_t>& t) {
        return t.size() >= 4 && t[0] >= 0xF0 && t[1] == 0xFF && t[2] == 0xFF;
    };
    const bool okA = plausible(a);
    const bool okB = plausible(b);
    if (okA == okB) return plan;    // both or neither: refuse to choose

    plan.kind        = okB ? RepairKind::FatTableFromMirror : RepairKind::FatTableMirrorUpdate;
    plan.offsetBytes = okB ? fat1 : fat2;
    plan.lengthBytes = static_cast<uint32_t>(window);
    plan.before      = okB ? a : b;
    plan.after       = okB ? b : a;
    plan.confidence  = 60;
    plan.reason      = okB ? "FAT#1 head is implausible and FAT#2 is valid; copy FAT#2 over FAT#1"
                           : "FAT#2 head differs from a valid FAT#1; refresh the mirror";
    plan.followUp    = "a real repair also needs the full table length and free-cluster count check";
    return plan;
}

// ---------------------------------------------------------------------------
// NTFS: rebuild the boot sector from the backup stored in the volume's last
// sector (TestDisk recover_NTFS backup=1).
// ---------------------------------------------------------------------------
inline RepairPlan PlanNtfsBootFromBackup(const ByteReader& read,
                                         uint64_t partOffset,
                                         uint32_t sectorSize,
                                         uint64_t partSectors)
{
    RepairPlan plan;
    if (partSectors < 2) return plan;

    std::vector<uint8_t> primary = ReadBytes(read, partOffset, sectorSize);
    if (primary.empty()) return plan;
    if (ValidNtfsBoot(primary.data(), sectorSize)) return plan;

    std::vector<uint8_t> backup = ReadBytes(read,
        partOffset + (partSectors - 1) * sectorSize, sectorSize);
    if (backup.empty()) return plan;
    if (!ValidNtfsBoot(backup.data(), sectorSize)) return plan;

    uint32_t spc = 0;
    uint64_t mft = 0, mirr = 0;
    if (!ValidNtfsBoot(backup.data(), sectorSize, &spc, &mft, &mirr)) return plan;
    const uint64_t cluster = static_cast<uint64_t>(spc) * Le16(backup.data() + 0x0B);
    if (mft * cluster >= partSectors * static_cast<uint64_t>(sectorSize)) return plan;

    plan.kind        = RepairKind::NtfsBootFromBackup;
    plan.offsetBytes = partOffset;
    plan.lengthBytes = sectorSize;
    plan.before      = primary;
    plan.after       = backup;
    plan.confidence  = 85;
    plan.reason      = "primary NTFS boot sector invalid; backup in the last sector is valid";
    plan.followUp    = "check $MFT LCN still resolves inside the volume";
    return plan;
}

// ---------------------------------------------------------------------------
// NTFS: $MFT vs $MFTMirr. The mirror holds the first 4 records (or one
// cluster, whichever is larger). When exactly one side still starts with a
// "FILE" record, the other side is unusable and can be overwritten.
// Overwriting $MFT from the mirror is lossy (it destroys whatever newer
// records survived), so it is capped at low confidence and must never be
// auto-applied.
// ---------------------------------------------------------------------------
inline RepairPlan PlanNtfsMftMirror(const ByteReader& read,
                                    uint64_t partOffset,
                                    uint32_t sectorSize,
                                    const uint8_t* bootSector,
                                    bool exposeMirrorToMft)
{
    RepairPlan plan;
    if (bootSector == nullptr) return plan;
    uint32_t spc = 0;
    uint64_t mftLcn = 0, mirrLcn = 0;
    uint16_t reserved = 0;
    if (!ValidNtfsBoot(bootSector, sectorSize, &spc, &mftLcn, &mirrLcn, &reserved)) return plan;

    const uint32_t clusterSize = spc * Le16(bootSector + 0x0B);
    const int8_t recClusters = static_cast<int8_t>(bootSector[0x40]);     // clusters_per_mft_record
    int64_t recSize = 0;
    if (recClusters > 0) recSize = static_cast<int64_t>(recClusters) * clusterSize;
    else                 recSize = static_cast<int64_t>(1) << (-static_cast<int>(recClusters));
    if (recSize < 42 || recSize > 65536) return plan;

    const uint64_t want = static_cast<uint64_t>(recSize) * 4 > clusterSize
                        ? static_cast<uint64_t>(recSize) * 4
                        : clusterSize;
    const uint64_t mftOff  = partOffset + (static_cast<uint64_t>(reserved) + mftLcn)  * clusterSize;
    const uint64_t mirrOff = partOffset + (static_cast<uint64_t>(reserved) + mirrLcn) * clusterSize;

    std::vector<uint8_t> mft  = ReadBytes(read, mftOff, static_cast<size_t>(want));
    std::vector<uint8_t> mirr = ReadBytes(read, mirrOff, static_cast<size_t>(want));
    if (mft.empty() || mirr.empty()) return plan;
    if (std::memcmp(mft.data(), mirr.data(), static_cast<size_t>(want)) == 0) return plan;

    const bool mftOk  = std::memcmp(mft.data(), "FILE", 4) == 0;
    const bool mirrOk = std::memcmp(mirr.data(), "FILE", 4) == 0;

    if (mftOk && !mirrOk) {
        plan.kind        = RepairKind::NtfsMftMirrorFromMft;
        plan.offsetBytes = mirrOff;
        plan.before      = mirr;
        plan.after       = mft;
        plan.confidence  = 75;
        plan.reason      = "$MFTMirr does not start with a FILE record; $MFT does";
    } else if (!mftOk && mirrOk) {
        if (!exposeMirrorToMft) return plan;      // expert gate
        plan.kind        = RepairKind::NtfsMftFromMirror;
        plan.offsetBytes = mftOff;
        plan.before      = mft;
        plan.after       = mirr;
        plan.confidence  = 35;
        plan.reason      = "$MFT head is damaged; restoring it from $MFTMirr loses newer records";
    } else {
        return plan;                               // both bad, or both fine but divergent
    }
    plan.lengthBytes = static_cast<uint32_t>(want);
    plan.followUp    = "run chkdsk /f on a copy before trusting the repaired volume";
    return plan;
}

// ---------------------------------------------------------------------------
// ext2/3/4: locate an alternate superblock and offer it as the new primary.
// Group numbers carrying a backup are 3^i * 5^j * 7^k (same rule as
// e2fsprogs); each candidate is validated before use, and both in-block
// layouts (offset 0 and offset 1024) are probed because the placement differs
// between 1 KiB and larger block sizes.
// ---------------------------------------------------------------------------
inline bool ExtBackupGroupCandidate(uint32_t g)
{
    if (g == 0) return false;
    while (g % 3 == 0) g /= 3;
    while (g % 5 == 0) g /= 5;
    while (g % 7 == 0) g /= 7;
    return g == 1;
}

inline RepairPlan PlanExtSuperblockFromBackup(const ByteReader& read,
                                             uint64_t partOffset,
                                             uint64_t partBytes)
{
    RepairPlan plan;
    const uint64_t sbOff = partOffset + 1024;
    std::vector<uint8_t> primary = ReadBytes(read, sbOff, 1024);
    if (primary.empty()) return plan;
    if (ValidExtSuperblock(primary.data())) return plan;

    // Geometry can only come from the (damaged) primary or from a candidate.
    // Try the primary first; fall back to the first valid backup's own fields.
    uint32_t bpg = Le32(primary.data() + 0x20);
    uint32_t firstDataBlock = Le32(primary.data() + 0x14);
    const bool primaryHasGeometry = bpg != 0 && firstDataBlock <= 1;

    uint32_t candBlock = 1024;
    if (primaryHasGeometry)
        candBlock = 1024u << Le32(primary.data() + 0x18);

    std::vector<uint32_t> groups;
    if (bpg != 0) {
        const uint64_t totalBlocks = partBytes / (candBlock ? candBlock : 1024u);
        const uint32_t maxGroup = static_cast<uint32_t>(totalBlocks / bpg) + 2;
        for (uint32_t g = 1; g <= maxGroup && g <= 4096; ++g)
            if (ExtBackupGroupCandidate(g)) groups.push_back(g);
    } else {
        for (uint32_t g : {1u, 3u, 5u, 7u, 9u, 25u, 27u, 49u}) groups.push_back(g);
    }

    for (uint32_t g : groups) {
        if (bpg == 0) break;
        const uint64_t groupBlock = static_cast<uint64_t>(firstDataBlock) +
                                    static_cast<uint64_t>(g) * bpg;
        // Probe both known placements: start of the block and +1024.
        for (int variant = 0; variant < 2; ++variant) {
            const uint64_t base = partOffset + groupBlock * candBlock +
                                  (variant == 0 ? 0ull : 1024ull);
            if (base + 1024 > partOffset + partBytes) continue;
            std::vector<uint8_t> sb = ReadBytes(read, base, 1024);
            if (sb.empty()) continue;
            uint32_t bs = 0;
            if (!ValidExtSuperblock(sb.data(), &bs)) continue;
            if (bs != candBlock) continue;

            plan.kind        = RepairKind::ExtSuperblockFromBackup;
            plan.offsetBytes = sbOff;
            plan.lengthBytes = 1024;
            plan.before      = primary;
            plan.after       = sb;
            plan.confidence  = 80;
            plan.reason      = "primary ext superblock invalid; backup at group " +
                               std::to_string(g) + " validates";
            plan.followUp    = "restore of the superblock alone may leave a stale GDT; "
                               "run e2fsck -p -b <block> -B <blocksize> on the copy";
            return plan;
        }
    }
    return plan;
}

// Convenience: run every planner that applies to a volume and keep the
// highest-confidence plan. Cheap enough for the UI/CLI to call on demand.
struct BootRepairRequest
{
    uint64_t partOffset     = 0;
    uint64_t partSectors    = 0;      // 0 => unknown
    uint32_t sectorSize     = 512;
    bool     exposeMftFromMirror = false;
    bool     includeFatTableMirror = true;
};

inline RepairPlan PlanBootRepair(const ByteReader& read, const BootRepairRequest& req)
{
    std::vector<RepairPlan> plans;

    std::vector<uint8_t> first = ReadBytes(read, req.partOffset, req.sectorSize);
    if (first.empty()) return RepairPlan{};

    if (ValidNtfsBoot(first.data(), req.sectorSize)) {
        plans.push_back(PlanNtfsMftMirror(read, req.partOffset, req.sectorSize,
                                          first.data(), req.exposeMftFromMirror));
    } else {
        plans.push_back(PlanNtfsBootFromBackup(read, req.partOffset, req.sectorSize,
                                                req.partSectors));
    }

    plans.push_back(PlanFat32BootFromBackup(read, req.partOffset, req.sectorSize,
                                             req.partSectors));

    uint32_t spc = 0, reserved = 0, fatSize = 0;
    uint64_t total = 0;
    if (ValidFat32Boot(first.data(), req.sectorSize, &spc, &total, &reserved, &fatSize) &&
        req.includeFatTableMirror) {
        plans.push_back(PlanFatTableMirror(read, req.partOffset, req.sectorSize,
                                            reserved, fatSize, first[0x10]));
    }

    plans.push_back(PlanExtSuperblockFromBackup(read, req.partOffset,
                                                 req.partSectors * req.sectorSize));

    RepairPlan best;
    for (const RepairPlan& p : plans)
        if (p.IsValid() && p.confidence > best.confidence) best = p;
    return best;
}

inline const char* RepairKindName(RepairKind k)
{
    switch (k) {
    case RepairKind::None:                    return "none";
    case RepairKind::Fat32BootFromBackup:     return "fat32-boot-from-backup";
    case RepairKind::FatTableFromMirror:      return "fat-table-from-mirror";
    case RepairKind::FatTableMirrorUpdate:    return "fat-table-mirror-update";
    case RepairKind::NtfsBootFromBackup:      return "ntfs-boot-from-backup";
    case RepairKind::NtfsMftMirrorFromMft:    return "ntfs-mftmirr-from-mft";
    case RepairKind::NtfsMftFromMirror:       return "ntfs-mft-from-mftmirr";
    case RepairKind::ExtSuperblockFromBackup: return "ext-superblock-from-backup";
    }
    return "unknown";
}

} // namespace repair
} // namespace phoenix
