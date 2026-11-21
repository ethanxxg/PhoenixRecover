// DFTT #12 EXT2 carve — skip when image missing; RawScan golden when present.
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#include "dftt_test_md5.h"
#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "recover/FileNode.h"
#include "scan/RawScan.h"

#include <cstdio>
#include <filesystem>
#include <vector>

static int g_failures = 0;

#define CHECK_MSG(expr, msg) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s - %s (%s:%d)\n", #expr, (msg), __FILE__, __LINE__); \
            ++g_failures; \
        } \
    } while (0)

static std::wstring ImagePath()
{
#ifdef PHOENIX_SOURCE_DIR
    namespace fs = std::filesystem;
    const fs::path candidates[] = {
        fs::path(PHOENIX_SOURCE_DIR) / "testdata" / "dftt" / "12-carve-ext2" / "12-carve-ext2.dd",
        fs::path(PHOENIX_SOURCE_DIR) / "testdata" / "dftt" / "12-carve-ext2.dd",
    };
    for (const auto& p : candidates) {
        if (fs::exists(p))
            return p.wstring();
    }
#endif
    return L"";
}

int main()
{
    const std::wstring imagePath = ImagePath();
    if (imagePath.empty()) {
        fprintf(stdout,
                "SKIP: DFTT 12-carve-ext2.dd not found (extract valid zip to testdata/dftt/12-carve-ext2/)\n");
        return 0;
    }

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage");
    if (!reader.IsOpen())
        return 1;

    Partition part{};
    part.startSector  = 0;
    part.totalSectors = reader.GetTotalSectors();
    part.sizeBytes    = part.totalSectors * reader.GetSectorSize();
    part.fsType       = L"ext2";

    RawScan raw;
    ScanResult result = raw.Run(reader, part);
    CHECK_MSG(result.errorMsg.empty(), "scan error");
    CHECK_MSG(!result.cancelled, "scan cancelled");
    CHECK_MSG(result.foundCount >= 1u, "expected carved candidates on EXT2 image");

    reader.Close();
    if (g_failures != 0) {
        fprintf(stderr, "DFTT #12 EXT2 carve smoke: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: DFTT #12 EXT2 RawScan smoke found=%u nodes\n", result.foundCount);
    return 0;
}

#else

int main()
{
    fprintf(stdout, "SKIP: DFTT #12 test is Windows-only\n");
    return 0;
}

#endif
