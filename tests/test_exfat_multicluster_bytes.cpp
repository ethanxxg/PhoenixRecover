// W7: multi-cluster / FAT-misleading exFAT recovery-byte gate.
//
// ExfatRecoverBytes (W3) covers ONE contiguous 2-cluster deleted file in
// dfr-01-xfat.dd.  CAR-003 was fixed there, but the fix must also hold when
// the stream spans more clusters AND when the FAT table actively disagrees
// with the directory entry's NoFatChain flag.
//
// Ground truth comes from a standalone Python oracle that reads each boot
// sector, walks the deleted root/subdirectory entries, and then reads the
// payload STRICTLY by the exFAT NoFatChain rule (consecutive clusters from
// firstCluster, ignoring FAT entirely).  MD5 values below are Python
// hashlib results; the C++ oracle re-reads the raw image independently.
//
//   image                     file              size   first  nclusters  FAT[first]
//   dfr-02-xfat.dd            Betelgeuse.txt    8192   13     2          15
//   dfr-03-xfat.dd            Betelgeuse.txt   16384   13     4          15
//   dfr-05-xfat.dd            Betelgeuse.txt    8192   13     2          15
//   dfr-05-xfat.dd            ElNath.TXT        8192   17     2          19
//   dfr-12-xfat.dd            Alcor.txt         8192   19     2          26  <-- misleading
//   dfr-12-xfat.dd            Mizar.txt         8192   21     2          31  <-- misleading
//   dfr-12-xfat.dd            Dubhe.txt         8192   23     2          33  <-- misleading
//
// Following FAT here would jump 19 -> 26 instead of 19 -> 20 and produce
// wrong bytes.  This gate pins the contiguous sector chain, the recovered
// bytes/MD5, and the read-only source invariant.
//
// NOTE: every deleted exFAT file in the repo's real DFR images has
// NoFatChain=1.  A genuinely FAT-chained (NoFatChain=0) deleted stream still
// needs a synthetic golden; see expected_results.csv (W7 scope note).

#include "common/Platform.h"

#include <cstdio>

#if !PHOENIX_OS_WINDOWS
int main()
{
    std::printf("SKIP: ExfatMulticlusterBytes requires the Windows FS code path\n");
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

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(expr, msg)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(expr)) {                                                          \
            ++g_fails;                                                          \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); \
        }                                                                       \
    } while (0)

