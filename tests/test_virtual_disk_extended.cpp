// A1-1b: VHDX / VMDK monolithicSparse / QCOW2 / VDI read-only mapping tests.
//
// Every fixture is a hand-built, in-memory image following the published
// container layouts. NO device handle is opened and NO disk is written.
// For layout verification see the spec references in ROADMAP_RECOVERY.md A1-1b.

#include "disk/VirtualDiskSource.h"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::cerr << "FAIL line " << __LINE__ << ": " #cond << "\n";   \
            return 1;                                                      \
        }                                                                  \
    } while (0)

using namespace phoenix;
using namespace phoenix::vds_detail;

static void Ensure(std::vector<uint8_t>& v, size_t n)
{
    if (v.size() < n) v.resize(n, 0);
}
static void PutSector(std::vector<uint8_t>& v, size_t off, uint8_t tag)
{
    Ensure(v, off + 512);
    for (size_t i = 0; i < 512; ++i) {
        v[off + i] = static_cast<uint8_t>(tag + (i & 0x1F));
    }
}
static bool SectorTag(const uint8_t* p, uint8_t tag)
{
    for (size_t i = 0; i < 512; ++i) {
        if (p[i] != static_cast<uint8_t>(tag + (i & 0x1F))) return false;
    }
    return true;
}
static bool SectorZero(const uint8_t* p)
{
    for (size_t i = 0; i < 512; ++i) if (p[i] != 0) return false;
    return true;
}
static void PutBytes(std::vector<uint8_t>& v, size_t off,
                     const uint8_t* data, size_t n)
{
    Ensure(v, off + n);
    std::memcpy(v.data() + off, data, n);
}
static void PutAscii(std::vector<uint8_t>& v, size_t off, const std::string& s)
{
    PutBytes(v, off, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// -------------------------------- VHDX ------------------------------------
// 0x00000  "vhdxfile" identifier
// 0x10000  header 1 ("head"), 0x20000 header 2 (unused)
// 0x30000  region table (BAT @2MiB, metadata @1MiB)
// 0x100000 metadata region (virtual size / file params / logical sector size)
// 0x200000 BAT (entry0 -> block @3MiB FULLY_PRESENT, entry1 -> NOT_PRESENT)
// 0x300000 payload block 0 (1 MiB)
static std::vector<uint8_t> BuildVhdx(int variant = 0)
{
    std::vector<uint8_t> v;
    Ensure(v, 0x400000);

    PutAscii(v, 0, "vhdxfile");
    PutAscii(v, 0x10000, "head");
    PutLe64(v.data() + 0x10008, 1);        // sequence number
    PutLe32(v.data() + 0x10042, 1);        // version = 1
    if (variant == 1) v[0x10000 + 48] = 0xA5;   // non-zero logGuid

    const uint8_t kBatGuid[16] = {
        0x66,0x77,0xC2,0x2D,0x23,0xF6,0x00,0x42,
        0x9D,0x64,0x11,0x5E,0x9B,0xFD,0x4A,0x08 };
    const uint8_t kMdGuid[16] = {
        0x06,0xA2,0x7C,0x8B,0x90,0x47,0x9A,0x4B,
        0xB8,0xFE,0x57,0x5F,0x05,0x0F,0x88,0x6E };
    PutAscii(v, 0x30000, "regi");
    PutLe32(v.data() + 0x30008, 2);        // entry count
    PutBytes(v, 0x30010, kBatGuid, 16);
    PutLe64(v.data() + 0x30020, 0x200000);
    PutLe32(v.data() + 0x30028, 0x100000);
    PutLe32(v.data() + 0x3002C, 1);        // required
    PutBytes(v, 0x30030, kMdGuid, 16);
    PutLe64(v.data() + 0x30040, 0x100000);
    PutLe32(v.data() + 0x30048, 0x100000);
    PutLe32(v.data() + 0x3004C, 1);        // required

    const uint8_t kSizeGuid[16] = {
        0x24,0x42,0xA5,0x2F,0x1B,0xCD,0x76,0x48,
        0xB2,0x11,0x5D,0xBE,0xD8,0x3B,0xF4,0xB8 };
    const uint8_t kParamsGuid[16] = {
        0x37,0x67,0xA1,0xCA,0x36,0xFA,0x43,0x4D,
        0xB3,0xB6,0x33,0xF0,0xAA,0x44,0xE7,0x6B };
    const uint8_t kSectorGuid[16] = {
        0x1D,0xBF,0x41,0x81,0x6F,0xA9,0x09,0x47,
        0xBA,0x47,0xF2,0x33,0xA8,0xFA,0xAB,0x5F };

    PutAscii(v, 0x100000, "metadata");
    v[0x10000A] = 3;                       // entry count (u16 LE)
    PutBytes(v, 0x100020, kSizeGuid, 16);
    PutLe32(v.data() + 0x100030, 0x400);
    PutLe32(v.data() + 0x100034, 8);
    PutBytes(v, 0x100040, kParamsGuid, 16);
    PutLe32(v.data() + 0x100050, 0x420);
    PutLe32(v.data() + 0x100054, 8);
    PutBytes(v, 0x100060, kSectorGuid, 16);
    PutLe32(v.data() + 0x100070, 0x440);
    PutLe32(v.data() + 0x100074, 4);

    PutLe64(v.data() + 0x100400, 0x200000);           // virtual size = 2 MiB
    PutLe32(v.data() + 0x100420, 0x100000);           // block size = 1 MiB
    PutLe32(v.data() + 0x100424, variant == 2 ? 0x2 : 0); // HasParent flag
    PutLe32(v.data() + 0x100440, variant == 3 ? 4096 : 512);

    PutLe64(v.data() + 0x200000, (3ULL << 20) | 6ULL);  // FULLY_PRESENT @3MiB
    PutLe64(v.data() + 0x200008, 0);                    // NOT_PRESENT
    for (int i = 0; i < 8; ++i) {
        PutSector(v, 0x300000 + static_cast<size_t>(i) * 512,
                  static_cast<uint8_t>(0x40 + i));
    }
    if (variant == 4) v[0x30000] = 'X';    // corrupt region table signature
    return v;
}

static int TestVhdx()
{
    {
        MemoryVirtualDiskReader r(BuildVhdx());
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::Ok);
        CHECK(s.Format() == VirtualDiskFormat::Vhdx);
        CHECK(s.Info().virtualBytes == 0x200000);
        CHECK(s.Info().blockSize == 0x100000);
        CHECK(s.Info().warnings.empty());
        CHECK(s.SectorCount() == 4096);

        uint8_t buf[2048] = {};
        CHECK(s.ReadSectors(0, 4, buf));
        for (int i = 0; i < 4; ++i) {
            CHECK(SectorTag(buf + i * 512, static_cast<uint8_t>(0x40 + i)));
        }
        CHECK(s.ReadSectors(100, 1, buf));     // allocated block, untouched area
        CHECK(SectorZero(buf));
        CHECK(s.ReadSectors(2048, 1, buf));    // NOT_PRESENT block -> zeros
        CHECK(SectorZero(buf));
        CHECK(!s.ReadSectors(4095, 2, buf));   // crosses end
    }
    {
        MemoryVirtualDiskReader r(BuildVhdx(1));
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::Ok);
        CHECK(s.Info().warnings.find("log journal") != std::string::npos);
    }
    {
        MemoryVirtualDiskReader r(BuildVhdx(2));
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::UnsupportedVariant);
        CHECK(s.Info().detail.find("parent") != std::string::npos);
    }
    {
        MemoryVirtualDiskReader r(BuildVhdx(3));
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::UnsupportedVariant);
        CHECK(s.Info().detail.find("4Kn") != std::string::npos);
    }
    {
        MemoryVirtualDiskReader r(BuildVhdx(4));
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::BadRegionTable);
    }
    return 0;
}

