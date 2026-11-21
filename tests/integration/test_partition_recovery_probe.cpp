// PartitionRecoveryService — probe disk images (MBR/GPT + lost-partition modes).

#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#include "disk/PartitionRecoveryService.h"
#include "disk/SectorReader.h"

#include <cstdio>
#include <filesystem>
#include <string>

static int g_failures = 0;

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "FAIL: %s  (%s:%d)\n", #expr, __FILE__, __LINE__); \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

static std::wstring ImagePath()
{
#ifdef PHOENIX_SOURCE_DIR
    namespace fs = std::filesystem;
    const fs::path base(PHOENIX_SOURCE_DIR);
#if PHOENIX_LINUX_EXT_FS_ONLY
    // For remote validation loops we copy one ext image at a time.
    // Prefer whatever *-ext.dd exists under testdata/dfr-test-images.
    const fs::path preferRoot = base / "testdata" / "dfr-test-images";
    if (fs::exists(preferRoot)) {
        for (const auto& ent : fs::recursive_directory_iterator(
                 preferRoot, fs::directory_options::skip_permission_denied)) {
            if (!ent.is_regular_file())
                continue;
            const auto name = ent.path().filename().wstring();
            if (ent.path().extension() == L".dd" &&
                name.find(L"-ext.dd") != std::wstring::npos) {
                return ent.path().wstring();
            }
        }
    }

    const fs::path candidates[] = {
        base / "testdata" / "cfreds" / "dfr-01-ext.dd",
        base / "testdata" / "DFR" / "dfr-01-ext.dd",
    };
#else
    const fs::path candidates[] = {
        base / "testdata" / "cfreds" / "dfr-01-ntfs.dd",
        base / "testdata" / "DFR" / "dfr-01-ntfs.dd",
    };
#endif
    for (const auto& p : candidates) {
        if (fs::exists(p))
            return p.wstring();
    }
    return L"";
#else
    return L"";
#endif
}

int main()
{
    const std::wstring imagePath = ImagePath();
    if (imagePath.empty() || !std::filesystem::exists(imagePath)) {
        fprintf(stdout, "SKIP: probe image not found (ext or ntfs)\n");
        return 0;
    }

    SectorReader reader;
    CHECK(reader.OpenImage(imagePath));
    if (!reader.IsOpen()) {
        return 1;
    }

    PartitionRecoveryService::Options opt;
    auto all = PartitionRecoveryService::ProbeVolume(reader, opt, nullptr, nullptr);
    CHECK(!all.empty());

    bool hasTable = false;
    for (const auto& c : all) {
        if (c.source.find(L"MBR") != std::wstring::npos ||
            c.source.find(L"GPT") != std::wstring::npos) {
            hasTable = true;
        }
    }
    CHECK(hasTable);

    auto tableOnly = PartitionRecoveryService::ListTablePartitions(reader, opt, nullptr, nullptr);
    CHECK(!tableOnly.empty());

    opt.signatureStrideSectors = 4096;
    opt.scanEndSector = (std::min)(reader.GetTotalSectors(), uint64_t{8192});
    auto lost = PartitionRecoveryService::RecoverLostPartitions(reader, opt, nullptr, nullptr);
    (void)lost;

    reader.Close();

    if (g_failures == 0) {
        fprintf(stdout, "OK: PartitionRecoveryService probe (%zu candidates)\n", all.size());
    }
    return g_failures ? 1 : 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: PartitionRecovery probe requires volume I/O platform\n");
    return 0;
}

#endif
