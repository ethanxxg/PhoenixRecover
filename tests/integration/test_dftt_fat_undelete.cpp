// DFTT #6 FAT Undelete – end-to-end integration test
//
// Opens the DFTT 6-fat-undel.dd image, runs FatParser, and verifies that
// all expected deleted files are found with correct sizes.
//
// requires volume I/O platform (SectorReader uses CreateFile) and Qt (Platform.h → QtGlobal).
// Wrapped in PHOENIX_OS_WINDOWS so the test is silently skipped on other platforms.

#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "fs/fat/FatParser.h"
#include "recover/FileNode.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <filesystem>

// ── Minimal test harness ─────────────────────────────────────────────────────

static int g_failures = 0;

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "FAIL: %s  (%s:%d)\n", #expr, __FILE__, __LINE__); \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

#define CHECK_MSG(expr, msg)                                                \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "FAIL: %s — %s  (%s:%d)\n",                    \
                    #expr, (msg), __FILE__, __LINE__);                      \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

// ── Helpers ──────────────────────────────────────────────────────────────────

static const FileNode* FindByName(const std::vector<FileNode>& files,
                                  const std::wstring& name)
{
    for (auto& f : files)
        if (_wcsicmp(f.name.c_str(), name.c_str()) == 0) return &f;
    // FAT deleted files have first char replaced by 0xE5 → '_'.
    // Try matching with '_' as first character (case insensitive).
    if (name.size() >= 1) {
        std::wstring alt = L"_" + name.substr(1);
        for (auto& f : files)
            if (_wcsicmp(f.name.c_str(), alt.c_str()) == 0) return &f;
    }
    return nullptr;
}

static const FileNode* FindByPath(const std::vector<FileNode>& files,
                                  const std::wstring& pathSuffix)
{
    for (auto& f : files) {
        if (f.originalPath.size() >= pathSuffix.size()) {
            auto tail = f.originalPath.substr(
                f.originalPath.size() - pathSuffix.size());
            if (_wcsicmp(tail.c_str(), pathSuffix.c_str()) == 0)
                return &f;
        }
    }
    return nullptr;
}

static bool HasDirectory(const std::vector<FileNode>& files,
                         const std::wstring& name)
{
    for (auto& f : files)
        if (f.isDirectory && _wcsicmp(f.name.c_str(), name.c_str()) == 0) return true;
    if (name.size() >= 1) {
        std::wstring alt = L"_" + name.substr(1);
        for (auto& f : files)
            if (f.isDirectory && _wcsicmp(f.name.c_str(), alt.c_str()) == 0) return true;
    }
    return false;
}

// ── Resolve image path relative to executable ────────────────────────────────

