#pragma once

// ---------------------------------------------------------------------------
// PartitionTableWrite -- byte builders for partition-structure write-back,
// aligned with TestDisk 7.2 (src/parti386.c write_mbr_i386 / write_all_log_i386,
// src/partgptw.c write_part_gpt).
//
// Same contract as repair/BootSectorRepair.h:
//   * header-only, zero I/O, zero Qt / Win32;
//   * functions build the *after image* of one sector / one region;
//   * nothing here decides to write. Writing goes through
//     WriteGuard -> WriteTransaction -> DeviceWriter, so a missing grant means
//     no bytes reach the device (A0 S1..S5 stay intact).
//
// Two deliberate differences from TestDisk, both safety-motivated:
//   1. This code never synthesises boot code. TestDisk embeds a small MBR
//      loader when the existing sector has no 0x55AA signature; we require the
//      caller to pass the 440-byte boot area it wants preserved (normally the
//      bytes read from the damaged disk). Inventing a loader would turn a
//      partition-table repair into a silent "your disk no longer boots" event.
//   2. Entries are validated (overlap, ordering, 2 TiB MBR limit, GPT usable
//      range) before a sector is produced. TestDisk lets the operator shoot
//      their own foot; we refuse to emit a table that cannot be correct.
// ---------------------------------------------------------------------------

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace phoenix {
namespace ptable {

// ---------------------------------------------------------------------------
// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) -- required by GPT headers
// and entries. Implemented bitwise: the largest input here is 16 KiB, so the
// table would only add static data for no measurable gain.
// ---------------------------------------------------------------------------
inline uint32_t Crc32(const void* data, size_t len)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

// ---------------------------------------------------------------------------
// Geometry and MBR entries
// ---------------------------------------------------------------------------
struct Geometry
{
    uint32_t sectorSize    = 512;
    uint32_t heads         = 255;
    uint32_t sectorsPerTrack = 63;
};

struct MbrEntry
{
    uint8_t  type      = 0x07;   // 0x07 NTFS/exFAT, 0x0C FAT32 LBA, 0x83 Linux, 0xEE GPT
    uint64_t startLba  = 0;
    uint64_t sectors   = 0;
    bool     bootable  = false;
    bool     isLogical = false;  // informational; BuildMbr ignores logical entries
};

// CHS encoding as written by every PC BIOS tool: cylinder 0..1023, head
// 0..254, sector 1..63, saturated at the 1023/254/63 ceiling.
struct Chs { uint8_t head = 0; uint8_t sector = 1; uint8_t cylinder = 0; };

inline Chs LbaToChs(uint64_t lba, const Geometry& g)
{
    Chs c;
    const uint64_t spt = g.sectorsPerTrack ? g.sectorsPerTrack : 63;
    const uint64_t heads = g.heads ? g.heads : 255;
    uint64_t cyl = lba / (spt * heads);
    uint64_t head = (lba / spt) % heads;
    uint64_t sec = (lba % spt) + 1;
    if (cyl > 1023) { cyl = 1023; head = heads - 1; sec = spt; }
    c.head     = static_cast<uint8_t>(head);
    c.sector   = static_cast<uint8_t>((sec & 0x3F) | ((cyl >> 2) & 0xC0));
    c.cylinder = static_cast<uint8_t>(cyl & 0xFF);
    return c;
}

inline void WriteEntry(uint8_t* dst, const MbrEntry& e, uint64_t baseLba, const Geometry& g)
{
    std::memset(dst, 0, 16);
    const Chs start = LbaToChs(baseLba + e.startLba, g);
    const Chs end   = LbaToChs(baseLba + e.startLba + (e.sectors ? e.sectors - 1 : 0), g);
    dst[0] = e.bootable ? 0x80 : 0x00;
    dst[1] = start.head; dst[2] = start.sector; dst[3] = start.cylinder;
    dst[4] = e.type;
    dst[5] = end.head;   dst[6] = end.sector;   dst[7] = end.cylinder;
    const uint32_t lba = static_cast<uint32_t>(e.startLba);
    const uint32_t cnt = static_cast<uint32_t>(e.sectors);
    std::memcpy(dst + 8,  &lba, 4);
    std::memcpy(dst + 12, &cnt, 4);
}

struct MbrValidation
{
    bool        ok = true;
    std::string reason;
};

inline MbrValidation ValidateMbrEntries(const std::vector<MbrEntry>& primaries)
{
    MbrValidation v;
    if (primaries.size() > 4) { v.ok = false; v.reason = "MBR holds at most 4 primary entries"; return v; }
    std::vector<const MbrEntry*> written;
    for (const MbrEntry& e : primaries) {
        if (e.isLogical) continue;
        if (e.sectors == 0) { v.ok = false; v.reason = "zero-length partition"; return v; }
        if (e.type == 0)    { v.ok = false; v.reason = "empty type byte in a written entry"; return v; }
        const uint64_t last = e.startLba + e.sectors;
        if (last > 0xFFFFFFFFull) { v.ok = false; v.reason = "partition exceeds the 2 TiB MBR address limit"; return v; }
        written.push_back(&e);
    }
    const int n = static_cast<int>(written.size());
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j) {
            const uint64_t a0 = written[i]->startLba, a1 = a0 + written[i]->sectors;
            const uint64_t b0 = written[j]->startLba, b1 = b0 + written[j]->sectors;
            if (a0 < b1 && b0 < a1) { v.ok = false; v.reason = "overlapping primary partitions"; return v; }
        }
    return v;
}

// Build one 512-byte MBR. `bootArea` is the first 440 bytes to preserve
// (pass the bytes read from the disk being repaired); entries are placed at
// 0x1BE and the 0x55AA signature at 0x1FE.
inline std::vector<uint8_t> BuildMbr(const std::vector<MbrEntry>& primaries,
                                     const std::vector<uint8_t>& bootArea,
                                     const Geometry& g)
{
    std::vector<uint8_t> sec(g.sectorSize ? g.sectorSize : 512, 0);
    if (bootArea.size() >= 440) std::memcpy(sec.data(), bootArea.data(), 440);
    int slot = 0;
    for (const MbrEntry& e : primaries) {
        if (e.isLogical) continue;
        if (slot >= 4) break;
        WriteEntry(sec.data() + 0x1BE + slot * 16, e, 0, g);
        ++slot;
    }
    // A GPT protective MBR reserves 0xEE; never mix it with real entries.
    sec[0x1FE] = 0x55;
    sec[0x1FF] = 0xAA;
    return sec;
}

// Build one EBR. The first entry describes `logical` (its start is relative to
// this EBR, as the on-disk format requires); the second links to the next EBR
// with type 0x0F (LBA) or 0x05 (CHS).
inline std::vector<uint8_t> BuildEbr(const MbrEntry& logical,
                                     uint64_t nextEbrLba,
                                     bool hasNext,
                                     uint8_t linkType,
                                     const std::vector<uint8_t>& bootArea,
                                     const Geometry& g)
{
    std::vector<uint8_t> sec(g.sectorSize ? g.sectorSize : 512, 0);
    if (bootArea.size() >= 440) std::memcpy(sec.data(), bootArea.data(), 440);
    WriteEntry(sec.data() + 0x1BE, logical, 0, g);
    if (hasNext) {
        MbrEntry link;
        link.type     = linkType ? linkType : 0x0F;
        link.startLba = nextEbrLba;
        link.sectors  = 1;
        WriteEntry(sec.data() + 0x1BE + 16, link, 0, g);
    }
    sec[0x1FE] = 0x55;
    sec[0x1FF] = 0xAA;
    return sec;
}

// ---------------------------------------------------------------------------
// GPT
// ---------------------------------------------------------------------------
struct GptPartition
{
    std::array<uint8_t, 16> typeGuid{};
    std::array<uint8_t, 16> uniqueGuid{};
    uint64_t firstLba = 0;
    uint64_t lastLba  = 0;
    uint64_t attributes = 0;
    std::u16string name;           // up to 36 UTF-16 code units
};

// Canonical text form "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" -> on-disk bytes
// (first three groups little-endian, last two big-endian).
inline bool ParseGuid(const char* text, std::array<uint8_t, 16>& out)
{
    if (text == nullptr) return false;
    uint8_t raw[16] = {0};
    int nibbles = 0;
    for (const char* p = text; *p; ++p) {
        if (*p == '-') continue;
        int v;
        if (*p >= '0' && *p <= '9') v = *p - '0';
        else if (*p >= 'a' && *p <= 'f') v = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') v = *p - 'A' + 10;
        else return false;
        if (nibbles >= 32) return false;
        if ((nibbles & 1) == 0) raw[nibbles / 2] = static_cast<uint8_t>(v << 4);
        else raw[nibbles / 2] |= static_cast<uint8_t>(v);
        ++nibbles;
    }
    if (nibbles != 32) return false;
    // raw[] holds big-endian text order
    out[0] = raw[3]; out[1] = raw[2]; out[2] = raw[1]; out[3] = raw[0];
    out[4] = raw[5]; out[5] = raw[4];
    out[6] = raw[7]; out[7] = raw[6];
    for (int i = 8; i < 16; ++i) out[i] = raw[i];
    return true;
}

inline void WriteLe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void WriteLe32(uint8_t* p, uint32_t v)
{
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline void WriteLe64(uint8_t* p, uint64_t v)
{
    WriteLe32(p, uint32_t(v));
    WriteLe32(p + 4, uint32_t(v >> 32));
}

struct GptLayout
{
    std::vector<uint8_t> protectiveMbr;
    std::vector<uint8_t> primaryEntries;
    std::vector<uint8_t> primaryHeader;
    std::vector<uint8_t> backupEntries;
    std::vector<uint8_t> backupHeader;
    uint32_t             entryCount  = 128;
    uint32_t             entrySize   = 128;
    uint64_t             entriesLba  = 2;
    uint64_t             backupEntriesLba = 0;
    uint64_t             backupHeaderLba  = 0;

    bool IsValid() const
    {
        return !primaryHeader.empty() && !backupHeader.empty() &&
               primaryEntries.size() == backupEntries.size();
    }
};

// Build a complete GPT: protective MBR, primary entries + header at LBA 2/1,
// backup entries + header in the last 33 sectors. `totalSectors` is the whole
// disk (not the partition). Mirrors TestDisk's write_part_gpt layout.
inline GptLayout BuildGpt(const std::vector<GptPartition>& parts,
                          uint64_t totalSectors,
                          const Geometry& g,
                          const std::array<uint8_t, 16>& diskGuid,
                          std::string* error = nullptr)
{
    GptLayout out;
    const uint32_t ss = g.sectorSize ? g.sectorSize : 512;
    const uint32_t entryCount = 128;
    const uint32_t entrySize  = 128;
    const uint64_t entriesBytes = static_cast<uint64_t>(entryCount) * entrySize;
    const uint64_t entriesSectors = (entriesBytes + ss - 1) / ss;

    if (totalSectors < entriesSectors * 2 + 34) {
        if (error) *error = "disk too small for a GPT";
        return out;
    }

    const uint64_t firstUsable = 2 + entriesSectors;
    const uint64_t lastUsable  = totalSectors - entriesSectors - 2;
    const uint64_t backupEntriesLba = totalSectors - entriesSectors - 1;
    const uint64_t backupHeaderLba  = totalSectors - 1;

    // -- entries ---------------------------------------------------------
    std::vector<uint8_t> entries(static_cast<size_t>(entriesBytes), 0);
    size_t slot = 0;
    for (const GptPartition& p : parts) {
        if (slot >= entryCount) {
            if (error) *error = "more than 128 GPT partitions";
            return out;
        }
        if (p.firstLba < firstUsable || p.lastLba > lastUsable || p.lastLba < p.firstLba) {
            if (error) *error = "partition outside the GPT usable range";
            return out;
        }
        uint8_t* e = entries.data() + slot * entrySize;
        std::memcpy(e, p.typeGuid.data(), 16);
        std::memcpy(e + 16, p.uniqueGuid.data(), 16);
        WriteLe64(e + 32, p.firstLba);
        WriteLe64(e + 40, p.lastLba);
        WriteLe64(e + 48, p.attributes);
        const size_t chars = p.name.size() < 36 ? p.name.size() : 36;
        for (size_t i = 0; i < chars; ++i) {
            const char16_t c = p.name[i];
            e[56 + i * 2]     = static_cast<uint8_t>(c & 0xFF);
            e[56 + i * 2 + 1] = static_cast<uint8_t>((c >> 8) & 0xFF);
        }
        ++slot;
    }
    const uint32_t entriesCrc = Crc32(entries.data(), entries.size());

    auto buildHeader = [&](uint64_t myLba, uint64_t altLba,
                           uint64_t entryLba) {
        std::vector<uint8_t> h(static_cast<size_t>(ss), 0);
        std::memcpy(h.data(), "EFI PART", 8);
        WriteLe32(h.data() + 8,  0x00010000u);   // revision 1.0
        WriteLe32(h.data() + 12, 92);            // header size
        WriteLe64(h.data() + 24, myLba);
        WriteLe64(h.data() + 32, altLba);
        WriteLe64(h.data() + 40, firstUsable);
        WriteLe64(h.data() + 48, lastUsable);
        std::memcpy(h.data() + 56, diskGuid.data(), 16);
        WriteLe64(h.data() + 72, entryLba);
        WriteLe32(h.data() + 80, entryCount);
        WriteLe32(h.data() + 84, entrySize);
        WriteLe32(h.data() + 88, entriesCrc);
        const uint32_t hcrc = Crc32(h.data(), 92);
        WriteLe32(h.data() + 16, hcrc);
        return h;
    };

    out.protectiveMbr = std::vector<uint8_t>(ss, 0);
    {
        MbrEntry prot;
        prot.type     = 0xEE;
        prot.startLba = 1;
        prot.sectors  = totalSectors - 1;
        WriteEntry(out.protectiveMbr.data() + 0x1BE, prot, 0, g);
        out.protectiveMbr[0x1FE] = 0x55;
        out.protectiveMbr[0x1FF] = 0xAA;
    }

    out.primaryEntries    = entries;
    out.backupEntries     = entries;
    out.primaryHeader     = buildHeader(1, backupHeaderLba, 2);
    out.backupHeader      = buildHeader(backupHeaderLba, 1, backupEntriesLba);
    out.entryCount        = entryCount;
    out.entrySize         = entrySize;
    out.entriesLba        = 2;
    out.backupEntriesLba  = backupEntriesLba;
    out.backupHeaderLba   = backupHeaderLba;
    return out;
}

// ---------------------------------------------------------------------------
// Read-back helpers (used by the gate, and by the UI when it has to show what
// it is about to write).
// ---------------------------------------------------------------------------
struct ParsedMbrEntry
{
    uint8_t  type = 0;
    bool     bootable = false;
    uint32_t startLba = 0;
    uint32_t sectors  = 0;
};

inline std::vector<ParsedMbrEntry> ParseMbrEntries(const uint8_t* sec, size_t len)
{
    std::vector<ParsedMbrEntry> out;
    if (len < 512 || sec[0x1FE] != 0x55 || sec[0x1FF] != 0xAA) return out;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = sec + 0x1BE + i * 16;
        if (e[4] == 0) continue;
        ParsedMbrEntry p;
        p.bootable = (e[0] == 0x80);
        p.type     = e[4];
        std::memcpy(&p.startLba, e + 8, 4);
        std::memcpy(&p.sectors,  e + 12, 4);
        out.push_back(p);
    }
    return out;
}

} // namespace ptable
} // namespace phoenix
