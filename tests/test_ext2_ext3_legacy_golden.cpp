// W6: independent ext2 / ext3 legacy golden (file-recovery-first, Linux FS).
//
// Ground truth was produced by a standalone Python oracle that parses the raw
// images directly (superblock -> group descriptors -> inode table -> linear
// directory entries -> i_block[0..11] direct blocks).  No PhoenixRecover
// parser participates in the oracle; its values are pinned below as constants.
//
//   image A  testdata/forensics-samples/fs.ext2
//            MBR disk image, partition 0 type 0x83 @ LBA 2048, 100352 sectors.
//            ext2: magic 0xEF53, 1024 B blocks, 128 B inodes,
//            compat=0x38 (ext_attr|resize_inode|dir_index),
//            incompat=0x2 (filetype), ro_compat=0x3 (sparse_super|large_file).
//            NO extents -> exercises Ext4Parser::GetIndirectBlocks().
//            Live tree: audio1/ movie1/ pic1/ text1/ with 19 regular files.
//            Deleted (swallowed by the preceding dirent's rec_len):
//              audio2 ino=8961, movie2 ino=1793, pic2 ino=3587, text2 ino=7173
//            All four have dtime!=0, links=0, size=0, i_block[]=0: content is
//            gone, but the ORIGINAL NAME survives in the dirent slack.
//
//   image B  testdata/sleuthkit_test_data/from_brian/4-kwsrch-ext3/ext3-img-kw-1.dd
//            Bare ext3 volume, 5 MiB, magic 0xEF53, 1024 B blocks, 128 B inodes,
//            compat=0x4 (has_journal), incompat=0x2 (filetype), ro=0x1.
//            Live: file1 601 B (block 1208), file2 1300 B (blocks 1209,1212),
//                  first 63 B (block 1211).
//            Deleted: file3 ino=14 (swallowed inside file2's rec_len).
//
// This gate checks: live file names/sizes, legacy block-map recovery bytes via
// RecoverEngine (MD5), deleted files retaining their ORIGINAL names, and the
// read-only source invariant (whole-image FNV-1a unchanged).
//
// No <filesystem>, no Qt: must build under both MinGW g++ 7.3 and GCC 14.2.

#include "common/Platform.h"

#if PHOENIX_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

#include "common/Utils.h"
#include "disk/Partition.h"
#include "disk/SectorReader.h"
#include "fs/FileSystem.h"
#include "fs/ext4/Ext4Parser.h"
#include "recover/FileNode.h"
#include "recover/RecoverEngine.h"
#include "scan/ScanEngine.h"

#include <chrono>
#include <cstdio>
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

