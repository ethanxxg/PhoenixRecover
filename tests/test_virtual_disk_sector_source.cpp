// A1-2: end-to-end test of the virtual-disk scan-source wiring.
//
// Unlike test_virtual_disk_golden.cpp (which exercises VirtualDiskSource
// directly), this test drives the REAL scan path: SectorReader::
// OpenVirtualImage() -> ReadSectors()/ReadBytes() -> BadSectorMap/progress/
// cancel, including the real-image golden corpus. All fixtures are ordinary
// temp files; no device is opened and no source disk is ever written.

#include "disk/SectorReader.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
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

namespace {

const wchar_t* kVhdTmp  = L"phoenix_a12_dynamic.vhd";
const wchar_t* kRawTmp  = L"phoenix_a12_raw.tmp";
const wchar_t* kDiffTmp = L"phoenix_a12_diff.vhd";

std::string Narrow(const std::wstring& w)
{
    return std::string(w.begin(), w.end());   // test fixtures are ASCII
}

bool WriteBytes(const std::wstring& path, const std::vector<uint8_t>& data)
{
    std::ofstream out(Narrow(path), std::ios::binary | std::ios::trunc);
    if (!out.good()) return false;
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    return out.good();
}

void RemoveTemp(const std::wstring& path)
{
    std::remove(Narrow(path).c_str());
}

void PutSector(std::vector<uint8_t>& v, size_t offset, uint8_t tag)
{
    if (v.size() < offset + 512) v.resize(offset + 512, 0);
    for (size_t i = 0; i < 512; ++i) {
        v[offset + i] = static_cast<uint8_t>(tag + (i & 0x1F));
    }
}

bool SectorHasTag(const uint8_t* p, uint8_t tag)
{
    for (size_t i = 0; i < 512; ++i) {
        if (p[i] != static_cast<uint8_t>(tag + (i & 0x1F))) return false;
    }
    return true;
}

bool SectorIsZero(const uint8_t* p)
{
    for (size_t i = 0; i < 512; ++i) {
        if (p[i] != 0) return false;
    }
    return true;
}

void AppendFooter(std::vector<uint8_t>& v, uint64_t currentSize,
                  uint32_t diskType, uint64_t dataOffset)
{
    const size_t off = v.size();
    v.resize(off + 512, 0);
    uint8_t* f = v.data() + off;
    std::memcpy(f, "conectix", 8);
    PutBe32(f + 0x0C, 0x00010000);
    PutBe64(f + 0x10, dataOffset);
    PutBe64(f + 0x24, currentSize);
    PutBe64(f + 0x30, currentSize);
    PutBe32(f + 0x3C, diskType);
}

// 2 MiB block VHD dynamic, 8192 virtual bytes (16 sectors).
//   block 0 -> file sector 4, bitmap bits 0 and 2 allocated
//   block 1 -> 0xFFFFFFFF (sparse)
std::vector<uint8_t> DynamicVhd(uint32_t diskType = 3)
{
    std::vector<uint8_t> v(1536, 0);
    std::vector<uint8_t> footerCopy;
    AppendFooter(footerCopy, 8192, diskType, 512);
    std::memcpy(v.data(), footerCopy.data(), 512);

    uint8_t* dyn = v.data() + 512;
    std::memcpy(dyn, "cxsparse", 8);
    PutBe64(dyn + 0x10, 1536ULL);      // BAT offset
    PutBe32(dyn + 0x18, 0x00010000);  // header version
    PutBe32(dyn + 0x1C, 2);           // max table entries
    PutBe32(dyn + 0x20, 4096);        // block size = 8 sectors

    v.resize(2048, 0);
    PutBe32(v.data() + 1536, 4);
    PutBe32(v.data() + 1540, 0xFFFFFFFFu);

    v.resize(2560, 0);
    v[2048] = 0xA0;
    for (int i = 0; i < 8; ++i) {
        PutSector(v, 2560 + static_cast<size_t>(i) * 512,
                  static_cast<uint8_t>(0x30 + i));
    }
    AppendFooter(v, 8192, diskType, 512);
    return v;
}

std::string Fnv1aHex(const uint8_t* p, size_t n)
{
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ULL;
    }
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(h));
    return std::string(buf);
}