namespace {

constexpr uint64_t kPartitionSector  = 2048;
constexpr uint32_t kSectorSize       = 512;
constexpr uint32_t kSectorsPerClus   = 8;      // 4 KiB clusters
constexpr uint64_t kVolumeSectors    = 124928; // boot.volumeLength for all four images

struct Case
{
    const char* name;
    uint64_t    size;
    uint32_t    firstCluster;
    uint32_t    clusterCount;
    uint32_t    firstSector;    // absolute disk sector of firstCluster
    const char* md5;            // Python hashlib, independent oracle
};

struct ImageGroup
{
    const char* rel;
    const Case* cases;
    size_t      count;
};

const Case kDfr02[] = {
    {"Betelgeuse.txt",  8192, 13, 2, 2392, "88b0162e11cda04570d6c218795636d9"},
};
const Case kDfr03[] = {
    {"Betelgeuse.txt", 16384, 13, 4, 2392, "6e426486ef20e9e47e93d2e122a0bdb8"},
};
const Case kDfr05[] = {
    {"Betelgeuse.txt",  8192, 13, 2, 2392, "88b0162e11cda04570d6c218795636d9"},
    {"ElNath.TXT",      8192, 17, 2, 2424, "8e8f38051a9bac72071b146caa3dccd9"},
};
const Case kDfr12[] = {
    {"Alcor.txt",       8192, 19, 2, 2440, "bfe2b4f81fb163f2d2113b9dc3658fc7"},
    {"Mizar.txt",       8192, 21, 2, 2456, "617b4d586663d6d6117be010418d6ba8"},
    {"Dubhe.txt",       8192, 23, 2, 2472, "a25f01d46e1d73ce78e14242e3e0fc41"},
};

const ImageGroup kGroups[] = {
    {"dfr-test-images/dfr-02/dfr-02-xfat.dd", kDfr02, sizeof(kDfr02) / sizeof(kDfr02[0])},
    {"dfr-test-images/dfr-03/dfr-03-xfat.dd", kDfr03, sizeof(kDfr03) / sizeof(kDfr03[0])},
    {"dfr-test-images/dfr-05/dfr-05-xfat.dd", kDfr05, sizeof(kDfr05) / sizeof(kDfr05[0])},
    {"dfr-test-images/dfr-12/dfr-12-xfat.dd", kDfr12, sizeof(kDfr12) / sizeof(kDfr12[0])},
};

std::wstring ImagePath(const char* rel)
{
    return std::wstring(PHOENIX_SOURCE_DIR) + L"/testdata/" +
           Utils::Utf8ToWide(rel);
}

// ── MD5 via WinCrypt, RAII (no goto / no label-crossing-initialisation) ────
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
                              CRYPT_VERIFYCONTEXT)) return false;
    if (!CryptCreateHash(c.prov, CALG_MD5, 0, 0, &c.hash)) return false;
    size_t off = 0;
    while (off < len) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(size_t(1u << 30), len - off));
        if (!CryptHashData(c.hash, data + off, chunk, 0)) return false;
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
    f.read(reinterpret_cast<char*>(out.data()),
           static_cast<std::streamsize>(len));
    if (static_cast<size_t>(f.gcount()) != len) return {};
    return out;
}