// ── Compact, dependency-free MD5 (RFC 1321), self-tested against known vectors ──
uint32_t RotL(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

void Md5Block(uint32_t s[4], const uint8_t* p)
{
    static const uint32_t k[64] = {
        0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
        0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
        0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
        0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
        0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
        0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
        0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
        0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u
    };
    static const int r[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = static_cast<uint32_t>(p[i * 4]) |
               (static_cast<uint32_t>(p[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(p[i * 4 + 2]) << 16) |
               (static_cast<uint32_t>(p[i * 4 + 3]) << 24);
    }
    uint32_t a = s[0], b = s[1], c = s[2], d = s[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (b & c) | (~b & d);      g = i; }
        else if (i < 32) { f = (d & b) | (~d & c);      g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d;               g = (3 * i + 5) % 16; }
        else             { f = c ^ (b | ~d);            g = (7 * i) % 16; }
        const uint32_t tmp = d;
        d = c; c = b;
        b = b + RotL(a + f + k[i] + m[g], r[i]);
        a = tmp;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}

bool Md5Hex(const uint8_t* data, size_t len, char out33[33])
{
    uint32_t s[4] = {0x67452301u,0xefcdab89u,0x98badcfeu,0x10325476u};
    size_t off = 0;
    uint8_t tail[128]{};
    while (len - off >= 64) { Md5Block(s, data + off); off += 64; }
    const size_t rem = len - off;
    if (rem) std::memcpy(tail, data + off, rem);
    tail[rem] = 0x80;
    const size_t padLen = (rem <= 55) ? 64 : 128;
    const uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i)
        tail[padLen - 8 + i] = static_cast<uint8_t>(bits >> (8 * i));
    Md5Block(s, tail);
    if (padLen == 128) Md5Block(s, tail + 64);
    static const char hexd[] = "0123456789abcdef";
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            const uint8_t b = static_cast<uint8_t>(s[i] >> (8 * j));
            out33[(i * 4 + j) * 2]     = hexd[b >> 4];
            out33[(i * 4 + j) * 2 + 1] = hexd[b & 15];
        }
    out33[32] = '\0';
    return true;
}

bool Md5SelfTest()
{
    char a[33]{}, b[33]{};
    Md5Hex(reinterpret_cast<const uint8_t*>(""), 0, a);
    Md5Hex(reinterpret_cast<const uint8_t*>("abc"), 3, b);
    return std::strcmp(a, "d41d8cd98f00b204e9800998ecf8427e") == 0 &&
           std::strcmp(b, "900150983cd24fb0d6963f7d28e17f72") == 0;
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

std::vector<uint8_t> ReadFileBytes(const std::wstring& path)
{
    std::ifstream f(Utils::WideToUtf8(path), std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

bool AsciiEq(const std::wstring& a, const char* b)
{
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (static_cast<unsigned char>(a[i]) != static_cast<unsigned char>(b[i]))
            return false;
    return i == a.size() && b[i] == '\0';
}

std::wstring ImagePath(const char* rel)
{
    return std::wstring(PHOENIX_SOURCE_DIR) + L"/testdata/" +
           Utils::Utf8ToWide(rel);
}

// ── Independent Python oracle, pinned as constants ─────────────────────────
struct ExpectedFile
{
    const char* name;
    uint64_t    size;
    const char* md5;      // nullptr = do not recover bytes
    bool        deleted;
};

const ExpectedFile kExt3[] = {
    {"file1",  601, "04c0acf459ebb102224bbde8b013c648", false},
    {"file2", 1300, "ca7f6af5315461ae36327fab78a8a1f0", false},
    {"first",   63, "46fd6ba5c76be42dd8dc04f2f1c050c6", false},
    {"file3",    0, nullptr,                             true },
};

const ExpectedFile kExt2[] = {
    {"debian.mp3",             69727, nullptr, false},
    {"VID_20191220_170832.mp4", 2942343, nullptr, false},
    {"a-text.docx",             4385, nullptr, false},
    {"debian_logo.png",         1734, nullptr, false},
    {"empty.jpg",               1142, nullptr, false},
    {"audio2",                     0, nullptr, true },
    {"movie2",                     0, nullptr, true },
    {"pic2",                       0, nullptr, true },
    {"text2",                      0, nullptr, true },
};

struct ImageSpec
{
    const char* rel;
    uint64_t    startSector;
    uint64_t    totalSectors;
    const ExpectedFile* expected;
    size_t      expectedCount;
    const char* label;
};

const ImageSpec kImages[] = {
    {"sleuthkit_test_data/from_brian/4-kwsrch-ext3/ext3-img-kw-1.dd",
     0, 10240, kExt3, sizeof(kExt3) / sizeof(kExt3[0]), "ext3 bare volume"},
    {"forensics-samples/fs.ext2",
     2048, 100352, kExt2, sizeof(kExt2) / sizeof(kExt2[0]), "ext2 MBR partition 0 @2048"},
};

std::wstring MakeTempOutDir()
{
#if PHOENIX_OS_WINDOWS
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"PhoenixRecover_ext_golden_" +
                       std::to_wstring(GetTickCount()) + L"_" +
                       std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
#else
    return std::wstring(L"/tmp/PhoenixRecover_ext_golden_") +
           std::to_wstring(static_cast<unsigned long long>(
               std::chrono::steady_clock::now().time_since_epoch().count()));
#endif
}

const FileNode* FindByName(const std::vector<FileNode>& files, const char* name)
{
    for (const FileNode& n : files)
        if (AsciiEq(n.name, name)) return &n;
    return nullptr;
}

void CheckImage(const ImageSpec& spec)
{
    const std::wstring image = ImagePath(spec.rel);
    std::printf("\n=== %s ===\n  image: %ls\n", spec.label, image.c_str());
    if (!Utils::FileExists(image)) {
        std::printf("  SKIP: image missing\n");
        return;
    }

    const uint64_t hashBefore = FnvWholeImage(image);
    CHECK(hashBefore != 0, "whole-image FNV-1a computed before scan");

    SectorReader reader;
    CHECK(reader.OpenImage(image), "SectorReader opens image");
    if (!reader.IsOpen()) return;

    Partition part;
    part.startSector  = spec.startSector;
    part.totalSectors = spec.totalSectors;
    part.sizeBytes    = spec.totalSectors * reader.GetSectorSize();
    part.fsType       = L"ext4";   // Ext4Parser handles ext2/3 via legacy block map

    Ext4Parser parser;
    CHECK(parser.Open(reader, part), "Ext4Parser::Open succeeds on legacy ext volume");
    if (!parser.IsValid()) { reader.Close(); return; }
    CHECK(parser.ScanMetadata(nullptr, nullptr), "Ext4Parser::ScanMetadata succeeds");
    const std::vector<FileNode> files = parser.GetAllFiles();
    CHECK(!files.empty(), "parser reports at least one node");
    std::printf("  nodes=%zu  totalRecords=%u  inUse=%u\n",
                files.size(), parser.GetTotalRecordCount(), parser.GetInUseCount());

    for (size_t i = 0; i < spec.expectedCount; ++i) {
        const ExpectedFile& e = spec.expected[i];
        const FileNode* n = FindByName(files, e.name);
        const bool found = n != nullptr;
        CHECK(found, e.deleted
              ? "deleted entry retains its ORIGINAL name from dirent slack"
              : "live file is discovered by name");
        if (!n) {
            std::fprintf(stderr, "       missing: %s (deleted=%d)\n",
                         e.name, e.deleted ? 1 : 0);
            continue;
        }
        CHECK(n->size == e.size, "node size matches independent oracle");
        CHECK(n->status == (e.deleted ? FileStatus::Deleted : FileStatus::Normal),
              "node status matches independent oracle");
        std::printf("  %-28s size=%-9llu status=%-8s dir=%d clusters=%zu\n",
                    e.name, static_cast<unsigned long long>(n->size),
                    n->status == FileStatus::Deleted ? "deleted" : "normal",
                    n->isDirectory ? 1 : 0, n->clusters.size());

        if (!e.md5) continue;

        // Recovery-byte gate: RecoverEngine must reproduce the exact payload.
        const std::wstring outDir = MakeTempOutDir();
        RecoverEngine engine;
        const FileRecoverResult rr =
            engine.RecoverFile(*n, outDir, false, reader, part);
        CHECK(rr.status == RecoverStatus::Success, "RecoverEngine writes the file");
        if (rr.status != RecoverStatus::Success || rr.targetPath.empty()) continue;
        const std::vector<uint8_t> recovered = ReadFileBytes(rr.targetPath);
        CHECK(recovered.size() == e.size, "recovered byte count matches oracle");
        char md5[33]{};
        Md5Hex(recovered.data(), recovered.size(), md5);
        CHECK(std::strcmp(md5, e.md5) == 0,
              "RECOVERY-BYTE GATE: written MD5 matches independent Python oracle");
        if (std::strcmp(md5, e.md5) != 0)
            std::fprintf(stderr, "       %s: got %s want %s\n", e.name, md5, e.md5);
    }

    // ext2: the four live subdirectory roots must be present too.
    if (spec.startSector == 2048) {
        static const char* const dirs[] = {"audio1", "movie1", "pic1", "text1"};
        for (const char* d : dirs) {
            const FileNode* n = FindByName(files, d);
            CHECK(n != nullptr, "live subdirectory root discovered");
            if (n) CHECK(n->isDirectory, "subdirectory node is marked as directory");
        }
    }

    reader.Close();
    const uint64_t hashAfter = FnvWholeImage(image);
    CHECK(hashAfter == hashBefore, "READ-ONLY INVARIANT: whole image unchanged");
}

} // namespace

int main()
{
    CHECK(Md5SelfTest(), "MD5 implementation self-test (empty + abc vectors)");
    for (const ImageSpec& spec : kImages)
        CheckImage(spec);

    std::printf("\n%s: Ext2Ext3LegacyGolden %d checks, %d failure(s)\n",
                g_fails ? "FAIL" : "OK", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