// ---------------------------------------------------------------------------

int TestPathRouting()
{
    CHECK(IsVirtualDiskImagePath(L"disk.vhd"));
    CHECK(IsVirtualDiskImagePath(L"disk.VHDX"));
    CHECK(IsVirtualDiskImagePath(L"C:\\images\\disk.Vmdk"));
    CHECK(IsVirtualDiskImagePath(L"disk.qcow2"));
    CHECK(IsVirtualDiskImagePath(L"disk.vdi"));
    CHECK(!IsVirtualDiskImagePath(L"disk.img"));
    CHECK(!IsVirtualDiskImagePath(L"disk.dd"));
    CHECK(!IsVirtualDiskImagePath(L"disk.raw"));
    CHECK(!IsVirtualDiskImagePath(L"disk"));
    CHECK(!IsVirtualDiskImagePath(L"disk.vhd.bak"));
    return 0;
}

int TestSyntheticEndToEnd()
{
    const std::vector<uint8_t> original = DynamicVhd();
    CHECK(WriteBytes(kVhdTmp, original));

    SectorReader r;
    std::string detail;
    CHECK(r.OpenVirtualImage(kVhdTmp, &detail));

    CHECK(r.IsOpen());
    CHECK(r.IsVirtualImage());
    CHECK(r.GetSectorSize() == 512);
    CHECK(r.GetTotalSectors() == 16);
    CHECK(r.GetTotalBytes() == 8192);
    CHECK(r.GetVirtualDiskInfo().format == VirtualDiskFormat::VhdDynamic);
    CHECK(r.GetVirtualDiskInfo().virtualBytes == 8192);
    CHECK(r.GetVirtualDiskInfo().blockSize == 4096);

    uint8_t buf[4 * 512] = {};
    CHECK(r.ReadSectors(0, buf, 4));
    CHECK(SectorHasTag(buf, 0x30));            // allocated sector 0
    CHECK(SectorIsZero(buf + 512));             // sector 1 unallocated in block
    CHECK(SectorHasTag(buf + 1024, 0x32));     // allocated sector 2
    CHECK(SectorIsZero(buf + 1536));            // sector 3 unallocated in block

    uint8_t sparse[512] = {1};
    CHECK(r.ReadSectors(8, sparse, 1));
    CHECK(SectorIsZero(sparse));                // whole sparse block

    // ReadBytes must go through the same virtual path (aligned + unaligned).
    uint8_t small[8] = {};
    CHECK(r.ReadBytes(10, small, sizeof(small)));
    for (size_t i = 0; i < sizeof(small); ++i) {
        CHECK(small[i] == static_cast<uint8_t>(0x30 + ((10 + i) & 0x1F)));
    }
    CHECK(r.ReadBytes(512 + 4, small, sizeof(small)));
    CHECK(small[0] == 0 && small[7] == 0);

    // Progress callback fires on the virtual read path.
    int progressCalls = 0;
    r.SetSectorReadCallback([&progressCalls](uint64_t, uint64_t) {
        ++progressCalls;
    });
    CHECK(r.ReadSectors(2, buf, 1));
    CHECK(progressCalls > 0);

    // Out-of-range: lenient mode zero-fills and records a bad sector.
    CHECK(r.GetBadSectorCount() == 0);
    CHECK(r.ReadSectors(16, buf, 1));
    CHECK(r.GetBadSectorCount() == 1);
    CHECK(SectorIsZero(buf));

    // Strict mode must fail instead of silently zero-filling.
    ReadPolicy strict;
    strict.maxRetries = 0;
    strict.skipBadSectors = false;
    r.SetReadPolicy(strict);
    CHECK(!r.ReadSectors(16, buf, 1));

    // Cancel flag is honoured before any virtual read.
    std::atomic<bool> cancel{false};
    r.SetCancelFlag(&cancel);
    cancel.store(true);
    CHECK(!r.ReadSectors(0, buf, 1));
    cancel.store(false);
    CHECK(r.ReadSectors(0, buf, 1));

    r.Close();
    CHECK(!r.IsOpen());
    CHECK(!r.IsVirtualImage());
    CHECK(r.GetTotalSectors() == 0);
    CHECK(r.GetVirtualDiskInfo().format == VirtualDiskFormat::Unknown);

    // S1 evidence: the whole open/read/close cycle must not alter a single
    // byte of the image file.
    {
        std::ifstream in(Narrow(kVhdTmp), std::ios::binary);
        CHECK(in.good());
        std::vector<uint8_t> after((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        CHECK(after == original);
    }

    RemoveTemp(kVhdTmp);
    return 0;
}

int TestRawAndVariantRejection()
{
    // A .vhd extension with plain raw content must NOT be silently scanned
    // as a raw disk -- the caller is told to use OpenImage() instead.
    std::vector<uint8_t> raw(1124, 0x5A);
    CHECK(WriteBytes(kVhdTmp, raw));
    {
        SectorReader r;
        std::string detail;
        CHECK(!r.OpenVirtualImage(kVhdTmp, &detail));
        CHECK(detail.find("raw image") != std::string::npos);
        CHECK(!r.IsOpen());
    }

    // The untouched OpenImage() path keeps its exact-byte-length semantics.
    CHECK(WriteBytes(kRawTmp, raw));
    {
        SectorReader r;
        CHECK(r.OpenImage(kRawTmp));
        CHECK(!r.IsVirtualImage());
        CHECK(r.GetTotalBytes() == 1124);
        CHECK(r.GetTotalSectors() == 2);
        uint8_t b[512] = {};
        CHECK(r.ReadSectors(0, b, 1));
        CHECK(b[0] == 0x5A && b[511] == 0x5A);
        r.Close();
    }

    // A recognised-but-unsupported container (VHD differencing) must surface
    // the parser's reason all the way up to SectorReader.
    CHECK(WriteBytes(kDiffTmp, DynamicVhd(4)));
    {
        SectorReader r;
        std::string detail;
        CHECK(!r.OpenVirtualImage(kDiffTmp, &detail));
        CHECK(!detail.empty());
        CHECK(!r.IsOpen());
        std::cout << "  differencing rejection: " << detail << "\n";
    }

    RemoveTemp(kVhdTmp);
    RemoveTemp(kRawTmp);
    RemoveTemp(kDiffTmp);
    return 0;
}

// ---------------------------------------------------------------------------
// Real-image golden corpus through the SectorReader path
// ---------------------------------------------------------------------------

struct GoldenRow
{
    std::string name;
    int         code      = 0;
    uint64_t    vbytes    = 0;
    std::string fingerprints;
};

std::string ResolveGoldenDir()
{
    std::vector<std::string> candidates;
    if (const char* env = std::getenv("PHOENIX_GOLDEN_DIR")) {
        if (*env) candidates.emplace_back(env);
    }
    candidates.emplace_back("testdata/virtual-disk-golden/");
    candidates.emplace_back("../testdata/virtual-disk-golden/");
    candidates.emplace_back("../../testdata/virtual-disk-golden/");
    candidates.emplace_back("../../../testdata/virtual-disk-golden/");
    candidates.emplace_back("../../../../testdata/virtual-disk-golden/");
    for (std::string d : candidates) {
        if (d.empty()) continue;
        if (d.back() != '/' && d.back() != '\\') d += '/';
        if (std::ifstream(d + "expectations.tsv").good()) return d;
    }
    return {};
}

bool LoadRows(const std::string& path, std::vector<GoldenRow>& rows)
{
    std::ifstream in(path);
    if (!in.good()) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, '\t')) cols.push_back(cell);
        if (cols.size() < 7) continue;
        GoldenRow r;
        r.name = cols[0];
        r.code = std::atoi(cols[1].c_str());
        r.vbytes = std::strtoull(cols[2].c_str(), nullptr, 10);
        r.fingerprints = cols[6];
        if (!r.name.empty()) rows.push_back(r);
    }
    return !rows.empty();
}

