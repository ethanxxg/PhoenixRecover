// W3: real exFAT recovery-byte / MD5 gate for CFReDS dfr-01-xfat.dd.
//
// ExfatProbeDispatch (W2) only proves the *parser routing*.  This test closes
// the next gap: it compares the bytes actually written by RecoverEngine with
// an INDEPENDENT oracle that reads the raw image and walks the deleted exFAT
// root-directory entries itself (no ExfatParser / RecoverEngine involvement).
//
// Ground truth (tools/dfr_exfat_diag.py, partition @ sector 2048):
//   DEL '\\Betelgeuse.txt' size=4396 first_cluster=13 nofat=1
//   boot: bps=512 spc=8 heap=256 root=5 volumeLength=124928 sectors
//   data offset = (2048 + 256 + (13-2)*8) * 512 = 1224704
//   md5 = 6650a82d6499770bf7eef8045a014b7d (Python hashlib, independent)
//
// The test also pins the scan-source read-only invariant with a whole-image
// FNV-1a hash before and after the recovery.
//
// No <filesystem>: the GCC 7.3 MinGW build has no std::filesystem, and this
// gate must run in both build-ac (GCC 7.3) and build-gcc14 (GCC 14.2).

#include "common/Platform.h"

#if !PHOENIX_OS_WINDOWS
int main()
{
    std::printf("SKIP: ExfatRecoverBytes requires the Windows FS code path\n");
    return 0;
}
#else

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincrypt.h>

#include "common/Utils.h"
#include "disk/Partition.h"
#include "disk/SectorReader.h"
#include "fs/FileSystem.h"
#include "fs/exfat/ExfatParser.h"
#include "recover/FileNode.h"
#include "recover/RecoverEngine.h"
#include "scan/FileSystemModuleService.h"
#include "scan/ScanEngine.h"

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(expr, msg)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(expr)) {                                                          \
            ++g_fails;                                                          \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); \
        }                                                                       \
    } while (0)

namespace {

constexpr uint64_t kPartitionSector = 2048;
const char* const  kExpectedName   = "Betelgeuse.txt";
const char* const  kExpectedMd5    = "6650a82d6499770bf7eef8045a014b7d";
const char* const  kContentMarker  = "\nDFR\nFile Betelgeuse.txt";

std::wstring ImagePath()
{
    return std::wstring(PHOENIX_SOURCE_DIR) + L"/testdata/cfreds/dfr-01-xfat.dd";
}

uint16_t Le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t Le64(const uint8_t* p)
{
    return static_cast<uint64_t>(Le32(p)) | (static_cast<uint64_t>(Le32(p + 4)) << 32);
}

// ── MD5 via WinCrypt, RAII so no goto/label-crossing-initialisation issues ──
struct Md5Ctx
{
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    ~Md5Ctx()
    {
        if (hash) CryptDestroyHash(hash);
        if (prov) CryptReleaseContext(prov, 0);
    }
};

bool Md5Hex(const uint8_t* data, size_t len, char out33[33])
{
    Md5Ctx c;
    if (!CryptAcquireContextW(&c.prov, nullptr, nullptr, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT))
        return false;
    if (!CryptCreateHash(c.prov, CALG_MD5, 0, 0, &c.hash))
        return false;
    size_t off = 0;
    while (off < len) {
        DWORD chunk = static_cast<DWORD>((std::min)(size_t(1u << 30), len - off));
        if (!CryptHashData(c.hash, data + off, chunk, 0))
            return false;
        off += chunk;
    }
    BYTE md5[16]{};
    DWORD cb = sizeof(md5);
    if (!CryptGetHashParam(c.hash, HP_HASHVAL, md5, &cb, 0) || cb != 16)
        return false;
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        out33[i * 2]     = hexd[md5[i] >> 4];
        out33[i * 2 + 1] = hexd[md5[i] & 15];
    }
    out33[32] = '\0';
    return true;
}

uint64_t Fnv1a(const std::vector<uint8_t>& v, uint64_t seed = 1469598103934665603ULL)
{
    uint64_t h = seed;
    for (uint8_t b : v) { h ^= b; h *= 1099511628211ULL; }
    return h;
}

