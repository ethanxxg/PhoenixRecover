// W2: exFAT must be reachable through the automatic file-system probe chain.
//
// Before this fix CreateParserForPartition() probed NTFS -> FAT -> ext4 only,
// so an exFAT volume whose partition.fsType was empty (lost partition, raw
// image, sector-based scan) could never reach the already-implemented
// ExfatParser.  This test pins both the positive routing and the absence of
// regressions in the NTFS/FAT/ext4 arms.
//
// Also covers W3's real exFAT golden: testdata/cfreds/dfr-01-xfat.dd has its
// boot sector at sector 2048 (first 1 MiB is the MBR/GPT gap).

#include "common/Platform.h"
#include "common/Utils.h"
#include "disk/Partition.h"
#include "disk/SectorReader.h"
#include "fs/FileSystem.h"
#include "fs/exfat/ExfatParser.h"
#include "fs/ext4/Ext4Parser.h"
#include "fs/fat/FatParser.h"
#include "fs/ntfs/NtfsParser.h"
#include "scan/FileSystemModuleService.h"
#include "scan/ScanEngine.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if !PHOENIX_OS_WINDOWS
int main()
{
    std::printf("SKIP: ExfatProbeDispatch requires the Windows FS code path\n");
    return 0;
}
#else

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(expr, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(expr)) {                                                         \
            ++g_fails;                                                         \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); \
        }                                                                      \
    } while (0)

namespace {

enum class Kind { None, Ntfs, Exfat, Fat, Ext4, Other };

Kind Classify(FileSystem* p)
{
    if (!p) return Kind::None;
    if (dynamic_cast<ExfatParser*>(p)) return Kind::Exfat;
    if (dynamic_cast<NtfsParser*>(p)) return Kind::Ntfs;
    if (dynamic_cast<FatParser*>(p)) return Kind::Fat;
    if (dynamic_cast<Ext4Parser*>(p)) return Kind::Ext4;
    return Kind::Other;
}

const char* Name(Kind k)
{
    switch (k) {
    case Kind::None:  return "none";
    case Kind::Ntfs:  return "NTFS";
    case Kind::Exfat: return "exFAT";
    case Kind::Fat:   return "FAT";
    case Kind::Ext4:  return "ext4";
    default:          return "other";
    }
}

std::wstring TempImagePath(const wchar_t* tag)
{
    std::wstring dir = Utils::GetTempDirectoryW();
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.empty()) dir = L".";
    return dir + L"/PhoenixRecover_exfat_probe_" + tag + L".dd";
}

