// TestDisk-alignment gate: partition-structure write-back and boot repair.
//
// TestDisk 7.2 is the reference for *behaviour*:
//   src/fat32.c    "Backup BS"  : sectors 6..8 -> sectors 0..2
//   src/ntfs_fix.c repair_MFT   : pick the surviving $MFT / $MFTMirr copy
//   src/ntfs.c     recover_NTFS : rebuild the boot sector from the last sector
//   src/ext2_sb.c               : alternate superblock at groups 3^i*5^j*7^k
//   src/parti386.c write_mbr_i386 / write_all_log_i386
//   src/partgptw.c write_part_gpt
//
// This gate keeps two things true:
//   1. every planner refuses to invent data (no valid source -> no plan), and
//   2. applying a plan to the buffer actually makes the structure valid again
//      when it is re-read by the same validator the product uses.
//
// Everything is in-memory: the planners take a ByteReader, so no temp files,
// no device handles, and no dependency on QSettings/drive layout.

#include "disk/PartitionTableWrite.h"
#include "repair/BootSectorRepair.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace phoenix;

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(cond)) {                                                      \
            ++g_fails;                                                      \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, (msg));     \
        }                                                                   \
    } while (0)

namespace {

using Buf = std::vector<uint8_t>;

repair::ByteReader ReaderFor(const Buf& buf)
{
    return [&buf](uint64_t off, uint8_t* dst, size_t len) -> bool {
        if (off + len > buf.size()) return false;
        std::memcpy(dst, buf.data() + off, len);
        return true;
    };
}

void ApplyPlan(const repair::RepairPlan& p, Buf& buf)
{
    if (!p.IsValid() || p.offsetBytes + p.lengthBytes > buf.size()) return;
    std::memcpy(buf.data() + p.offsetBytes, p.after.data(), p.lengthBytes);
}

void PutLe16(Buf& b, size_t off, uint16_t v) { b[off] = uint8_t(v); b[off + 1] = uint8_t(v >> 8); }
void PutLe32(Buf& b, size_t off, uint32_t v)
{
    b[off] = uint8_t(v); b[off + 1] = uint8_t(v >> 8);
    b[off + 2] = uint8_t(v >> 16); b[off + 3] = uint8_t(v >> 24);
}
void PutLe64(Buf& b, size_t off, uint64_t v)
{
    PutLe32(b, off, uint32_t(v));
    PutLe32(b, off + 4, uint32_t(v >> 32));
}

// A minimal but structurally valid FAT32 BPB (TestDisk's test_FAT equivalent).
void WriteFat32Boot(Buf& img, size_t off, uint32_t sectorSize,
                    uint32_t totalSectors, uint32_t fatSize, uint8_t spc)
{
    img[off + 0] = 0xEB; img[off + 1] = 0x58; img[off + 2] = 0x90;
    std::memcpy(&img[off + 3], "MSWIN4.1", 8);
    PutLe16(img, off + 0x0B, static_cast<uint16_t>(sectorSize));
    img[off + 0x0D] = spc;
    PutLe16(img, off + 0x0E, 32);        // reserved sectors
    img[off + 0x10] = 2;                 // two FATs
    PutLe16(img, off + 0x11, 0);         // root entries (FAT32: 0)
    PutLe16(img, off + 0x13, 0);         // total16     (FAT32: 0)
    img[off + 0x15] = 0xF8;              // media
    PutLe16(img, off + 0x16, 0);         // fat size 16
    PutLe32(img, off + 0x20, totalSectors);
    PutLe32(img, off + 0x24, fatSize);
    PutLe32(img, off + 0x2C, 2);         // root cluster
    PutLe16(img, off + 0x30, 1);         // FSInfo sector
    PutLe16(img, off + 0x32, 6);         // backup boot sector
    std::memcpy(&img[off + 0x52], "FAT32   ", 8);
    img[off + 0x1FE] = 0x55;
    img[off + 0x1FF] = 0xAA;
}

// A minimal but structurally valid NTFS boot sector.
void WriteNtfsBoot(Buf& img, size_t off, uint32_t sectorSize,
                   uint8_t spc, uint64_t mftLcn, uint64_t mftMirrLcn)
{
    img[off + 0] = 0xEB; img[off + 1] = 0x52; img[off + 2] = 0x90;
    std::memcpy(&img[off + 3], "NTFS    ", 8);
    PutLe16(img, off + 0x0B, static_cast<uint16_t>(sectorSize));
    img[off + 0x0D] = spc;
    PutLe16(img, off + 0x0E, 0);         // reserved sectors
    img[off + 0x15] = 0xF8;
    PutLe64(img, off + 0x28, 0x200000);  // total sectors
    PutLe64(img, off + 0x30, mftLcn);
    PutLe64(img, off + 0x38, mftMirrLcn);
    img[off + 0x40] = 0xF6;              // -10 -> 1024-byte records
    img[off + 0x1FE] = 0x55;
    img[off + 0x1FF] = 0xAA;
}

void WriteExtSuperblock(Buf& img, size_t off, uint32_t logBlock,
                        uint32_t blocksPerGroup, uint32_t inodesPerGroup,
                        uint32_t firstDataBlock)
{
    PutLe32(img, off + 0x00, 1024);              // s_inodes_count
    PutLe32(img, off + 0x04, 8192);              // s_blocks_count
    PutLe32(img, off + 0x14, firstDataBlock);    // s_first_data_block
    PutLe32(img, off + 0x18, logBlock);          // s_log_block_size
    PutLe32(img, off + 0x20, blocksPerGroup);
    PutLe32(img, off + 0x28, inodesPerGroup);
    PutLe16(img, off + 0x38, 0xEF53);
    PutLe16(img, off + 0x3A, 1);                 // s_state: clean
    PutLe16(img, off + 0x58, 256);               // s_inode_size
}

// ---------------------------------------------------------------------------
// 1. FAT32 boot sector: primary damaged, backup at sector 6 is the truth.
// ---------------------------------------------------------------------------
void TestFat32Boot()
{
    const uint32_t ss = 512;
    Buf img(4u * 1024u * 1024u, 0);
    WriteFat32Boot(img, 0, ss, 8192, 64, 8);
    WriteFat32Boot(img, 6u * ss, ss, 8192, 64, 8);
    Buf pristine = img;

    auto read = ReaderFor(img);
    CHECK(!repair::PlanFat32BootFromBackup(read, 0, ss, img.size() / ss).IsValid(),
          "intact FAT32 volume yields no repair plan");

    // Damage the primary BPB but leave the signature: a half-written sector.
    std::memset(img.data() + 0x0B, 0, 0x40);
    CHECK(!repair::ValidFat32Boot(img.data(), ss), "damaged FAT32 boot sector is rejected");

    repair::RepairPlan p = repair::PlanFat32BootFromBackup(read, 0, ss, img.size() / ss);
    CHECK(p.IsValid(), "FAT32 backup boot sector produces a plan");
    CHECK(p.kind == repair::RepairKind::Fat32BootFromBackup, "plan kind is backup->primary");
    CHECK(p.offsetBytes == 0 && p.lengthBytes == 3 * ss, "plan covers sectors 0..2");
    CHECK(p.confidence >= 80, "backup-sourced plan is high confidence");

    ApplyPlan(p, img);
    CHECK(repair::ValidFat32Boot(img.data(), ss), "primary boot sector validates after restore");
    CHECK(std::memcmp(img.data(), pristine.data(), 3 * ss) == 0,
          "restored bytes are byte-identical to the pre-damage image");
    CHECK(!repair::PlanFat32BootFromBackup(ReaderFor(img), 0, ss, img.size() / ss).IsValid(),
          "re-running the planner after the repair yields nothing");

    // No valid backup -> must refuse rather than invent geometry.
    Buf headless = pristine;
    std::memset(headless.data(), 0, 3 * ss);
    std::memset(headless.data() + 6u * ss, 0, 3 * ss);
    CHECK(!repair::PlanFat32BootFromBackup(ReaderFor(headless), 0, ss,
                                           headless.size() / ss).IsValid(),
          "no valid backup means no plan (never invent a BPB)");
}

// ---------------------------------------------------------------------------
// 2. FAT table mirror: only a self-consistent copy may overwrite the other.
// ---------------------------------------------------------------------------
void TestFatTableMirror()
{
    const uint32_t ss = 512;
    Buf img(4u * 1024u * 1024u, 0);
    WriteFat32Boot(img, 0, ss, 8192, 64, 8);
    const uint64_t fat1 = 32ull * ss;
    const uint64_t fat2 = fat1 + 64ull * ss;

    auto fillFat = [&](uint64_t off, bool valid) {
        std::memset(img.data() + off, valid ? 0xFF : 0x00, 4096);
        if (valid) { img[off] = 0xF8; img[off + 1] = 0xFF; img[off + 2] = 0xFF; }
    };

    fillFat(fat1, true);
    fillFat(fat2, false);
    repair::RepairPlan p = repair::PlanFatTableMirror(ReaderFor(img), 0, ss, 32, 64, 2);
    CHECK(p.IsValid(), "FAT#2 head implausible -> plan to refresh mirror");
    CHECK(p.kind == repair::RepairKind::FatTableMirrorUpdate, "plan kind refreshes FAT#2");
    CHECK(p.offsetBytes == fat2, "plan writes the mirror, not FAT#1");
    ApplyPlan(p, img);
    CHECK(std::memcmp(img.data() + fat1, img.data() + fat2, 4096) == 0, "FAT#2 refreshed from FAT#1");

    // Reverse: mirror is the only plausible copy.
    Buf rev = img;
    std::memset(rev.data() + fat1, 0, 4096);
    repair::RepairPlan q = repair::PlanFatTableMirror(ReaderFor(rev), 0, ss, 32, 64, 2);
    CHECK(q.IsValid() && q.kind == repair::RepairKind::FatTableFromMirror,
          "damaged FAT#1 is restored from a valid FAT#2");
    ApplyPlan(q, rev);
    CHECK(std::memcmp(rev.data() + fat1, rev.data() + fat2, 4096) == 0, "FAT#1 restored");

    // Both plausible but different -> refuse (either may be the newer one).
    Buf both = img;
    both[fat2 + 3] = 0x11;
    CHECK(!repair::PlanFatTableMirror(ReaderFor(both), 0, ss, 32, 64, 2).IsValid(),
          "two plausible but divergent FATs -> no plan");
}

// ---------------------------------------------------------------------------
// 3. NTFS boot sector: rebuild from the last sector of the volume.
// ---------------------------------------------------------------------------
void TestNtfsBoot()
{
    const uint32_t ss = 512;
    const uint64_t sectors = 16384;                       // 8 MiB volume
    Buf img(static_cast<size_t>(sectors * ss), 0);
    WriteNtfsBoot(img, 0, ss, 8, 4, 2);
    WriteNtfsBoot(img, static_cast<size_t>((sectors - 1) * ss), ss, 8, 4, 2);
    Buf pristine = img;

    CHECK(!repair::PlanNtfsBootFromBackup(ReaderFor(img), 0, ss, sectors).IsValid(),
          "intact NTFS volume yields no plan");

    std::memset(img.data(), 0, 0x40);
    img[0x1FE] = 0x55; img[0x1FF] = 0xAA;                 // signature survived
    CHECK(!repair::ValidNtfsBoot(img.data(), ss), "damaged NTFS boot sector is rejected");

    repair::RepairPlan p = repair::PlanNtfsBootFromBackup(ReaderFor(img), 0, ss, sectors);
    CHECK(p.IsValid() && p.kind == repair::RepairKind::NtfsBootFromBackup,
          "backup boot sector produces a plan");
    CHECK(p.offsetBytes == 0 && p.lengthBytes == ss, "plan rewrites sector 0 only");
    ApplyPlan(p, img);
    CHECK(repair::ValidNtfsBoot(img.data(), ss), "NTFS boot validates after restore");
    CHECK(std::memcmp(img.data(), pristine.data(), ss) == 0, "restored boot sector is byte-identical");

    Buf noBackup = pristine;
    std::memset(noBackup.data(), 0, 0x40);
    std::memset(noBackup.data() + (sectors - 1) * ss, 0, ss);
    CHECK(!repair::PlanNtfsBootFromBackup(ReaderFor(noBackup), 0, ss, sectors).IsValid(),
          "neither copy valid -> no plan");
}

// ---------------------------------------------------------------------------
// 4. NTFS $MFT vs $MFTMirr.
// ---------------------------------------------------------------------------
void TestNtfsMftMirror()
{
    const uint32_t ss = 512;
    Buf img(8u * 1024u * 1024u, 0);
    WriteNtfsBoot(img, 0, ss, 8, /*mft*/ 4, /*mirr*/ 2);
    const size_t cluster = 8 * ss;
    const size_t mftOff  = 4 * cluster;
    const size_t mirrOff = 2 * cluster;
    const size_t want    = 4096;                          // max(cluster, 4*1024)

    std::memcpy(img.data() + mftOff, "FILE", 4);
    std::memset(img.data() + mftOff + 4, 0xA5, want - 4);
    std::memset(img.data() + mirrOff, 0, want);           // mirror destroyed

    repair::RepairPlan p = repair::PlanNtfsMftMirror(ReaderFor(img), 0, ss,
                                                     img.data(), false);
    CHECK(p.IsValid() && p.kind == repair::RepairKind::NtfsMftMirrorFromMft,
          "$MFTMirr rebuilt from a valid $MFT");
    CHECK(p.offsetBytes == mirrOff && p.lengthBytes == want, "plan targets the mirror head");
    ApplyPlan(p, img);
    CHECK(std::memcmp(img.data() + mftOff, img.data() + mirrOff, want) == 0, "mirror matches $MFT");

    // Reverse direction must stay behind the expert gate.
    Buf rev = img;
    std::memcpy(rev.data() + mirrOff, "FILE", 4);
    std::memset(rev.data() + mftOff, 0, want);
    CHECK(!repair::PlanNtfsMftMirror(ReaderFor(rev), 0, ss, rev.data(), false).IsValid(),
          "$MFT-from-mirror is withheld unless the expert gate is open");
    repair::RepairPlan q = repair::PlanNtfsMftMirror(ReaderFor(rev), 0, ss, rev.data(), true);
    CHECK(q.IsValid() && q.kind == repair::RepairKind::NtfsMftFromMirror,
          "expert gate exposes the lossy direction");
    CHECK(q.confidence < 50, "lossy direction carries low confidence");
}

// ---------------------------------------------------------------------------
// 5. ext2/3/4: alternate superblock at group 1 (1 KiB blocks).
// ---------------------------------------------------------------------------
void TestExtSuperblock()
{
    const uint32_t blockSize = 1024;
    const uint32_t bpg = 1024;
    const uint64_t backupOff = static_cast<uint64_t>(1 + 1 * bpg) * blockSize + 1024;
    Buf img(4u * 1024u * 1024u, 0);

    WriteExtSuperblock(img, 1024, 0, bpg, 256, 1);
    WriteExtSuperblock(img, static_cast<size_t>(backupOff), 0, bpg, 256, 1);
    Buf pristine = img;

    CHECK(repair::ValidExtSuperblock(img.data() + 1024), "fixture superblock validates");
    CHECK(!repair::PlanExtSuperblockFromBackup(ReaderFor(img), 0, img.size()).IsValid(),
          "intact ext volume yields no plan");

    PutLe16(img, 1024 + 0x38, 0);                          // kill s_magic
    CHECK(!repair::ValidExtSuperblock(img.data() + 1024), "damaged superblock is rejected");

    repair::RepairPlan p = repair::PlanExtSuperblockFromBackup(ReaderFor(img), 0, img.size());
    CHECK(p.IsValid() && p.kind == repair::RepairKind::ExtSuperblockFromBackup,
          "alternate superblock produces a plan");
    CHECK(p.offsetBytes == 1024 && p.lengthBytes == 1024, "plan rewrites the 1 KiB primary SB");
    CHECK(p.reason.find("group 1") != std::string::npos, "plan names the source group");
    ApplyPlan(p, img);
    CHECK(repair::ValidExtSuperblock(img.data() + 1024), "primary SB validates after restore");
    CHECK(std::memcmp(img.data() + 1024, pristine.data() + 1024, 1024) == 0,
          "restored superblock is byte-identical");
}

// ---------------------------------------------------------------------------
// 6. MBR: build -> parse round-trip, plus the refusals.
// ---------------------------------------------------------------------------
void TestMbrBuild()
{
    ptable::Geometry g;
    std::vector<ptable::MbrEntry> parts = {
        {0x07, 2048,  8192, true,  false},
        {0x83, 10240, 4096, false, false},
        {0x0C, 14336, 2048, false, false},
    };
    CHECK(ptable::ValidateMbrEntries(parts).ok, "three disjoint primaries validate");

    Buf bootArea(440, 0x90);
    Buf mbr = ptable::BuildMbr(parts, bootArea, g);
    CHECK(mbr.size() == 512, "MBR sector is 512 bytes");
    CHECK(mbr[0x1FE] == 0x55 && mbr[0x1FF] == 0xAA, "MBR signature present");
    CHECK(std::memcmp(mbr.data(), bootArea.data(), 16) == 0, "boot area preserved");

    std::vector<ptable::ParsedMbrEntry> back = ptable::ParseMbrEntries(mbr.data(), mbr.size());
    CHECK(back.size() == 3, "three entries parse back");
    CHECK(back[0].type == 0x07 && back[0].startLba == 2048 && back[0].sectors == 8192,
          "first entry round-trips");
    CHECK(back[0].bootable && !back[1].bootable, "bootable flag round-trips");
    CHECK(back[2].type == 0x0C && back[2].startLba == 14336, "third entry round-trips");

    // CHS encoding of LBA 0 must be the canonical 0/0/1.
    ptable::Chs c0 = ptable::LbaToChs(0, g);
    CHECK(c0.head == 0 && (c0.sector & 0x3F) == 1 && c0.cylinder == 0, "LBA0 encodes as CHS 0/0/1");
    ptable::Chs big = ptable::LbaToChs(200000000ull, g);
    CHECK(big.cylinder == 0xFF && (big.sector & 0xC0) == 0xC0, "CHS saturates at 1023/254/63");

    // Refusals: overlap, zero length, >2 TiB.
    std::vector<ptable::MbrEntry> overlap = {
        {0x07, 2048, 8192, false, false},
        {0x83, 4096, 8192, false, false},
    };
    CHECK(!ptable::ValidateMbrEntries(overlap).ok, "overlapping primaries are refused");

    std::vector<ptable::MbrEntry> zero = {{0x07, 2048, 0, false, false}};
    CHECK(!ptable::ValidateMbrEntries(zero).ok, "zero-length partition is refused");

    std::vector<ptable::MbrEntry> huge = {{0x07, 2048, 0xFFFFFFFFull, false, false}};
    CHECK(!ptable::ValidateMbrEntries(huge).ok, ">2 TiB partition is refused by the MBR builder");
}

// ---------------------------------------------------------------------------
// 7. EBR chain: logical entry + link to the next EBR.
// ---------------------------------------------------------------------------
void TestEbrChain()
{
    ptable::Geometry g;
    ptable::MbrEntry logical{0x07, 63, 2048, false, true};
    Buf bootArea(440, 0);
    Buf ebr = ptable::BuildEbr(logical, 4096, true, 0x0F, bootArea, g);
    CHECK(ebr.size() == 512 && ebr[0x1FE] == 0x55, "EBR is signed");

    std::vector<ptable::ParsedMbrEntry> e = ptable::ParseMbrEntries(ebr.data(), ebr.size());
    CHECK(e.size() == 2, "EBR carries the logical entry and the link");
    CHECK(e[0].type == 0x07 && e[0].startLba == 63 && e[0].sectors == 2048,
          "logical entry is relative to its EBR");
    CHECK(e[1].type == 0x0F && e[1].startLba == 4096, "link points at the next EBR");

    Buf last = ptable::BuildEbr(logical, 0, false, 0x0F, bootArea, g);
    CHECK(ptable::ParseMbrEntries(last.data(), last.size()).size() == 1,
          "last EBR has no link entry");
}

// ---------------------------------------------------------------------------
// 8. GPT: primary + backup headers, CRCs, protective MBR, refusals.
// ---------------------------------------------------------------------------
void TestGptBuild()
{
    ptable::Geometry g;
    const uint64_t totalSectors = 1024ull * 1024ull;      // 512 MiB
    std::array<uint8_t, 16> diskGuid{};
    CHECK(ptable::ParseGuid("01234567-89AB-CDEF-0123-456789ABCDEF", diskGuid),
          "canonical GUID text parses");

    ptable::GptPartition a;
    CHECK(ptable::ParseGuid("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7", a.typeGuid),
          "Microsoft basic-data GUID parses");
    CHECK(ptable::ParseGuid("11111111-2222-3333-4444-555555555555", a.uniqueGuid),
          "unique GUID parses");
    a.firstLba = 2048;
    a.lastLba  = 206847;
    a.name     = u"System";

    ptable::GptPartition b = a;
    CHECK(ptable::ParseGuid("0FC63DAF-8483-4772-8E79-3D69D8477DE4", b.typeGuid),
          "Linux data GUID parses");
    b.firstLba = 206848;
    b.lastLba  = 411647;

    std::string err;
    ptable::GptLayout gpt = ptable::BuildGpt({a, b}, totalSectors, g, diskGuid, &err);
    CHECK(gpt.IsValid(), "GPT layout builds");
    CHECK(err.empty(), "no error reported for a valid layout");

    // Protective MBR: one 0xEE entry covering everything after LBA 0.
    std::vector<ptable::ParsedMbrEntry> prot = ptable::ParseMbrEntries(
        gpt.protectiveMbr.data(), gpt.protectiveMbr.size());
    CHECK(prot.size() == 1 && prot[0].type == 0xEE, "protective MBR holds a single 0xEE entry");
    CHECK(prot[0].startLba == 1 && prot[0].sectors == totalSectors - 1,
          "protective entry spans the whole disk");

    // Header CRC must match an independent recomputation with the field zeroed.
    auto headerCrcOk = [](Buf h) {
        const uint32_t stored = h[16] | (h[17] << 8) | (h[18] << 16) | (uint32_t(h[19]) << 24);
        h[16] = h[17] = h[18] = h[19] = 0;
        return ptable::Crc32(h.data(), 92) == stored;
    };
    CHECK(headerCrcOk(gpt.primaryHeader), "primary header CRC32 self-consistent");
    CHECK(headerCrcOk(gpt.backupHeader), "backup header CRC32 self-consistent");

    const uint32_t entriesCrc = ptable::Crc32(gpt.primaryEntries.data(), gpt.primaryEntries.size());
    const uint32_t storedEntries = gpt.primaryHeader[88] | (gpt.primaryHeader[89] << 8) |
                                   (gpt.primaryHeader[90] << 16) | (uint32_t(gpt.primaryHeader[91]) << 24);
    CHECK(entriesCrc == storedEntries, "entries CRC matches the header");
    CHECK(gpt.primaryEntries == gpt.backupEntries, "primary and backup entries are identical");

    auto readLe64 = [](const Buf& v, size_t off) {
        uint64_t x = 0;
        for (int i = 7; i >= 0; --i) x = (x << 8) | v[off + i];
        return x;
    };
    CHECK(readLe64(gpt.primaryHeader, 24) == 1, "primary myLba = 1");
    CHECK(readLe64(gpt.primaryHeader, 32) == totalSectors - 1, "primary altLba = last sector");
    CHECK(readLe64(gpt.backupHeader, 24) == totalSectors - 1, "backup myLba = last sector");
    CHECK(readLe64(gpt.backupHeader, 32) == 1, "backup altLba = 1");
    CHECK(readLe64(gpt.primaryHeader, 40) == 34, "first usable LBA = 34");
    CHECK(readLe64(gpt.backupHeader, 72) == totalSectors - 33, "backup entries at last-33");

    // Entry payload.
    const uint8_t* e0 = gpt.primaryEntries.data();
    uint64_t first = 0, last = 0;
    for (int i = 7; i >= 0; --i) first = (first << 8) | e0[32 + i];
    for (int i = 7; i >= 0; --i) last  = (last  << 8) | e0[40 + i];
    CHECK(first == a.firstLba && last == a.lastLba, "first GPT entry round-trips");
    CHECK(e0[56] == 'S' && e0[57] == 0 && e0[58] == 'y', "partition name is UTF-16LE");
    CHECK(std::memcmp(e0, a.typeGuid.data(), 16) == 0, "type GUID stored in on-disk order");

    // Refusals.
    ptable::GptPartition bad = a;
    bad.firstLba = 4;
    std::string err2;
    CHECK(!ptable::BuildGpt({bad}, totalSectors, g, diskGuid, &err2).IsValid(),
          "partition below first-usable LBA is refused");
    CHECK(!err2.empty(), "refusal carries a reason");

    ptable::GptPartition over = a;
    over.lastLba = totalSectors - 1;
    CHECK(!ptable::BuildGpt({over}, totalSectors, g, diskGuid).IsValid(),
          "partition overlapping the backup GPT is refused");

    CHECK(!ptable::BuildGpt({a}, 40, g, diskGuid).IsValid(), "disk too small for a GPT is refused");
}

} // namespace

int main()
{
    TestFat32Boot();
    TestFatTableMirror();
    TestNtfsBoot();
    TestNtfsMftMirror();
    TestExtSuperblock();
    TestMbrBuild();
    TestEbrChain();
    TestGptBuild();

    std::printf("%s: RepairAlign %d checks, %d failure(s)\n",
                g_fails == 0 ? "PASS" : "FAIL", g_checks, g_fails);
    return g_fails == 0 ? 0 : 8;
}
