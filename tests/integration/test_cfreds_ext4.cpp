// CFReDS DFR-01-EXT — ext4 deleted-file discovery via FileSystemModuleService.

#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#include "disk/Partition.h"
#include "disk/PartitionRecoveryService.h"
#include "disk/SectorReader.h"
#include "fs/FileSystem.h"
#include "recover/FileNode.h"
#include "scan/FileSystemModuleService.h"
#include "scan/ScanEngine.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

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

static std::wstring ResolveImage()
{
#ifdef PHOENIX_SOURCE_DIR
    namespace fs = std::filesystem;
    const fs::path base(PHOENIX_SOURCE_DIR);
    const fs::path candidates[] = {
        base / "testdata" / "dfr-test-images" / "dfr-01" / "dfr-01-ext.dd",
        base / "testdata" / "cfreds" / "dfr-01-ext.dd",
        base / "testdata" / "DFR" / "dfr-01-ext.dd",
    };
    for (const auto& p : candidates) {
        if (fs::exists(p)) {
            return p.wstring();
        }
    }
#endif
    return L"";
}

int main()
{
    const std::wstring imagePath = ResolveImage();
    if (imagePath.empty()) {
        fprintf(stdout, "SKIP: dfr-01-ext.dd not found (download CFReDS DFR-01-EXT)\n");
        return 0;
    }

    fprintf(stdout, "Image: %ls\n", imagePath.c_str());

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage");
    if (!reader.IsOpen()) {
        return 1;
    }

    PartitionRecoveryService::Options opt;
    auto candidates = PartitionRecoveryService::ProbeVolume(reader, opt, nullptr, nullptr);
    CHECK_MSG(!candidates.empty(), "no partition candidates");

    const PartitionRecoveryService::Candidate* extCand = nullptr;
    for (const auto& c : candidates) {
        if (c.fsType == L"ext4" && c.totalSectors > 0) {
            extCand = &c;
            break;
        }
    }
    if (!extCand) {
        for (const auto& c : candidates) {
            if (c.totalSectors > 0 && c.startSector > 0) {
                extCand = &c;
                break;
            }
        }
    }
    CHECK_MSG(extCand != nullptr, "no usable ext4 partition");

    Partition part = PartitionRecoveryService::ToPartition(*extCand, -1);
    if (part.fsType.empty()) {
        part.fsType = L"ext4";
    }

    ScanResult scanMeta;
    auto fs = FileSystemModuleService::CreateParserForPartition(reader, part, scanMeta);
    CHECK_MSG(fs != nullptr, "CreateParserForPartition");
    CHECK_MSG(fs->Open(reader, part), "FileSystem::Open");
    CHECK_MSG(fs->ScanMetadata(), "ScanMetadata");

    int deletedFiles = 0;
    for (const auto& f : fs->GetAllFiles()) {
        if (f.status == FileStatus::Deleted && !f.isDirectory) {
            ++deletedFiles;
        }
    }

    const unsigned totalNodes = static_cast<unsigned>(fs->GetAllFiles().size());
    fprintf(stdout, "ext4 nodes=%u deleted=%d records=%u\n",
            totalNodes, deletedFiles, fs->GetTotalRecordCount());
    CHECK_MSG(totalNodes >= 1, "expected at least one file node from ext4 scan");
    CHECK_MSG(deletedFiles >= 1, "expected at least one Deleted file node (htree + dentry scan)");

    reader.Close();

    if (g_failures == 0) {
        fprintf(stdout, "OK: CFReDS EXT4 DFR-01 smoke (%u nodes, %d deleted)\n",
                totalNodes, deletedFiles);
    }
    return g_failures ? 1 : 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: CFReDS ext4 test requires volume I/O platform\n");
    return 0;
}

#endif