uint64_t FnvWholeImage(const std::wstring& path)
{
    std::ifstream f(Utils::WideToUtf8(path), std::ios::binary);
    if (!f) return 0;
    uint64_t h = 1469598103934665603ULL;
    std::vector<uint8_t> buf(1u << 20);
    while (f) {
        f.read(reinterpret_cast<char*>(buf.data()),
               static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();
        if (got <= 0) break;
        for (std::streamsize i = 0; i < got; ++i) {
            h ^= static_cast<uint64_t>(buf[static_cast<size_t>(i)]);
            h *= 1099511628211ULL;
        }
    }
    return h;
}

std::vector<uint8_t> ReadAt(const std::wstring& path, uint64_t off, size_t len)
{
    std::vector<uint8_t> out(len, 0);
    std::ifstream f(Utils::WideToUtf8(path), std::ios::binary);
    if (!f) return {};
    f.seekg(static_cast<std::streamoff>(off), std::ios::beg);
    if (!f) return {};
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(len));
    if (static_cast<size_t>(f.gcount()) != len) return {};
    return out;
}

std::wstring ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out)
{
    out.clear();
    std::ifstream f(Utils::WideToUtf8(path), std::ios::binary);
    if (!f) return L"cannot open";
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return L"";
}

bool AsciiEqI(const std::wstring& a, const char* b)
{
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i) {
        const wchar_t ca = static_cast<wchar_t>(std::towlower(a[i]));
        const wchar_t cb = static_cast<wchar_t>(std::towlower(
                              static_cast<wchar_t>(static_cast<unsigned char>(b[i]))));
        if (ca != cb) return false;
    }
    return i == a.size() && b[i] == '\0';
}

// ── Independent exFAT oracle: parse boot + deleted root dir entries by hand ──
struct Oracle
{
    bool        found = false;
    uint64_t    byteOffset = 0;
    uint64_t    size = 0;
    uint32_t    firstCluster = 0;
    uint64_t    bytesPerSector = 0;
    uint64_t    sectorsPerCluster = 0;
    uint64_t    clusterHeapSector = 0;
    std::vector<uint8_t> bytes;
};

Oracle ReadDeletedFileOracle(const std::wstring& path)
{
    Oracle o;
    const std::vector<uint8_t> boot = ReadAt(path, kPartitionSector * 512, 512);
    if (boot.size() != 512) return o;
    if (std::memcmp(boot.data() + 3, "EXFAT   ", 8) != 0) return o;

    o.bytesPerSector      = 1ULL << boot[108];
    o.sectorsPerCluster   = 1ULL << boot[109];
    o.clusterHeapSector   = Le32(boot.data() + 88);
    const uint32_t rootCl = Le32(boot.data() + 96);
    if (o.bytesPerSector != 512 || o.sectorsPerCluster == 0 || rootCl < 2) return o;

    const uint64_t bpc = o.bytesPerSector * o.sectorsPerCluster;
    const uint64_t rootOff =
        (kPartitionSector + o.clusterHeapSector +
         (static_cast<uint64_t>(rootCl) - 2) * o.sectorsPerCluster) * 512;
    const std::vector<uint8_t> dir = ReadAt(path, rootOff, static_cast<size_t>(bpc));
    if (dir.size() != bpc) return o;

    size_t p = 0;
    while (p + 32 <= dir.size()) {
        const uint8_t et = dir[p];
        if (et == 0x00) break;                       // end of directory
        if (et != 0x85 && et != 0x05) { p += 32; continue; }
        const size_t sc = dir[p + 1];
        if (sc < 1 || p + 32 * (1 + sc) > dir.size()) break;

        const uint8_t* se = dir.data() + p + 32;
        if (se[0] != 0xC0 && se[0] != 0x40) { p += 32 * (1 + sc); continue; }

        std::wstring name;
        for (size_t i = 2; i <= sc; ++i) {
            const uint8_t* ne = dir.data() + p + 32 * i;
            if (ne[0] != 0xC1 && ne[0] != 0x41) break;
            for (size_t j = 2; j + 1 < 32; j += 2) {
                const uint16_t ch = Le16(ne + j);
                if (ch == 0) break;
                name.push_back(static_cast<wchar_t>(ch));
            }
        }

        if (et == 0x05 && !name.empty() && AsciiEqI(name, kExpectedName)) {
            o.found        = true;
            o.firstCluster = Le32(se + 20);
            o.size         = Le64(se + 24);
            o.byteOffset   =
                (kPartitionSector + o.clusterHeapSector +
                 (static_cast<uint64_t>(o.firstCluster) - 2) *
                     o.sectorsPerCluster) * 512;
            o.bytes = ReadAt(path, o.byteOffset, static_cast<size_t>(o.size));
            return o;
        }
        p += 32 * (1 + sc);
    }
    return o;
}

