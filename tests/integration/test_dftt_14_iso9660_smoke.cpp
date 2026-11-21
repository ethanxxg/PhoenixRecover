// DFTT #14 ISO9660 samples — primary volume descriptor smoke (no full parser yet).
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#include "disk/SectorReader.h"

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

static std::wstring SourceDir()
{
#ifdef PHOENIX_SOURCE_DIR
    return PHOENIX_SOURCE_DIR;
#else
    return L".";
#endif
}

static bool HasIso9660Pvd(SectorReader& reader)
{
    // ISO9660 PVD at byte offset 16 * 2048 (independent of SectorReader stride).
    constexpr uint64_t kPvdOff = 16ull * 2048ull;
    uint8_t sec[2048]{};
    if (!reader.ReadBytes(kPvdOff, sec, sizeof(sec)))
        return false;
    return sec[0] == 1 && sec[1] == 'C' && sec[2] == 'D' && sec[3] == '0' &&
           sec[4] == '0' && sec[5] == '1';
}

static void TestIsoFile(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) {
        fprintf(stdout, "SKIP missing %s\n", path.filename().string().c_str());
        return;
    }

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(path.wstring()), "OpenImage");
    CHECK_MSG(reader.IsOpen(), "reader open");
    CHECK_MSG(HasIso9660Pvd(reader), "ISO9660 primary volume descriptor");
    reader.Close();
    fprintf(stdout, "  OK %s\n", path.filename().string().c_str());
}

int main()
{
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(SourceDir()) / "testdata" / "dftt" / "14-iso9660-1";
    if (!fs::exists(dir)) {
        fprintf(stdout, "SKIP: %s not found\n", dir.string().c_str());
        return 0;
    }

    TestIsoFile(dir / "iso-dirtree1.iso");
    TestIsoFile(dir / "iso-dirtree2.iso");
    TestIsoFile(dir / "iso-endian.iso");

    if (g_failures != 0) {
        fprintf(stderr, "DFTT #14 ISO9660 smoke: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: DFTT #14 ISO9660 PVD smoke\n");
    return 0;
}

#else

int main()
{
    fprintf(stdout, "SKIP: DFTT #14 ISO9660 smoke is Windows-only\n");
    return 0;
}

#endif