// -------------------------- VMDK monolithicSparse --------------------------
// sector 0  KDMV sparse extent header
// sector 1  embedded descriptor
// sector 2  grain table (512 GTEs)
// sector 3  grain directory (1 entry -> grain table @sector 2)
// sector 4  grain 0 (16 sectors)
// sector 20 grain 2 (16 sectors)
static std::vector<uint8_t> BuildVmdk(int variant = 0)
{
    std::vector<uint8_t> v;
    Ensure(v, 40 * 512);

    PutAscii(v, 0, "KDMV");
    PutLe32(v.data() + 4, variant == 1 ? 2 : 1);              // version
    PutLe32(v.data() + 8, variant == 1 ? 0x4 :
                           (variant == 2 ? (1u << 16) : 0));  // flags
    PutLe64(v.data() + 12, 64);                                // capacity (sectors)
    PutLe64(v.data() + 20, 16);                                // grainSize (sectors)
    PutLe64(v.data() + 28, 1);                                  // descriptorOffset
    PutLe64(v.data() + 36, 1);                                  // descriptorSize
    PutLe32(v.data() + 44, 512);                                // numGTEsPerGT
    PutLe64(v.data() + 48, 0);                                  // rgdOffset
    PutLe64(v.data() + 56,
            variant == 3 ? 0xFFFFFFFFFFFFFFFFULL : 3ULL);       // gdOffset
    PutLe64(v.data() + 64, 4);                                  // overHead
    v[72] = (variant == 5) ? 1 : 0;                             // uncleanShutdown
    v[73] = 0x0A; v[74] = 0x20; v[75] = 0x0D; v[76] = 0x0A;
    v[77] = (variant == 2) ? 1 : 0;                             // compressAlgorithm

    const std::string desc =
        "# Disk DescriptorFile\nversion=1\nCID=fffffffe\n"
        "createType=\"monolithicSparse\"\n# Extent description\n"
        "RW 64 SPARSE \"disk.vmdk\"\n# The Disk Data Base\n"
        "ddb.adapterType = \"lsilogic\"\n";
    PutAscii(v, 512, variant == 4 ? desc + "RW 64 FLAT \"disk-flat.vmdk\" 0\n" : desc);

    PutLe32(v.data() + 2 * 512, 4);            // GTE[0] -> grain @sector 4
    PutLe32(v.data() + 2 * 512 + 4, 0);        // GTE[1] unallocated
    PutLe32(v.data() + 2 * 512 + 8, 20);       // GTE[2] -> grain @sector 20
    PutLe32(v.data() + 2 * 512 + 12,
            variant == 1 ? 1 : 0);             // GTE[3] zeroed-grain / unallocated
    PutLe32(v.data() + 3 * 512, 2);            // GD[0] -> GT @sector 2

    for (int i = 0; i < 16; ++i) {
        PutSector(v, (4 + i) * 512, static_cast<uint8_t>(0x40 + i));
        PutSector(v, (20 + i) * 512, static_cast<uint8_t>(0x60 + i));
    }
    return v;
}