std::wstring MakeTempOutDir()
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    // GetTickCount64() requires _WIN32_WINNT >= 0x0600; MinGW g++ 7.3's
    // headers do not expose it by default.  Tick + PID is unique enough.
    std::wstring dir = std::wstring(tmp) + L"PhoenixRecover_exfat_bytes_" +
                       std::to_wstring(GetTickCount()) + L"_" +
                       std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

} // namespace

int main()
{
    const std::wstring image = ImagePath();
    if (!Utils::FileExists(image)) {
        std::printf("SKIP: ExfatRecoverBytes (testdata/cfreds/dfr-01-xfat.dd missing)\n");
        return 0;
    }

    // ── 1. Read-only source invariant: whole-image hash BEFORE ─────────────
    const uint64_t imageBefore = FnvWholeImage(image);
    CHECK(imageBefore != 0, "whole-image FNV-1a hash computed before recovery");

    // ── 2. Independent oracle (raw image, hand-parsed exFAT metadata) ──────
    const Oracle oracle = ReadDeletedFileOracle(image);
    CHECK(oracle.found, "independent oracle finds deleted Betelgeuse.txt root entry");
    CHECK(oracle.bytesPerSector == 512, "oracle boot: 512 bytes per sector");
    CHECK(oracle.sectorsPerCluster == 8, "oracle boot: 8 sectors per cluster (4 KiB)");
    CHECK(oracle.clusterHeapSector == 256, "oracle boot: cluster heap at sector 256");
    CHECK(oracle.firstCluster == 13, "oracle stream ext: first cluster 13");
    CHECK(oracle.size == 4396, "oracle stream ext: data length 4396 bytes");
    CHECK(oracle.byteOffset == 1224704, "oracle data offset 1224704 (sector 2392)");
    CHECK(oracle.bytes.size() == oracle.size, "oracle reads full 4396-byte payload");

    char oracleMd5[33]{};
    CHECK(Md5Hex(oracle.bytes.data(), oracle.bytes.size(), oracleMd5),
          "oracle MD5 computed");
    CHECK(std::strcmp(oracleMd5, kExpectedMd5) == 0,
          "oracle MD5 matches independent Python hashlib value");
    CHECK(oracle.bytes.size() >= std::strlen(kContentMarker) &&
          std::memcmp(oracle.bytes.data(), kContentMarker,
                      std::strlen(kContentMarker)) == 0,
          "oracle payload starts with the DFR content marker");

    // Negative control: a neighbouring cluster must NOT match, so the byte
    // comparison below cannot pass trivially on constant/zero-filled data.
    const std::vector<uint8_t> wrongCluster =
        ReadAt(image, oracle.byteOffset - 4096, static_cast<size_t>(oracle.size));
    CHECK(wrongCluster.size() == oracle.bytes.size() &&
          std::memcmp(wrongCluster.data(), oracle.bytes.data(),
                      oracle.bytes.size()) != 0,
          "negative control: preceding cluster differs from oracle payload");

    // ── 3. Production path: auto-probe (W2) -> ExfatParser -> RecoverEngine ─
    SectorReader reader;
    CHECK(reader.OpenImage(image), "SectorReader opens the real exFAT image");
    if (!reader.IsOpen()) {
        std::printf("%s: ExfatRecoverBytes %d checks, %d failure(s)\n",
                    g_fails ? "FAIL" : "OK", g_checks, g_fails);
        return g_fails ? 1 : 0;
    }

    Partition part;
    part.startSector  = kPartitionSector;
    part.totalSectors = 124928;                 // volumeLength from boot
    part.sizeBytes    = part.totalSectors * reader.GetSectorSize();
    part.fsType       = L"";                    // force automatic probing

    ScanResult scanResult;
    auto parser = FileSystemModuleService::CreateParserForPartition(
                      reader, part, scanResult);
    CHECK(parser != nullptr, "auto-probe returns a parser for the real exFAT volume");
    CHECK(dynamic_cast<ExfatParser*>(parser.get()) != nullptr,
          "auto-probe resolves to ExfatParser with empty fsType (W2 chain)");
    if (!parser || !dynamic_cast<ExfatParser*>(parser.get())) {
        reader.Close();
        std::printf("%s: ExfatRecoverBytes %d checks, %d failure(s)\n",
                    g_fails ? "FAIL" : "OK", g_checks, g_fails);
        return g_fails ? 1 : 0;
    }

    CHECK(parser->Open(reader, part), "ExfatParser::Open succeeds on real image");
    CHECK(parser->ScanMetadata(), "ExfatParser::ScanMetadata succeeds on real image");
    const std::vector<FileNode> files = parser->GetAllFiles();
    CHECK(!files.empty(), "ExfatParser reports files from the real image");

    const FileNode* target = nullptr;
    for (const FileNode& n : files) {
        if (n.status == FileStatus::Deleted && !n.isDirectory &&
            AsciiEqI(n.name, kExpectedName))
        {
            target = &n;
            break;
        }
    }
    CHECK(target != nullptr, "production scan discovers the deleted Betelgeuse.txt");
    if (target) {
        CHECK(target->size == oracle.size, "discovered node size == oracle 4396 bytes");
        std::printf("  node: name=%ls size=%llu status=deleted clusters=%zu [",
                    target->name.c_str(),
                    static_cast<unsigned long long>(target->size),
                    target->clusters.size());
        for (uint64_t c : target->clusters)
            std::printf("%llu ", static_cast<unsigned long long>(c));
        std::printf("] rawOffset=%llu rawSize=%llu ext=%ls\n",
                    static_cast<unsigned long long>(target->rawOffset),
                    static_cast<unsigned long long>(target->rawSize),
                    target->extension.c_str());
    }

    if (!target) {
        parser.reset();
        reader.Close();
        std::printf("%s: ExfatRecoverBytes %d checks, %d failure(s)\n",
                    g_fails ? "FAIL" : "OK", g_checks, g_fails);
        return g_fails ? 1 : 0;
    }

    const std::wstring outDir = MakeTempOutDir();
    RecoverEngine engine;
    const FileRecoverResult rr =
        engine.RecoverFile(*target, outDir, false, reader, part);
    CHECK(rr.status == RecoverStatus::Success, "RecoverEngine writes the deleted file");
    CHECK(!rr.targetPath.empty(), "RecoverEngine returns a concrete target path");

    std::vector<uint8_t> recovered;
    if (rr.status == RecoverStatus::Success && !rr.targetPath.empty()) {
        const std::wstring err = ReadFileBytes(rr.targetPath, recovered);
        CHECK(err.empty(), "recovered output file is readable");
        CHECK(recovered.size() == oracle.size,
              "recovered byte count == 4396 (no truncation, no padding)");
        CHECK(recovered.size() == oracle.bytes.size() &&
              std::memcmp(recovered.data(), oracle.bytes.data(),
                          oracle.bytes.size()) == 0,
              "RECOVERY-BYTE GATE: written bytes are byte-for-byte identical to oracle");

        char recoveredMd5[33]{};
        CHECK(Md5Hex(recovered.data(), recovered.size(), recoveredMd5),
              "recovered MD5 computed");
        CHECK(std::strcmp(recoveredMd5, kExpectedMd5) == 0,
              "RECOVERY-MD5 GATE: recovered md5 == 6650a82d6499770bf7eef8045a014b7d");
        CHECK(recovered.size() >= std::strlen(kContentMarker) &&
              std::memcmp(recovered.data(), kContentMarker,
                          std::strlen(kContentMarker)) == 0,
              "recovered payload retains the DFR content marker");
        std::printf("  recovered: %llu bytes md5=%s\n",
                    static_cast<unsigned long long>(recovered.size()), recoveredMd5);
        DeleteFileW(rr.targetPath.c_str());
    }
    RemoveDirectoryW(outDir.c_str());

    parser.reset();
    reader.Close();

    // ── 4. Read-only source invariant: whole-image hash AFTER ──────────────
    const uint64_t imageAfter = FnvWholeImage(image);
    CHECK(imageAfter == imageBefore && imageAfter != 0,
          "whole-image FNV-1a unchanged after scan + recovery (source is read-only)");
    std::printf("  image fnv1a before/after = %016llx / %016llx\n",
                static_cast<unsigned long long>(imageBefore),
                static_cast<unsigned long long>(imageAfter));

    std::printf("%s: ExfatRecoverBytes %d checks, %d failure(s)\n",
                g_fails ? "FAIL" : "OK", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

#endif
