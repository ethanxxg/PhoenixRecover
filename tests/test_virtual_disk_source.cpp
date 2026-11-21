// A1-1a: read-only virtual disk sector mapping tests.
//
// All format fixtures are built in memory (MemoryVirtualDiskReader), except
// one small FileVirtualDiskReader smoke test that writes/removes a temp file.
// NO device handle is opened and NO source disk is ever written.

#include "disk/VirtualDiskSource.h"

#include <cstdio>
#include <cstring>
#include <fstream>
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

static void PutSector(std::vector<uint8_t>& v, size_t offset, uint8_t tag)
{
    if (v.size() < offset + 512) v.resize(offset + 512, 0);
    for (size_t i = 0; i < 512; ++i) {
        v[offset + i] = static_cast<uint8_t>(tag + (i & 0x1F));
    }
}

static bool SectorHasTag(const uint8_t* p, uint8_t tag)
{
    for (size_t i = 0; i < 512; ++i) {
        if (p[i] != static_cast<uint8_t>(tag + (i & 0x1F))) return false;
    }
    return true;
}

static bool SectorIsZero(const uint8_t* p)
{
    for (size_t i = 0; i < 512; ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}

static void AppendFooter(std::vector<uint8_t>& v, uint64_t currentSize,
                         uint32_t diskType, uint64_t dataOffset)
{
    const size_t off = v.size();
    v.resize(off + 512, 0);
    uint8_t* f = v.data() + off;
    std::memcpy(f, "conectix", 8);
    PutBe32(f + 0x0C, 0x00010000);          // file format version 1.0
    PutBe64(f + 0x10, dataOffset);
    PutBe64(f + 0x24, currentSize);          // original size
    PutBe64(f + 0x30, currentSize);          // current size
    PutBe32(f + 0x3C, diskType);             // 2=fixed, 3=dynamic, 4=differencing
}

static std::vector<uint8_t> RawImage(int sectors, bool trailingBytes = false)
{
    std::vector<uint8_t> v;
    for (int i = 0; i < sectors; ++i) {
        PutSector(v, static_cast<size_t>(i) * 512,
                  static_cast<uint8_t>(0x10 + i));
    }
    if (trailingBytes) v.resize(v.size() + 100, 0xFE);
    return v;
}

static int TestRaw()
{
    MemoryVirtualDiskReader r(RawImage(4));
    VirtualDiskSource src(r);
    CHECK(src.Open() == VirtualDiskOpenResult::Ok);
    CHECK(src.IsOpen());
    CHECK(src.Format() == VirtualDiskFormat::Raw);
    CHECK(src.Info().virtualBytes == 2048);
    CHECK(src.Info().sectorSize == 512);
    CHECK(src.SectorCount() == 4);

    uint8_t buf[1024] = {};
    CHECK(src.ReadSectors(1, 2, buf));
    CHECK(SectorHasTag(buf, 0x11));
    CHECK(SectorHasTag(buf + 512, 0x12));
    CHECK(src.ReadSectors(3, 1, buf));
    CHECK(!src.ReadSectors(3, 2, buf));       // crosses end
    CHECK(!src.ReadSectors(4, 1, buf));       // starts past end
    CHECK(!src.ReadSectors(0, 0, buf));       // zero count rejected

    // A partial final sector is ignored for geometry but still readable data.
    MemoryVirtualDiskReader partial(RawImage(4, true));
    VirtualDiskSource p(partial);
    CHECK(p.Open() == VirtualDiskOpenResult::Ok);
    CHECK(p.Format() == VirtualDiskFormat::Raw);
    CHECK(p.SectorCount() == 4);
    CHECK(p.ReadSectors(3, 1, buf));
    return 0;
}

static int TestVhdFixed()
{
    std::vector<uint8_t> v = RawImage(4);
    AppendFooter(v, 2048, 2, 0xFFFFFFFFFFFFFFFFULL);

    MemoryVirtualDiskReader r(std::move(v));
    VirtualDiskSource src(r);
    CHECK(src.Open() == VirtualDiskOpenResult::Ok);
    CHECK(src.Format() == VirtualDiskFormat::VhdFixed);
    CHECK(src.Info().virtualBytes == 2048);
    CHECK(src.SectorCount() == 4);

    uint8_t buf[2048] = {};
    CHECK(src.ReadSectors(0, 4, buf));
    for (int i = 0; i < 4; ++i) {
        CHECK(SectorHasTag(buf + i * 512, static_cast<uint8_t>(0x10 + i)));
    }
    CHECK(!src.ReadSectors(4, 1, buf));

    // Declared size larger than the file -> Truncated, never silently accepted.
    std::vector<uint8_t> shortV = RawImage(2);
    AppendFooter(shortV, 4096, 2, 0xFFFFFFFFFFFFFFFFULL);
    MemoryVirtualDiskReader r2(std::move(shortV));
    VirtualDiskSource s2(r2);
    CHECK(s2.Open() == VirtualDiskOpenResult::Truncated);
    CHECK(!s2.IsOpen());
    return 0;
}

// Dynamic VHD layout used by this fixture:
//   0..511      footer copy (also exercises "conectix at start" detection)
//   512..1535   dynamic header (cxsparse), tableOffset=1536, 2 entries, 4096B blocks
//   1536..2047  BAT: block0 -> sector 4, block1 -> 0xFFFFFFFF (sparse)
//   2048..2559  block0 bitmap (bits 0 and 2 set)
//   2560..6655  block0 data (8 sectors)
//   6656..7167  footer at EOF
static std::vector<uint8_t> DynamicVhd(bool corruptBatOffset = false)
{
    std::vector<uint8_t> v(1536, 0);
    std::vector<uint8_t> footerCopy;
    AppendFooter(footerCopy, 8192, 3, 512);
    std::memcpy(v.data(), footerCopy.data(), 512);   // footer copy at offset 0

    uint8_t* dyn = v.data() + 512;
    std::memcpy(dyn, "cxsparse", 8);
    PutBe64(dyn + 0x10, corruptBatOffset ? 999999ULL : 1536ULL);
    PutBe32(dyn + 0x18, 0x00010000);
    PutBe32(dyn + 0x1C, 2);          // maxTableEntries
    PutBe32(dyn + 0x20, 4096);       // blockSize = 8 sectors

    v.resize(2048, 0);                         // BAT occupies sector 3
    PutBe32(v.data() + 1536, 4);               // BAT[0] -> sector 4
    PutBe32(v.data() + 1540, 0xFFFFFFFFu);     // BAT[1] -> sparse

    v.resize(2560, 0);               // 512-byte bitmap sector
    v[2048] = 0xA0;                  // bits 0 and 2 allocated
    for (int i = 0; i < 8; ++i) {
        PutSector(v, 2560 + static_cast<size_t>(i) * 512,
                  static_cast<uint8_t>(0x30 + i));
    }
    AppendFooter(v, 8192, 3, 512);   // real footer at EOF
    return v;
}

static int TestVhdDynamic()
{
    MemoryVirtualDiskReader r(DynamicVhd());
    VirtualDiskSource src(r);
    CHECK(src.Open() == VirtualDiskOpenResult::Ok);
    CHECK(src.Format() == VirtualDiskFormat::VhdDynamic);
    CHECK(src.Info().virtualBytes == 8192);
    CHECK(src.Info().blockSize == 4096);
    CHECK(src.Info().batEntries == 2);
    CHECK(src.SectorCount() == 16);

    std::vector<uint8_t> buf(16 * 512, 0xCC);
    CHECK(src.ReadSectors(0, 16, buf.data()));
    CHECK(SectorHasTag(buf.data(), 0x30));            // allocated
    CHECK(SectorIsZero(buf.data() + 512));            // bitmap bit clear
    CHECK(SectorHasTag(buf.data() + 2 * 512, 0x32));  // allocated
    for (int i = 3; i < 16; ++i) {
        CHECK(SectorIsZero(buf.data() + static_cast<size_t>(i) * 512));
    }
    CHECK(!src.ReadSectors(15, 2, buf.data()));       // crosses end

    // Invalid BAT offset is rejected instead of reading out of bounds.
    MemoryVirtualDiskReader bad(DynamicVhd(true));
    VirtualDiskSource s2(bad);
    CHECK(s2.Open() == VirtualDiskOpenResult::BadBat);
    CHECK(!s2.IsOpen());

    // Differencing disks need a parent image; never guess.
    std::vector<uint8_t> diff = RawImage(2);
    AppendFooter(diff, 1024, 4, 512);
    MemoryVirtualDiskReader r3(std::move(diff));
    VirtualDiskSource s3(r3);
    CHECK(s3.Open() == VirtualDiskOpenResult::UnsupportedFormat);
    CHECK(std::string(s3.Info().detail).find("parent") != std::string::npos);
    return 0;
}

static VirtualDiskOpenResult OpenWithHead(std::vector<uint8_t> head,
                                         VirtualDiskFormat& out)
{
    if (!head.empty() && head.size() < 1024) head.resize(1024, 0);
    MemoryVirtualDiskReader r(std::move(head));
    VirtualDiskSource src(r);
    const VirtualDiskOpenResult res = src.Open();
    out = src.Format();
    return res;
}

static int TestDetectedButUnsupported()
{
    VirtualDiskFormat f = VirtualDiskFormat::Unknown;

    // Signatures are now routed to real parsers. Truncated / garbage headers
    // must be rejected with a specific reason, never treated as RAW.
    CHECK(OpenWithHead({'v','h','d','x','f','i','l','e'}, f) ==
          VirtualDiskOpenResult::Truncated);
    CHECK(f == VirtualDiskFormat::Vhdx);

    CHECK(OpenWithHead({'Q','F','I',0xFB}, f) ==
          VirtualDiskOpenResult::UnsupportedVariant);
    CHECK(f == VirtualDiskFormat::Qcow2);

    CHECK(OpenWithHead({'K','D','M','V'}, f) ==
          VirtualDiskOpenResult::BadHeader);
    CHECK(f == VirtualDiskFormat::Vmdk);

    // A standalone VMDK descriptor references companion extent files.
    const std::string desc =
        "# Disk DescriptorFile\nversion=1\ncreateType=\"monolithicFlat\"\n"
        "# Extent description\nRW 2048 FLAT \"disk-flat.vmdk\" 0\n";
    std::vector<uint8_t> descBytes(desc.begin(), desc.end());
    descBytes.resize(1024, 0);
    MemoryVirtualDiskReader dr(std::move(descBytes));
    VirtualDiskSource ds(dr);
    CHECK(ds.Open() == VirtualDiskOpenResult::UnsupportedFormat);
    CHECK(ds.Format() == VirtualDiskFormat::Vmdk);
    CHECK(std::string(ds.Info().detail).find("monolithicFlat") != std::string::npos);
    CHECK(std::string(ds.Info().detail).find("companion") != std::string::npos);

    std::vector<uint8_t> vdi(0x44, 0);
    vdi[0x40] = 0x7F; vdi[0x41] = 0x10;
    vdi[0x42] = 0xDA; vdi[0x43] = 0xBE;
    CHECK(OpenWithHead(vdi, f) == VirtualDiskOpenResult::UnsupportedVariant);
    CHECK(f == VirtualDiskFormat::Vdi);
    return 0;
}

static int TestEdgeCasesAndFileReader()
{
    VirtualDiskFormat f = VirtualDiskFormat::Unknown;
    CHECK(OpenWithHead({}, f) == VirtualDiskOpenResult::EmptySource);

    MemoryVirtualDiskReader tiny(std::vector<uint8_t>(100, 0));
    VirtualDiskSource t(tiny);
    CHECK(t.Open() == VirtualDiskOpenResult::TooSmall);

    // FileVirtualDiskReader: read-only, size + random access verification.
    const std::string path = "phoenix_vds_smoke.tmp";
    const std::vector<uint8_t> data = RawImage(4);
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        CHECK(out.is_open());
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    FileVirtualDiskReader fr;
    CHECK(fr.Open(path));
    CHECK(fr.Size() == data.size());
    VirtualDiskSource fs(fr);
    CHECK(fs.Open() == VirtualDiskOpenResult::Ok);
    CHECK(fs.Format() == VirtualDiskFormat::Raw);
    uint8_t buf[512] = {};
    CHECK(fs.ReadSectors(2, 1, buf));
    CHECK(SectorHasTag(buf, 0x12));
    fr.Close();
    std::remove(path.c_str());

    FileVirtualDiskReader missing;
    CHECK(!missing.Open("phoenix_vds_no_such_file.tmp"));
    CHECK(missing.Size() == 0);
    return 0;
}

int main()
{
    if (TestRaw() != 0) return 1;
    if (TestVhdFixed() != 0) return 1;
    if (TestVhdDynamic() != 0) return 1;
    if (TestDetectedButUnsupported() != 0) return 1;
    if (TestEdgeCasesAndFileReader() != 0) return 1;
    std::cout << "VirtualDiskSource A1-1a tests: PASS\n";
    return 0;
}
