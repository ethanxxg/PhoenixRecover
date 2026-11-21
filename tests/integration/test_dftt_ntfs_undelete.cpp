// DFTT #7 NTFS Undelete – end-to-end integration test
//
// Opens the DFTT 7-ntfs-undel.dd image, runs NtfsParser, and verifies that
// all expected deleted files are found with correct sizes, including the
// resident file res1.dat.
//
// requires volume I/O platform (SectorReader uses CreateFile) and Qt (Platform.h → QtGlobal).

#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "fs/ntfs/NtfsParser.h"
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
        if (f.name == name) return &f;
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

// ── Resolve image path ───────────────────────────────────────────────────────

static std::wstring ResolveImagePath()
{
    namespace fs = std::filesystem;
#ifdef PHOENIX_SOURCE_DIR
    fs::path root(PHOENIX_SOURCE_DIR);
#else
    fs::path root = fs::canonical(fs::path(__FILE__).parent_path() / ".." / "..");
#endif
    return (root / "testdata" / "dftt" / "7-undel-ntfs" / "7-ntfs-undel.dd").wstring();
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main()
{
    std::wstring imagePath = ResolveImagePath();
    fprintf(stdout, "Image: %ls\n", imagePath.c_str());

    // 1. Open image
    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");
    if (!reader.IsOpen()) {
        fprintf(stderr, "FATAL: cannot open image, aborting.\n");
        return 1;
    }

    fprintf(stdout, "Sector size: %u, total sectors: %llu\n",
            reader.GetSectorSize(), reader.GetTotalSectors());

    // 2. Partition covering entire image
    Partition part;
    part.startSector  = 0;
    part.totalSectors = reader.GetTotalSectors();
    part.sizeBytes    = reader.GetTotalBytes();
    part.fsType       = L"NTFS";

    // 3. Parse NTFS
    NtfsParser parser;
    CHECK_MSG(parser.Open(reader, part), "NtfsParser::Open failed");
    CHECK_MSG(parser.ScanMetadata(), "NtfsParser::ScanMetadata failed");

    auto files = parser.GetAllFiles();
    fprintf(stdout, "Total nodes found: %zu\n", files.size());
    CHECK(files.size() >= 5);

    // 4a. Resident file (res1.dat – 101 bytes, data stored in MFT record)
    auto* res1 = FindByName(files, L"res1.dat");
    CHECK_MSG(res1 != nullptr, "missing res1.dat (resident file)");
    if (res1) {
        CHECK(res1->size == 101);
        CHECK_MSG(!res1->residentData.empty(),
                  "res1.dat residentData should be non-empty");
        if (!res1->residentData.empty()) {
            CHECK(res1->residentData.size() == 101);
        }
    }

    // 4b. Root-level files with sizes
    struct Expected { const wchar_t* name; uint64_t size; };
    Expected rootFiles[] = {
        { L"sing1.dat", 780  },
        { L"mult1.dat", 3801 },
        { L"frag1.dat", 1584 },
        { L"frag2.dat", 3873 },
    };
    for (auto& e : rootFiles) {
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

    // 4c. Files in subdirectories
    struct PathExpected { const wchar_t* path; const wchar_t* fallbackName; uint64_t size; };
    PathExpected subFiles[] = {
        { L"\\dir1\\mult2.dat",       L"mult2.dat", 1715 },
        { L"\\dir1\\dir2\\frag3.dat", L"frag3.dat", 2027 },
        { L"\\dir3\\sing2.dat",       L"sing2.dat", 1005 },
    };
    for (auto& e : subFiles) {
        auto* node = FindByPath(files, e.path);
        if (!node) node = FindByName(files, e.fallbackName);
        CHECK_MSG(node != nullptr,
                  (std::string("missing: ") +
                   std::string(e.path, e.path + wcslen(e.path))).c_str());
        if (node) {
            bool sizeOk = (node->size == e.size);
            if (!sizeOk) {
                fprintf(stderr, "  size mismatch for %ls: expected %llu, got %llu\n",
                        e.path, e.size, node->size);
            }
            CHECK(sizeOk);
        }
    }

    // 5. Summary
    reader.Close();
    if (g_failures == 0) {
        fprintf(stdout, "\n=== DFTT NTFS Undelete: ALL CHECKS PASSED ===\n");
    } else {
        fprintf(stderr, "\n=== DFTT NTFS Undelete: %d CHECK(S) FAILED ===\n",
                g_failures);
    }
    return g_failures ? 1 : 0;
}

#else // !PHOENIX_OS_WINDOWS

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: DFTT NTFS Undelete test requires volume I/O platform.\n");
    return 0;
}

#endif
