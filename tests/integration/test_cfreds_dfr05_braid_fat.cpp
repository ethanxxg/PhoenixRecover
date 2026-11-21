// CFReDS DFR-05-BRAID-FAT — interleaved fragmentation dataset (metadata visibility).
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "fs/fat/FatParser.h"
#include "recover/FileNode.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK_MSG(expr, msg) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s — %s (%s:%d)\n", #expr, (msg), __FILE__, __LINE__); \
            ++g_failures; \
        } \
    } while (0)

static std::wstring ImagePath()
{
#ifdef PHOENIX_SOURCE_DIR
    namespace fs = std::filesystem;
    return (fs::path(PHOENIX_SOURCE_DIR) / "testdata" / "cfreds" / "dfr-05-braid-fat.dd").wstring();
#else
    return L"";
#endif
}

struct MbrEntry {
    uint64_t startSector;
    uint64_t totalSectors;
    uint8_t  type;
};

static bool ParseMbr(SectorReader& reader, std::vector<MbrEntry>& entries)
{
    std::vector<uint8_t> mbr(512);
    if (!reader.ReadSectors(0, mbr.data(), 1)) return false;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return false;

    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = mbr.data() + 446 + i * 16;
        uint8_t type = e[4];
        if (type == 0) continue;
        uint32_t start = *reinterpret_cast<const uint32_t*>(e + 8);
        uint32_t count = *reinterpret_cast<const uint32_t*>(e + 12);
        if (start > 0 && count > 0) entries.push_back({start, count, type});
    }
    return !entries.empty();
}

static bool PartitionHasDeleted(SectorReader& reader, uint64_t startSector, uint64_t totalSectors,
                                const wchar_t* label)
{
    Partition part;
    part.startSector  = startSector;
    part.totalSectors = totalSectors;
    part.sizeBytes    = totalSectors * reader.GetSectorSize();
    part.fsType       = L"FAT";

    FatParser parser;
    if (!parser.Open(reader, part) || !parser.ScanMetadata()) {
        fprintf(stderr, "  [%ls] FatParser open/scan failed\n", label);
        return false;
    }

    auto files = parser.GetAllFiles();
    int deleted = 0;
    for (auto& f : files) {
        if (f.status == FileStatus::Deleted && !f.isDirectory) ++deleted;
    }

    fprintf(stdout, "  [%ls] start=%6llu sectors=%llu files=%zu deleted=%d fs=%ls\n",
            label, startSector, totalSectors, files.size(), deleted, parser.GetFsTypeName().c_str());
    return deleted > 0;
}

int main()
{
    std::wstring imagePath = ImagePath();
    if (imagePath.empty() || !std::filesystem::exists(imagePath)) {
        fprintf(stdout, "SKIP: CFReDS dfr-05-braid-fat.dd not found at %ls\n", imagePath.c_str());
        return 0;
    }

    fprintf(stdout, "Image: %ls\n", imagePath.c_str());

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");
    if (!reader.IsOpen()) return 1;

    std::vector<MbrEntry> entries;
    CHECK_MSG(ParseMbr(reader, entries), "MBR parse failed");
    CHECK_MSG(!entries.empty(), "Expected MBR partitions");

    int partitionsWithDeleted = 0;
    for (size_t i = 0; i < entries.size(); ++i) {
        auto& e = entries[i];
        if (e.type == 0x05 || e.type == 0x0F || e.type == 0x85) continue;
        std::wstring label = L"Part" + std::to_wstring(i);
        if (PartitionHasDeleted(reader, e.startSector, e.totalSectors, label.c_str())) {
            ++partitionsWithDeleted;
        }
    }

    CHECK_MSG(partitionsWithDeleted >= 1, "At least 1 FAT partition should contain deleted files (braid)");

    reader.Close();
    return g_failures ? 1 : 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: CFReDS DFR-05 braid FAT test requires volume I/O platform.\n");
    return 0;
}

#endif