static int TestVmdk()
{
    {
        MemoryVirtualDiskReader r(BuildVmdk());
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::Ok);
        CHECK(s.Format() == VirtualDiskFormat::Vmdk);
        CHECK(s.Info().virtualBytes == 64 * 512);
        CHECK(s.Info().blockSize == 16 * 512);
        CHECK(s.Info().warnings.empty());
        CHECK(s.SectorCount() == 64);

        uint8_t buf[1024] = {};
        CHECK(s.ReadSectors(0, 2, buf));
        CHECK(SectorTag(buf, 0x40));
        CHECK(SectorTag(buf + 512, 0x41));
        CHECK(s.ReadSectors(16, 1, buf));      // unallocated grain
        CHECK(SectorZero(buf));
        CHECK(s.ReadSectors(32, 2, buf));      // second allocated grain
        CHECK(SectorTag(buf, 0x60));
        CHECK(SectorTag(buf + 512, 0x61));
        CHECK(s.ReadSectors(48, 1, buf));      // unallocated grain
        CHECK(SectorZero(buf));
        CHECK(!s.ReadSectors(63, 2, buf));
    }
    {
        MemoryVirtualDiskReader r(BuildVmdk(1));   // zeroed-grain GTE
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::Ok);
        uint8_t buf[512] = {0xCC};
        CHECK(s.ReadSectors(48, 1, buf));
        CHECK(SectorZero(buf));
    }
    {
        MemoryVirtualDiskReader r(BuildVmdk(2));   // compressed grains
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::UnsupportedVariant);
        CHECK(s.Info().detail.find("compress") != std::string::npos);
    }
    {
        MemoryVirtualDiskReader r(BuildVmdk(3));   // streamOptimized
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::UnsupportedVariant);
        CHECK(s.Info().detail.find("streamOptimized") != std::string::npos);
    }
    {
        MemoryVirtualDiskReader r(BuildVmdk(4));   // flat extent in descriptor
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::UnsupportedVariant);
    }
    {
        MemoryVirtualDiskReader r(BuildVmdk(5));   // unclean shutdown warning
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::Ok);
        CHECK(s.Info().warnings.find("unclean") != std::string::npos);
    }
    return 0;
}