int TestRealGoldenThroughSectorReader()
{
    const std::string dir = ResolveGoldenDir();
    if (dir.empty()) {
        std::cout << "SKIPPED real-image corpus (run "
                     "testdata/tools/Fetch-VirtualDiskGolden.py)\n";
        return 0;
    }
    std::vector<GoldenRow> rows;
    CHECK(LoadRows(dir + "expectations.tsv", rows));

    int images = 0;
    int sectors = 0;
    int oor = 0;
    for (const GoldenRow& row : rows) {
        // 3 = VHD dynamic, 4 = VHDX. Other rows are RAW / rejected VMDK
        // variants, which OpenVirtualImage intentionally refuses.
        if (row.code != 3 && row.code != 4) continue;
        ++images;

        const std::string narrow = dir + row.name;
        const std::wstring path(narrow.begin(), narrow.end());
        SectorReader r;
        std::string detail;
        if (!r.OpenVirtualImage(path, &detail)) {
            std::cerr << "FAIL cannot open golden image " << row.name
                      << ": " << detail << "\n";
            return 1;
        }
        CHECK(r.IsVirtualImage());
        CHECK(r.GetTotalSectors() == row.vbytes / 512);

        ReadPolicy strict;
        strict.maxRetries = 0;
        strict.skipBadSectors = false;
        r.SetReadPolicy(strict);

        std::stringstream ss(row.fingerprints);
        std::string item;
        while (std::getline(ss, item, ',')) {
            const size_t colon = item.find(':');
            CHECK(colon != std::string::npos);
            const uint64_t lba = std::strtoull(item.substr(0, colon).c_str(),
                                               nullptr, 10);
            const std::string expect = item.substr(colon + 1);
            uint8_t buf[512] = {};
            if (expect == "OOR") {
                CHECK(!r.ReadSectors(lba, buf, 1));
                ++oor;
            } else {
                CHECK(r.ReadSectors(lba, buf, 1));
                CHECK(Fnv1aHex(buf, 512) == expect);
                ++sectors;
            }
        }
        r.Close();
    }

    CHECK(images >= 6);   // 3 VHD dynamic + 3 VHDX in the corpus
    std::cout << "  SectorReader golden: " << images << " real images, "
              << sectors << " sector fingerprints, " << oor
              << " out-of-range checks\n";
    return 0;
}

} // namespace

int main()
{
    struct Case { const char* name; int (*fn)(); };
    const Case cases[] = {
        {"PathRouting",            TestPathRouting},
        {"SyntheticEndToEnd",      TestSyntheticEndToEnd},
        {"RawAndVariantRejection", TestRawAndVariantRejection},
        {"RealGoldenSectorReader", TestRealGoldenThroughSectorReader},
    };
    for (const Case& c : cases) {
        std::cout << "[A1-2] " << c.name << "\n";
        const int rc = c.fn();
        if (rc != 0) {
            RemoveTemp(kVhdTmp);
            RemoveTemp(kRawTmp);
            RemoveTemp(kDiffTmp);
            std::cerr << "FAILED: " << c.name << "\n";
            return rc;
        }
    }
    RemoveTemp(kVhdTmp);
    RemoveTemp(kRawTmp);
    RemoveTemp(kDiffTmp);
    std::cout << "A1-2 SectorReader virtual-image wiring: ALL PASS\n";
    return 0;
}
