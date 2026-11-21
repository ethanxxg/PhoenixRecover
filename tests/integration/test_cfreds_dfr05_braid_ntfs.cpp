// CFReDS DFR-05-BRAID-NTFS — interleaved fragmentation dataset (metadata visibility).
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "fs/ntfs/NtfsParser.h"
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
    return (fs::path(PHOENIX_SOURCE_DIR) / "testdata" / "cfreds" / "dfr-05-braid-ntfs.dd").wstring();
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

int main()
{
    std::wstring imagePath = ImagePath();
    if (imagePath.empty() || !std::filesystem::exists(imagePath)) {
        fprintf(stdout, "SKIP: CFReDS dfr-05-braid-ntfs.dd not found at %ls\n", imagePath.c_str());
        return 0;
    }

    fprintf(stdout, "Image: %ls\n", imagePath.c_str());

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");
    if (!reader.IsOpen()) return 1;

    std::vector<MbrEntry> entries;
    CHECK_MSG(ParseMbr(reader, entries), "MBR parse failed");

    uint64_t ntfsStart = 0, ntfsSectors = 0;
    for (auto& e : entries) {
        if (e.type == 0x07 && ntfsStart == 0) {
            ntfsStart = e.startSector;
            ntfsSectors = e.totalSectors;
        }
    }
    if (ntfsStart == 0 && !entries.empty()) {
        ntfsStart = entries[0].startSector;
        ntfsSectors = entries[0].totalSectors;
    }
    CHECK_MSG(ntfsStart > 0 && ntfsSectors > 0, "NTFS partition not found in MBR");

    Partition part;
    part.startSector  = ntfsStart;
    part.totalSectors = ntfsSectors;
    part.sizeBytes    = ntfsSectors * reader.GetSectorSize();
    part.fsType       = L"NTFS";

    NtfsParser parser;
    CHECK_MSG(parser.Open(reader, part), "NtfsParser::Open failed");
    CHECK_MSG(parser.ScanMetadata(), "NtfsParser::ScanMetadata failed");

    auto files = parser.GetAllFiles();
    int deleted = 0;
    for (auto& f : files) {
        if (f.status == FileStatus::Deleted && !f.isDirectory) ++deleted;
    }

    fprintf(stdout, "Total nodes: %zu, deleted files: %d\n", files.size(), deleted);
    CHECK_MSG(deleted >= 1, "Expected at least 1 deleted file in NTFS partition (braid)");

    reader.Close();
    return g_failures ? 1 : 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: CFReDS DFR-05 braid NTFS test requires volume I/O platform.\n");
    return 0;
}

#endif

