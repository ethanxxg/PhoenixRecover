// CFReDS DFR-01-NTFS — deleted-file-recovery integration test
//
// Opens the NIST CFReDS dfr-01-ntfs.dd image (1 GB, single NTFS partition
// with known deleted files), runs NtfsParser, and verifies that deleted
// files are discovered.
//
// Pure C++ — does NOT depend on Qt.

#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "disk/PartitionProbe.h"
#include "fs/ntfs/NtfsParser.h"
#include "recover/FileNode.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <filesystem>

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

static const FileNode* FindByName(const std::vector<FileNode>& files,
                                  const std::wstring& name)
{
    for (auto& f : files)
        if (f.name == name) return &f;
    return nullptr;
}

static std::wstring ImagePath()
{
#ifdef PHOENIX_SOURCE_DIR
    namespace fs = std::filesystem;
    return (fs::path(PHOENIX_SOURCE_DIR) / "testdata" / "cfreds" / "dfr-01-ntfs.dd").wstring();
#else
    return L"";
#endif
}

int main()
{
    std::wstring imagePath = ImagePath();
    if (imagePath.empty() || !std::filesystem::exists(imagePath)) {
        fprintf(stdout, "SKIP: CFReDS dfr-01-ntfs.dd not found at %ls\n", imagePath.c_str());
        return 0;
    }

    fprintf(stdout, "Image: %ls\n", imagePath.c_str());

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");
    if (!reader.IsOpen()) { fprintf(stderr, "FATAL: cannot open image\n"); return 1; }

    fprintf(stdout, "Sector size: %u, total sectors: %llu, total bytes: %llu MB\n",
            reader.GetSectorSize(), reader.GetTotalSectors(),
            reader.GetTotalBytes() / (1024 * 1024));

    // Read MBR to find the NTFS partition
    std::vector<uint8_t> mbr(512);
    CHECK_MSG(reader.ReadSectors(0, mbr.data(), 1), "ReadSectors(0) failed");
    CHECK_MSG(mbr[510] == 0x55 && mbr[511] == 0xAA, "MBR signature missing");

    // Parse MBR partition table
    uint64_t ntfsStart = 0, ntfsSectors = 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = mbr.data() + 446 + i * 16;
        uint8_t type = e[4];
        if (type == 0) continue;
        uint32_t startLBA = *reinterpret_cast<const uint32_t*>(e + 8);
        uint32_t numSects = *reinterpret_cast<const uint32_t*>(e + 12);
        fprintf(stdout, "MBR[%d]: type=0x%02X start=%u sectors=%u\n",
                i, type, startLBA, numSects);
        if (type == 0x07 && ntfsStart == 0) {
            ntfsStart = startLBA;
            ntfsSectors = numSects;
        }
    }

    // If no type-0x07 found, try the first partition
    if (ntfsStart == 0) {
        const uint8_t* e = mbr.data() + 446;
        uint8_t type = e[4];
        if (type != 0) {
            ntfsStart = *reinterpret_cast<const uint32_t*>(e + 8);
            ntfsSectors = *reinterpret_cast<const uint32_t*>(e + 12);
            fprintf(stdout, "No type-0x07 found, trying first partition (type=0x%02X)\n", type);
        }
    }

    CHECK_MSG(ntfsStart > 0 && ntfsSectors > 0, "Could not find a partition in MBR");

    Partition part;
    part.startSector  = ntfsStart;
    part.totalSectors = ntfsSectors;
    part.sizeBytes    = ntfsSectors * reader.GetSectorSize();
    part.fsType       = L"NTFS";

    NtfsParser parser;
    CHECK_MSG(parser.Open(reader, part), "NtfsParser::Open failed");
    CHECK_MSG(parser.ScanMetadata(), "NtfsParser::ScanMetadata failed");

    auto files = parser.GetAllFiles();
    int totalDeleted = 0;
    for (auto& f : files)
        if (f.status == FileStatus::Deleted && !f.isDirectory) ++totalDeleted;

    fprintf(stdout, "Total nodes: %zu, deleted files: %d, in-use: %u, total records: %u\n",
            files.size(), totalDeleted, parser.GetInUseCount(), parser.GetTotalRecordCount());

    CHECK_MSG(totalDeleted >= 1, "Expected at least 1 deleted file in NTFS partition");

    // List all deleted files for diagnostic purposes
    fprintf(stdout, "\nDeleted files found:\n");
    for (auto& f : files) {
        if (f.status == FileStatus::Deleted && !f.isDirectory) {
            fprintf(stdout, "  %-30ls  size=%6llu  clusters=%zu  resident=%s\n",
                    f.name.c_str(), f.size, f.clusters.size(),
                    f.residentData.empty() ? "no" : "yes");
        }
    }

    reader.Close();

    if (g_failures == 0) {
        fprintf(stdout, "\n=== CFReDS NTFS DFR-01: ALL CHECKS PASSED (%d deleted files found) ===\n",
                totalDeleted);
    } else {
        fprintf(stderr, "\n=== CFReDS NTFS DFR-01: %d CHECK(S) FAILED ===\n", g_failures);
    }
    return g_failures ? 1 : 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: CFReDS NTFS test requires volume I/O platform.\n");
    return 0;
}

#endif