std::vector<uint8_t> ReadFileBytes(const std::wstring& path)
{
    std::ifstream f(Utils::WideToUtf8(path), std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
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

std::wstring MakeTempOutDir()
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    // GetTickCount64() needs _WIN32_WINNT >= 0x0600, absent in MinGW g++ 7.3.
    std::wstring dir = std::wstring(tmp) + L"PhoenixRecover_exfat_multi_" +
                       std::to_wstring(GetTickCount()) + L"_" +
                       std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

const FileNode* FindDeleted(const std::vector<FileNode>& files, const char* name)
{
    for (const FileNode& n : files)
        if (n.status == FileStatus::Deleted && !n.isDirectory &&
            AsciiEqI(n.name, name)) return &n;
    return nullptr;
}

void CheckGroup(const ImageGroup& group)
{
    const std::wstring image = ImagePath(group.rel);
    std::printf("\n=== %s ===\n  image: %ls\n", group.rel, image.c_str());
    if (!Utils::FileExists(image)) {
        std::printf("  SKIP: image missing\n");
        return;
    }

    const uint64_t hashBefore = FnvWholeImage(image);
    CHECK(hashBefore != 0, "whole-image FNV-1a computed before recovery");

    SectorReader reader;
    CHECK(reader.OpenImage(image), "SectorReader opens the real exFAT image");
    if (!reader.IsOpen()) return;

    Partition part;
    part.startSector  = kPartitionSector;
    part.totalSectors = kVolumeSectors;
    part.sizeBytes    = kVolumeSectors * reader.GetSectorSize();
    part.fsType       = L"";  // force automatic probing (W2 chain)

    ScanResult scanResult;
    auto parser = FileSystemModuleService::CreateParserForPartition(
                      reader, part, scanResult);
    CHECK(parser != nullptr, "auto-probe returns a parser");
    CHECK(dynamic_cast<ExfatParser*>(parser.get()) != nullptr,
          "auto-probe resolves to ExfatParser");
    if (!parser || !dynamic_cast<ExfatParser*>(parser.get())) {
        reader.Close();
        return;
    }
    CHECK(parser->Open(reader, part), "ExfatParser::Open succeeds");
    CHECK(parser->ScanMetadata(nullptr, nullptr),
          "ExfatParser::ScanMetadata succeeds");
    const std::vector<FileNode> files = parser->GetAllFiles();
    CHECK(!files.empty(), "parser reports nodes");
    std::printf("  nodes=%zu\n", files.size());

    const std::wstring outDir = MakeTempOutDir();

    for (size_t i = 0; i < group.count; ++i) {
        const Case& c = group.cases[i];
        const uint64_t dataOffset =
            static_cast<uint64_t>(c.firstSector) * kSectorSize;

        // ── Independent oracle: raw image, NoFatChain = consecutive clusters ──
        const std::vector<uint8_t> oracleBytes =
            ReadAt(image, dataOffset, static_cast<size_t>(c.size));
        CHECK(oracleBytes.size() == c.size, "oracle reads full payload");
        char oracleMd5[33]{};
        Md5Hex(oracleBytes.data(), oracleBytes.size(), oracleMd5);
        CHECK(std::strcmp(oracleMd5, c.md5) == 0,
              "oracle MD5 matches independent Python hashlib value");

        // ── Production path ──────────────────────────────────────────────
        const FileNode* node = FindDeleted(files, c.name);
        CHECK(node != nullptr, "production scan discovers the deleted file");
        if (!node) {
            std::fprintf(stderr, "       missing: %s\n", c.name);
            continue;
        }
        CHECK(node->size == c.size, "discovered size matches oracle");
        CHECK(node->rawOffset == dataOffset,
              "rawOffset points at the first cluster (NoFatChain fast path)");
        CHECK(node->clusters.size() == c.clusterCount,
              "cluster chain length == ceil(size / clusterBytes)");

        bool chainOk = node->clusters.size() == c.clusterCount;
        for (uint32_t k = 0; chainOk && k < c.clusterCount; ++k) {
            const uint64_t want =
                static_cast<uint64_t>(c.firstSector) + k * kSectorsPerClus;
            if (node->clusters[k] != want) chainOk = false;
        }
        CHECK(chainOk,
              "CONTIGUOUS-CHAIN GATE: FAT table must be ignored for NoFatChain");

        std::printf("  %-18s size=%-7llu clusters=%zu [", c.name,
                    static_cast<unsigned long long>(node->size),
                    node->clusters.size());
        for (uint64_t s : node->clusters)
            std::printf("%llu ", static_cast<unsigned long long>(s));
        std::printf("] rawOffset=%llu md5=%s\n",
                    static_cast<unsigned long long>(node->rawOffset), oracleMd5);

        // ── Recovery bytes ───────────────────────────────────────────────
        RecoverEngine engine;
        const FileRecoverResult rr =
            engine.RecoverFile(*node, outDir, false, reader, part);
        CHECK(rr.status == RecoverStatus::Success, "RecoverEngine writes the file");
        if (rr.status != RecoverStatus::Success || rr.targetPath.empty()) continue;

        const std::vector<uint8_t> recovered = ReadFileBytes(rr.targetPath);
        CHECK(recovered.size() == c.size, "recovered byte count matches oracle");
        CHECK(recovered.size() == oracleBytes.size() &&
              std::memcmp(recovered.data(), oracleBytes.data(),
                          recovered.size()) == 0,
              "RECOVERY-BYTE GATE: written bytes identical to NoFatChain oracle");
        char recoveredMd5[33]{};
        Md5Hex(recovered.data(), recovered.size(), recoveredMd5);
        CHECK(std::strcmp(recoveredMd5, c.md5) == 0,
              "recovered MD5 matches independent Python oracle");
    }

    reader.Close();
    const uint64_t hashAfter = FnvWholeImage(image);
    CHECK(hashAfter == hashBefore, "READ-ONLY INVARIANT: whole image unchanged");
}

} // namespace

int main()
{
    for (const ImageGroup& g : kGroups)
        CheckGroup(g);

    std::printf("\n%s: ExfatMulticlusterBytes %d checks, %d failure(s)\n",
                g_fails ? "FAIL" : "OK", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

#endif
