// Recovery module facades — type routing smoke test (no disk I/O).

#include "common/Platform.h"
#include "scan/FileSystemModuleService.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "FAIL: %s  (%s:%d)\n", #expr, __FILE__, __LINE__); \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

int main()
{
    using FileSystemModuleService::ClassifyFsType;
    using FileSystemModuleService::FsModuleId;
    using FileSystemModuleService::SupportsFsType;

#if PHOENIX_EXT_FS_CODEPATH
    CHECK(!SupportsFsType(L"NTFS"));
    CHECK(!SupportsFsType(L"FAT32"));
    CHECK(!SupportsFsType(L"exFAT"));
#else
    CHECK(SupportsFsType(L"NTFS"));
    CHECK(ClassifyFsType(L"NTFS") == FsModuleId::Ntfs);
    CHECK(ClassifyFsType(L"FAT32") == FsModuleId::Fat);
    CHECK(ClassifyFsType(L"exFAT") == FsModuleId::Exfat);
#endif
    CHECK(ClassifyFsType(L"ext4") == FsModuleId::Ext4);
    CHECK(ClassifyFsType(L"EXT2") == FsModuleId::Ext4);
    CHECK(!SupportsFsType(L"APFS"));
    CHECK(ClassifyFsType(L"") == FsModuleId::Unknown);

    if (g_failures == 0) {
        fprintf(stdout, "OK: RecoveryModules FsType routing\n");
    }
    return g_failures ? 1 : 0;
}