bool WriteImage(const std::wstring& path, const std::vector<uint8_t>& img)
{
    std::ofstream f(Utils::WideToUtf8(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(img.data()),
            static_cast<std::streamsize>(img.size()));
    return static_cast<bool>(f);
}

void RemoveImage(const std::wstring& path)
{
    std::remove(Utils::WideToUtf8(path).c_str());
}

std::vector<uint8_t> MakeExfatImage()
{
    std::vector<uint8_t> img(64 * 512, 0);
    uint8_t* b = img.data();
    b[0] = 0xEB; b[1] = 0x76; b[2] = 0x90;
    std::memcpy(b + 3, "EXFAT   ", 8);
    // BPB fields (EXFAT_BOOT_SECTOR layout, packed):
    //  @64 partitionOffset(8) @72 volumeLength(8) @80 fatOffset(4)
    //  @84 fatLength(4) @88 clusterHeapOffset(4) @92 clusterCount(4)
    b[72] = 64;                 // volumeLength = 64 sectors
    b[80] = 8;                  // fatOffset
    b[84] = 1;                  // fatLength
    b[88] = 16;                 // clusterHeapOffset
    b[92] = 8;                  // clusterCount
    b[108] = 9;                 // bytesPerSectorShift -> 512
    b[109] = 0;                 // sectorsPerClusterShift -> 1
    b[110] = 1;                 // numberOfFATs
    b[510] = 0x55; b[511] = 0xAA;
    return img;
}

std::vector<uint8_t> MakeNtfsImage()
{
    std::vector<uint8_t> img(64 * 512, 0);
    uint8_t* b = img.data();
    b[0] = 0xEB; b[1] = 0x52; b[2] = 0x90;
    std::memcpy(b + 3, "NTFS    ", 8);
    b[11] = 0x00; b[12] = 0x02; // bytesPerSector = 512
    b[13] = 1;                  // sectorsPerCluster
    b[21] = 0xF8;               // media descriptor
    b[40] = 64;                 // totalSectors (LE64)
    b[64] = static_cast<uint8_t>(-10); // clustersPerMftRecord -> 1024 B
    b[68] = static_cast<uint8_t>(-9);  // clustersPerIndexBuffer
    b[510] = 0x55; b[511] = 0xAA;
    return img;
}

std::vector<uint8_t> MakeFat16Image()
{
    std::vector<uint8_t> img(64 * 512, 0);
    uint8_t* b = img.data();
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    std::memcpy(b + 3, "MSWIN4.1", 8);
    b[11] = 0x00; b[12] = 0x02; // bytesPerSector = 512
    b[13] = 1;                  // sectorsPerCluster
    b[14] = 1;                  // reservedSectors
    b[16] = 1;                  // numFATs
    b[17] = 16;                 // rootEntryCount
    b[19] = 64;                 // totalSectors16
    b[21] = 0xF8;
    b[22] = 1;                  // fatSize16
    b[36] = 0x29;
    std::memcpy(b + 54, "FAT16   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
    return img;
}

Kind ProbeImage(const std::wstring& path, uint64_t startSector,
                const std::wstring& declaredFs)
{
    SectorReader reader;
    if (!reader.OpenImage(path)) return Kind::None;
    Partition part;
    part.startSector  = startSector;
    part.totalSectors = 64;
    part.sizeBytes    = 64ULL * 512;
    part.fsType       = declaredFs;
    ScanResult result;
    auto parser = FileSystemModuleService::CreateParserForPartition(reader, part, result);
    Kind k = Classify(parser.get());
    parser.reset();
    reader.Close();
    return k;
}

std::string ReadSourceFile(const std::wstring& path)
{
    std::ifstream f(Utils::WideToUtf8(path), std::ios::binary);
    if (!f) return std::string();
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

std::wstring SourceDir()
{
    return std::wstring(PHOENIX_SOURCE_DIR);
}

} // namespace

int main()
{
    // ── 1. Explicit fsType routing (unchanged behaviour) ──────────────────
    CHECK(Classify(FileSystemModuleService::CreateParser(L"exFAT").get()) == Kind::Exfat,
          "CreateParser(exFAT) returns ExfatParser");
    CHECK(Classify(FileSystemModuleService::CreateParser(L"NTFS").get()) == Kind::Ntfs,
          "CreateParser(NTFS) returns NtfsParser");
    CHECK(Classify(FileSystemModuleService::CreateParser(L"FAT32").get()) == Kind::Fat,
          "CreateParser(FAT32) returns FatParser");
    CHECK(Classify(FileSystemModuleService::CreateParser(L"ext4").get()) == Kind::Ext4,
          "CreateParser(ext4) returns Ext4Parser");

    // ── 2. Synthetic boots: positive exFAT + no regression on other arms ──
    const std::wstring exPath = TempImagePath(L"exfat");
    const std::wstring ntPath = TempImagePath(L"ntfs");
    const std::wstring ftPath = TempImagePath(L"fat16");
    const std::wstring zrPath = TempImagePath(L"zeros");

    CHECK(WriteImage(exPath, MakeExfatImage()), "write synthetic exFAT image");
    CHECK(WriteImage(ntPath, MakeNtfsImage()),  "write synthetic NTFS image");
    CHECK(WriteImage(ftPath, MakeFat16Image()), "write synthetic FAT16 image");
    CHECK(WriteImage(zrPath, std::vector<uint8_t>(64 * 512, 0)), "write zero image");

    const Kind exEmpty  = ProbeImage(exPath, 0, L"");
    const Kind exUnkn   = ProbeImage(exPath, 0, L"Unknown");
    const Kind ntEmpty  = ProbeImage(ntPath, 0, L"");
    const Kind ftEmpty  = ProbeImage(ftPath, 0, L"");
    const Kind zero     = ProbeImage(zrPath, 0, L"");

    CHECK(exEmpty == Kind::Exfat,
          "unknown fsType on exFAT boot routes to ExfatParser (the W2 fix)");
    CHECK(exUnkn == Kind::Exfat,
          "fsType=Unknown on exFAT boot also routes to ExfatParser");
    CHECK(ntEmpty == Kind::Ntfs,
          "NTFS boot still routes to NtfsParser (no exFAT false positive)");
    CHECK(ftEmpty == Kind::Fat,
          "FAT16 boot still routes to FatParser (no exFAT false positive)");
    CHECK(zero == Kind::None, "zero-filled image matches no parser");

    std::printf("  synthetic probe: exFAT=%s/%s NTFS=%s FAT16=%s zeros=%s\n",
                Name(exEmpty), Name(exUnkn), Name(ntEmpty), Name(ftEmpty), Name(zero));

    RemoveImage(exPath);
    RemoveImage(ntPath);
    RemoveImage(ftPath);
    RemoveImage(zrPath);

    // ── 3. W3 real golden: CFReDS dfr-01-xfat.dd, boot at sector 2048 ────
    const std::wstring realPath =
        SourceDir() + L"/testdata/cfreds/dfr-01-xfat.dd";
    if (Utils::FileExists(realPath)) {
        const Kind realAuto = ProbeImage(realPath, 2048, L"");
        const Kind realDecl = ProbeImage(realPath, 2048, L"exFAT");
        CHECK(realAuto == Kind::Exfat,
              "real CFReDS exFAT image (sector 2048) auto-probes as ExfatParser");
        CHECK(realDecl == Kind::Exfat,
              "real CFReDS exFAT image with declared fsType stays ExfatParser");
        const Kind realWrongSector = ProbeImage(realPath, 0, L"");
        CHECK(realWrongSector != Kind::Exfat,
              "real exFAT image probed at sector 0 is not exFAT (startSector matters)");
        std::printf("  real golden: auto=%s declared=%s sector0=%s\n",
                    Name(realAuto), Name(realDecl), Name(realWrongSector));
    } else {
        std::printf("  SKIP real golden (testdata/cfreds/dfr-01-xfat.dd missing)\n");
    }

    // ── 4. Source-level wiring guards ───────────────────────────────────
    const std::string svc = ReadSourceFile(
        SourceDir() + L"/scan/FileSystemModuleService.cpp");
    const std::string hdr = ReadSourceFile(
        SourceDir() + L"/scan/FileSystemModuleService.h");
    CHECK(!svc.empty() && !hdr.empty(), "read factory sources for wiring guards");

    const size_t fnPos = svc.find("CreateParserForPartition");
    CHECK(fnPos != std::string::npos, "factory source contains CreateParserForPartition");
    const size_t exProbe = svc.find("std::make_unique<ExfatParser>()", fnPos);
    const size_t ftProbe = svc.find("std::make_unique<FatParser>()", fnPos);
    const size_t endPos  = svc.find("#endif", fnPos);
    CHECK(exProbe != std::string::npos,
          "Windows probe chain constructs ExfatParser");
    CHECK(ftProbe != std::string::npos,
          "Windows probe chain still constructs FatParser");
    CHECK(exProbe != std::string::npos && ftProbe != std::string::npos &&
          exProbe < ftProbe,
          "exFAT is probed before FAT (deterministic routing)");
    CHECK(exProbe != std::string::npos && endPos != std::string::npos &&
          exProbe < endPos,
          "exFAT probe sits inside the Windows (non ext-only) branch");
    CHECK(hdr.find("exFAT") != std::string::npos,
          "factory header documents the exFAT probe step");

    std::printf("%s: ExfatProbeDispatch %d checks, %d failure(s)\n",
                g_fails ? "FAIL" : "OK", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

#endif