// ------------------------------- QCOW2 -------------------------------------
// cluster 0 (0x0000) header, cluster 1 (0x1000) L1 table,
// cluster 2 (0x2000) L2 table, cluster 3 (0x3000) data.
static std::vector<uint8_t> BuildQcow2(int variant = 0)
{
    std::vector<uint8_t> v;
    Ensure(v, 0x4000);

    PutAscii(v, 0, std::string("QFI\xfb", 4));
    PutBe32(v.data() + 4, 3);                 // version 3
    PutBe64(v.data() + 8, variant == 1 ? 0x800 : 0);   // backing file offset
    PutBe32(v.data() + 16, variant == 1 ? 8 : 0);
    PutBe32(v.data() + 20, 12);                // cluster_bits = 12 -> 4096
    PutBe64(v.data() + 24, 8192);              // virtual size = 2 clusters
    PutBe32(v.data() + 32, variant == 2 ? 1 : 0);  // crypt_method
    PutBe32(v.data() + 36, 1);                 // l1_size
    PutBe64(v.data() + 40, 0x1000);            // l1_table_offset
    PutBe64(v.data() + 48, 0x3000);            // refcount_table_offset (unused)
    PutBe32(v.data() + 56, 1);
    PutBe32(v.data() + 60, variant == 5 ? 1 : 0);  // nb_snapshots
    PutBe64(v.data() + 64, 0);
    PutBe64(v.data() + 72, variant == 3 ? 0x10 : 0); // incompatible_features
    PutBe64(v.data() + 80, 0);
    PutBe64(v.data() + 88, 0);
    PutBe32(v.data() + 96, 4);                 // refcount_order
    PutBe32(v.data() + 100, 104);              // header_length

    PutBe64(v.data() + 0x1000, 0x2000);        // L1[0] -> L2 @0x2000
    PutBe64(v.data() + 0x2000,
            variant == 4 ? ((1ULL << 63) | 0x3000) : 0x3000);  // L2[0]
    PutBe64(v.data() + 0x2008, 0);             // L2[1] unallocated -> zeros
    for (int i = 0; i < 8; ++i) {
        PutSector(v, 0x3000 + static_cast<size_t>(i) * 512,
                  static_cast<uint8_t>(0x50 + i));
    }
    return v;
}

static int TestQcow2()
{
    {
        MemoryVirtualDiskReader r(BuildQcow2());
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::Ok);
        CHECK(s.Format() == VirtualDiskFormat::Qcow2);
        CHECK(s.Info().virtualBytes == 8192);
        CHECK(s.Info().blockSize == 4096);
        CHECK(s.Info().warnings.empty());
        CHECK(s.SectorCount() == 16);

        uint8_t buf[1024] = {};
        CHECK(s.ReadSectors(0, 2, buf));
        CHECK(SectorTag(buf, 0x50));
        CHECK(SectorTag(buf + 512, 0x51));
        CHECK(s.ReadSectors(8, 1, buf));       // unallocated cluster
        CHECK(SectorZero(buf));
        CHECK(!s.ReadSectors(15, 2, buf));
    }
    {
        MemoryVirtualDiskReader r(BuildQcow2(1));
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::UnsupportedVariant);
        CHECK(s.Info().detail.find("backing") != std::string::npos);
    }
    {
        MemoryVirtualDiskReader r(BuildQcow2(2));
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::UnsupportedVariant);
        CHECK(s.Info().detail.find("encrypt") != std::string::npos);
    }
    {
        MemoryVirtualDiskReader r(BuildQcow2(3));
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::UnsupportedVariant);
        CHECK(s.Info().detail.find("incompatible") != std::string::npos);
    }
    {
        MemoryVirtualDiskReader r(BuildQcow2(4));  // compressed cluster
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::Ok);
        uint8_t buf[512] = {};
        CHECK(!s.ReadSectors(0, 1, buf));          // refused, not misread
    }
    {
        MemoryVirtualDiskReader r(BuildQcow2(5));
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::Ok);
        CHECK(s.Info().warnings.find("snapshots") != std::string::npos);
    }
    return 0;
}

