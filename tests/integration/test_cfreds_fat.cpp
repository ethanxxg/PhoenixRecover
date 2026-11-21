// CFReDS DFR-01-FAT — deleted-file-recovery integration test
//
// Opens the NIST CFReDS dfr-01-fat.dd image (1 GB, contains FAT12/16/32
// partitions with known deleted files), runs FatParser on each partition,
// and verifies that deleted files are discovered.
//
// Pure C++ — does NOT depend on Qt.

#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "disk/PartitionProbe.h"
#include "fs/fat/FatParser.h"
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

static std::wstring ImagePath()
{
#ifdef PHOENIX_SOURCE_DIR
    namespace fs = std::filesystem;
    return (fs::path(PHOENIX_SOURCE_DIR) / "testdata" / "cfreds" / "dfr-01-fat.dd").wstring();
#else
    return L"";
#endif
}

static bool FindDeletedFiles(SectorReader& reader, uint64_t startSector,
                             uint64_t totalSectors, const std::wstring& label)
{
    Partition part;
    part.startSector  = startSector;
    part.totalSectors = totalSectors;
    part.sizeBytes    = totalSectors * reader.GetSectorSize();
    part.fsType       = L"FAT";

    FatParser parser;
    if (!parser.Open(reader, part)) {
        fprintf(stderr, "  [%ls] FatParser::Open failed (start=%llu)\n",
                label.c_str(), startSector);
        return false;
    }
    if (!parser.ScanMetadata()) {
        fprintf(stderr, "  [%ls] FatParser::ScanMetadata failed\n", label.c_str());
        return false;
    }

    auto files = parser.GetAllFiles();
    int deleted = 0;
    for (auto& f : files)
        if (f.status == FileStatus::Deleted && !f.isDirectory) ++deleted;

    fprintf(stdout, "  [%ls] start=%6llu  sectors=%llu  files=%zu  deleted=%d  fs=%ls\n",
            label.c_str(), startSector, totalSectors,
            files.size(), deleted, parser.GetFsTypeName().c_str());

    return deleted > 0;
}

int main()
{
    std::wstring imagePath = ImagePath();
    if (imagePath.empty() || !std::filesystem::exists(imagePath)) {
        fprintf(stdout, "SKIP: CFReDS dfr-01-fat.dd not found at %ls\n", imagePath.c_str());
        return 0;
    }

    fprintf(stdout, "Image: %ls\n", imagePath.c_str());

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");
    if (!reader.IsOpen()) { fprintf(stderr, "FATAL: cannot open image\n"); return 1; }

    fprintf(stdout, "Sector size: %u, total sectors: %llu, total bytes: %llu MB\n",
            reader.GetSectorSize(), reader.GetTotalSectors(),
            reader.GetTotalBytes() / (1024 * 1024));

    // Read MBR to find partitions
    std::vector<uint8_t> mbr(512);
    CHECK_MSG(reader.ReadSectors(0, mbr.data(), 1), "ReadSectors(0) failed");

    // Verify MBR signature
    CHECK_MSG(mbr[510] == 0x55 && mbr[511] == 0xAA, "MBR signature missing");

    // Parse MBR partition table (4 entries at offset 446)
    struct MbrEntry {
        uint64_t startSector;
        uint64_t totalSectors;
        uint8_t  type;
    };
    std::vector<MbrEntry> entries;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = mbr.data() + 446 + i * 16;
        uint8_t type = e[4];
        if (type == 0) continue;
        uint32_t startLBA = *reinterpret_cast<const uint32_t*>(e + 8);
        uint32_t numSects = *reinterpret_cast<const uint32_t*>(e + 12);
        if (startLBA > 0 && numSects > 0) {
            entries.push_back({startLBA, numSects, type});
            fprintf(stdout, "MBR[%d]: type=0x%02X start=%u sectors=%u\n",
                    i, type, startLBA, numSects);
        }
    }

    CHECK_MSG(entries.size() >= 2, "Expected at least 2 partitions in CFReDS FAT image");

    // Try to parse each partition as FAT and find deleted files
    int partitionsWithDeleted = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        auto& e = entries[i];
        // Skip extended partitions
        if (e.type == 0x05 || e.type == 0x0F || e.type == 0x85) continue;
        std::wstring label = L"Part" + std::to_wstring(i);
        if (FindDeletedFiles(reader, e.startSector, e.totalSectors, label))
            ++partitionsWithDeleted;
    }

    CHECK_MSG(partitionsWithDeleted >= 1,
              "At least 1 partition should contain deleted files");

    reader.Close();

    if (g_failures == 0) {
        fprintf(stdout, "\n=== CFReDS FAT DFR-01: ALL CHECKS PASSED (%d partitions with deleted files) ===\n",
                partitionsWithDeleted);
    } else {
        fprintf(stderr, "\n=== CFReDS FAT DFR-01: %d CHECK(S) FAILED ===\n", g_failures);
    }
    return g_failures ? 1 : 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: CFReDS FAT test requires volume I/O platform.\n");
    return 0;
}

#endif