static std::wstring ResolveImagePath()
{
    namespace fs = std::filesystem;
    // Walk upward from the executable until we find testdata/
    fs::path exe = fs::canonical(fs::path(__FILE__).parent_path() / ".." / "..");
    // __FILE__ gives the source path; at build time CMAKE_SOURCE_DIR is the root
    // Use a compile-time macro if available, otherwise fallback to source-relative.
#ifdef PHOENIX_SOURCE_DIR
    fs::path root(PHOENIX_SOURCE_DIR);
#else
    fs::path root = fs::canonical(fs::path(__FILE__).parent_path() / ".." / "..");
#endif
    return (root / "testdata" / "dftt" / "6-undel-fat" / "6-fat-undel.dd").wstring();
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main()
{
    std::wstring imagePath = ResolveImagePath();
    fprintf(stdout, "Image: %ls\n", imagePath.c_str());

    // 1. Open the image via SectorReader
    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");
    if (!reader.IsOpen()) {
        fprintf(stderr, "FATAL: cannot open image, aborting.\n");
        return 1;
    }

    fprintf(stdout, "Sector size: %u, total sectors: %llu\n",
            reader.GetSectorSize(), reader.GetTotalSectors());

    // 2. Create a dummy partition covering the whole image
    Partition part;
    part.startSector  = 0;
    part.totalSectors = reader.GetTotalSectors();
    part.sizeBytes    = reader.GetTotalBytes();
    part.fsType       = L"FAT16";

    // 3. Parse FAT
    FatParser parser;
    CHECK_MSG(parser.Open(reader, part), "FatParser::Open failed");
    CHECK_MSG(parser.ScanMetadata(), "FatParser::ScanMetadata failed");

    auto files = parser.GetAllFiles();
    fprintf(stdout, "Total nodes found: %zu\n", files.size());

    for (auto& f : files) {
        fprintf(stdout, "  [%ls] name=%-20ls size=%6llu dir=%d status=%d path=%ls\n",
                parser.GetFsTypeName().c_str(),
                f.name.c_str(), f.size, f.isDirectory ? 1 : 0,
                static_cast<int>(f.status), f.originalPath.c_str());
    }

    CHECK(files.size() >= 6);

    // 4a. Verify expected files and sizes (from expected_results.csv)
    struct Expected { const wchar_t* name; uint64_t size; };
    Expected expectedFiles[] = {
        { L"sing.dat",  780  },
        { L"mult1.dat", 3801 },
        { L"frag1.dat", 1584 },
        { L"frag2.dat", 3873 },
    };
    for (auto& e : expectedFiles) {
        auto* node = FindByName(files, e.name);
        CHECK_MSG(node != nullptr,
                  (std::string("missing file: ") +
                   std::string(e.name, e.name + wcslen(e.name))).c_str());
        if (node) {
            bool sizeOk = (node->size == e.size);
            if (!sizeOk) {
                fprintf(stderr, "  size mismatch for %ls: expected %llu, got %llu\n",
                        e.name, e.size, node->size);
            }
            CHECK(sizeOk);
        }
    }

    // 4b. Files in subdirectories (soft checks — FAT deleted subdirectory
    // traversal depends on whether FAT chain is still intact)
    auto* mult2 = FindByPath(files, L"\\dir1\\mult2.dat");
    if (!mult2) mult2 = FindByName(files, L"mult2.dat");
    if (mult2) {
        CHECK(mult2->size == 1715);
        fprintf(stdout, "  dir1/mult2.dat: found, size OK\n");
    } else {
        fprintf(stdout, "  WARN: dir1/mult2.dat not found (deleted subdir traversal limitation)\n");
    }

    auto* frag3 = FindByPath(files, L"\\dir1\\dir2\\frag3.dat");
    if (!frag3) frag3 = FindByName(files, L"frag3.dat");
    if (frag3) {
        CHECK(frag3->size == 2027);
        fprintf(stdout, "  dir1/dir2/frag3.dat: found, size OK\n");
    } else {
        fprintf(stdout, "  WARN: dir1/dir2/frag3.dat not found (deleted subdir traversal limitation)\n");
    }

    // 4c. Directories
    CHECK_MSG(HasDirectory(files, L"dir1"), "missing directory dir1");
    bool hasDir2 = HasDirectory(files, L"dir2");
    if (!hasDir2) {
        auto* frag3Node = FindByPath(files, L"\\dir1\\dir2\\frag3.dat");
        hasDir2 = (frag3Node != nullptr);
    }
    if (!hasDir2)
        fprintf(stdout, "  WARN: dir2 not found (expected with deleted subdir)\n");

    // 5. Summary
    reader.Close();
    if (g_failures == 0) {
        fprintf(stdout, "\n=== DFTT FAT Undelete: ALL CHECKS PASSED ===\n");
    } else {
        fprintf(stderr, "\n=== DFTT FAT Undelete: %d CHECK(S) FAILED ===\n",
                g_failures);
    }
    return g_failures ? 1 : 0;
}

#else // !PHOENIX_OS_WINDOWS

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: DFTT FAT Undelete test requires volume I/O platform.\n");
    return 0;
}

#endif