// -------------------------------- VDI --------------------------------------
// 0x000 header, 0x200 block map, 0x400 data block 0.
static std::vector<uint8_t> BuildVdi(int variant = 0)
{
    std::vector<uint8_t> v;
    Ensure(v, 0x400 + 4096);

    const uint8_t sig[4] = {0x7F, 0x10, 0xDA, 0xBE};
    PutBytes(v, 0x40, sig, 4);
    PutLe32(v.data() + 0x44, 0x00010001);     // version 1.1
    PutLe32(v.data() + 0x48, 0x190);          // header size
    PutLe32(v.data() + 0x4C, variant == 2 ? 4 : 1);   // image type
    PutLe32(v.data() + 0x50, 0);
    PutLe32(v.data() + 0x154, 0x200);         // blocks map offset
    PutLe32(v.data() + 0x158, 0x400);         // data offset
    PutLe32(v.data() + 0x168, variant == 3 ? 4096 : 512);
    PutLe64(v.data() + 0x170, 8192);          // disk size
    PutLe32(v.data() + 0x178, 4096);          // block data size
    PutLe32(v.data() + 0x17C, 0);             // block extra
    PutLe32(v.data() + 0x180, 2);             // blocks in image
    PutLe32(v.data() + 0x184, 1);             // blocks allocated

    PutLe32(v.data() + 0x200, 0);             // block 0 -> physical 0
    PutLe32(v.data() + 0x204,
            variant == 1 ? 0xFFFFFFFEu : 0xFFFFFFFFu);  // zero / unallocated
    for (int i = 0; i < 8; ++i) {
        PutSector(v, 0x400 + static_cast<size_t>(i) * 512,
                  static_cast<uint8_t>(0x70 + i));
    }
    return v;
}

static int TestVdi()
{
    {
        MemoryVirtualDiskReader r(BuildVdi());
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::Ok);
        CHECK(s.Format() == VirtualDiskFormat::Vdi);
        CHECK(s.Info().virtualBytes == 8192);
        CHECK(s.Info().blockSize == 4096);
        CHECK(s.SectorCount() == 16);

        uint8_t buf[1024] = {};
        CHECK(s.ReadSectors(0, 2, buf));
        CHECK(SectorTag(buf, 0x70));
        CHECK(SectorTag(buf + 512, 0x71));
        CHECK(s.ReadSectors(8, 1, buf));       // unallocated block
        CHECK(SectorZero(buf));
        CHECK(!s.ReadSectors(15, 2, buf));
    }
    {
        MemoryVirtualDiskReader r(BuildVdi(1));    // zero block marker
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::Ok);
        uint8_t buf[512] = {0xCC};
        CHECK(s.ReadSectors(8, 1, buf));
        CHECK(SectorZero(buf));
    }
    {
        MemoryVirtualDiskReader r(BuildVdi(2));    // differencing
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::UnsupportedVariant);
    }
    {
        MemoryVirtualDiskReader r(BuildVdi(3));    // 4Kn
        VirtualDiskSource s(r);
        CHECK(s.Open() == VirtualDiskOpenResult::UnsupportedVariant);
    }
    return 0;
}

int main()
{
    if (TestVhdx()  != 0) return 1;
    if (TestVmdk()  != 0) return 1;
    if (TestQcow2() != 0) return 1;
    if (TestVdi()   != 0) return 1;
    std::cout << "VirtualDiskSource A1-1b extended format tests: PASS\n";
    return 0;
}
