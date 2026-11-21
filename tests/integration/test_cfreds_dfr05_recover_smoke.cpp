// CFReDS DFR-05 — recover deleted files (byte size check, no golden MD5).
// Complements metadata-only CFReDS_DFR05_* tests (RCV-P0-06).
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "fs/fat/FatParser.h"
#include "fs/ntfs/NtfsParser.h"
#include "recover/FileNode.h"
#include "recover/RecoverEngine.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
            ++g_failures; \
        } \
    } while (0)

#define CHECK_MSG(expr, msg) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s - %s (%s:%d)\n", #expr, (msg), __FILE__, __LINE__); \
            ++g_failures; \
        } \
    } while (0)

static std::wstring RepoRoot()
{
#ifdef PHOENIX_SOURCE_DIR
    return std::wstring(PHOENIX_SOURCE_DIR);
#else
    return L"";
#endif
}

static std::wstring MakeTempDir(const wchar_t* tag)
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"PhoenixRecover_" + tag + L"_" +
                       std::to_wstring(GetTickCount64());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static void RemoveTree(const std::wstring& root)
{
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(root), ec);
}

static bool Recoverable(const FileNode& f)
{
    if (f.isDirectory || f.size == 0)
        return false;
    if (!f.residentData.empty())
        return true;
    if (!f.clusters.empty())
        return true;
    return f.rawSize > 0;
}

static uint64_t FileSizeOnDisk(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return 0;
    uint64_t hi = fad.nFileSizeHigh;
    return (hi << 32) | fad.nFileSizeLow;
}

static int RecoverDeletedOnPartition(SectorReader& reader, const Partition& part,
                                      const wchar_t* fsLabel, const std::wstring& outDir,
                                      int maxFiles)
{
    std::vector<FileNode> files;
    std::wstring fsType = part.fsType;

    if (fsType.find(L"NTFS") != std::wstring::npos) {
        NtfsParser parser;
        if (!parser.Open(reader, part) || !parser.ScanMetadata())
            return 0;
        files = parser.GetAllFiles();
    } else {
        FatParser parser;
        if (!parser.Open(reader, part) || !parser.ScanMetadata())
            return 0;
        files = parser.GetAllFiles();
        fsType = parser.GetFsTypeName();
    }

    RecoverEngine engine;
    int attempted = 0;
    int ok = 0;

    for (const FileNode& node : files) {
        if (node.status != FileStatus::Deleted || !Recoverable(node))
            continue;
        if (maxFiles > 0 && attempted >= maxFiles)
            break;

        ++attempted;
        FileRecoverResult rr = engine.RecoverFile(node, outDir, false, reader, part);
        if (rr.status != RecoverStatus::Success) {
            fprintf(stderr, "  [%ls/%ls] recover failed: %ls\n", fsLabel, fsType.c_str(),
                    node.name.c_str());
            continue;
        }

        uint64_t written = FileSizeOnDisk(rr.targetPath);
        if (written != node.size) {
            fprintf(stderr,
                    "  [%ls] size mismatch %ls: expected %llu wrote %llu\n",
                    fsLabel, node.name.c_str(),
                    static_cast<unsigned long long>(node.size),
                    static_cast<unsigned long long>(written));
            continue;
        }

        fprintf(stdout, "  [%ls] OK %ls (%llu bytes)\n", fsLabel, node.name.c_str(),
                static_cast<unsigned long long>(node.size));
        ++ok;
    }

    fprintf(stdout, "  [%ls] recover smoke: %d/%d deleted files OK\n", fsLabel, ok, attempted);
    return ok;
}

struct MbrEntry {
    uint64_t startSector;
    uint64_t totalSectors;
    uint8_t  type;
};

static bool ParseMbr(SectorReader& reader, std::vector<MbrEntry>& entries)
{
    std::vector<uint8_t> mbr(512);
    if (!reader.ReadSectors(0, mbr.data(), 1))
        return false;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return false;

    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = mbr.data() + 446 + i * 16;
        uint8_t type = e[4];
        if (type == 0)
            continue;
        uint32_t start = *reinterpret_cast<const uint32_t*>(e + 8);
        uint32_t count = *reinterpret_cast<const uint32_t*>(e + 12);
        if (start > 0 && count > 0)
            entries.push_back({start, count, type});
    }
    return !entries.empty();
}

static int RunFatImage(const std::wstring& imagePath)
{
    if (!std::filesystem::exists(imagePath)) {
        fprintf(stdout, "SKIP: %ls not found\n", imagePath.c_str());
        return 0;
    }

    fprintf(stdout, "\n=== DFR-05 FAT recover smoke: %ls ===\n", imagePath.c_str());

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");
    if (!reader.IsOpen())
        return 1;

    std::vector<MbrEntry> entries;
    CHECK_MSG(ParseMbr(reader, entries), "MBR parse failed");

    std::wstring outDir = MakeTempDir(L"dfr05fat");
    int totalOk = 0;
    int parts = 0;

    for (size_t i = 0; i < entries.size(); ++i) {
        auto& e = entries[i];
        if (e.type == 0x05 || e.type == 0x0F || e.type == 0x85)
            continue;

        Partition part;
        part.startSector  = e.startSector;
        part.totalSectors = e.totalSectors;
        part.sizeBytes    = e.totalSectors * reader.GetSectorSize();
        part.fsType       = L"FAT";

        std::wstring label = L"FAT_P" + std::to_wstring(i);
        int ok = RecoverDeletedOnPartition(reader, part, label.c_str(), outDir, 16);
        if (ok > 0)
            ++parts;
        totalOk += ok;
    }

    RemoveTree(outDir);
    reader.Close();

    CHECK_MSG(totalOk >= 1, "expected at least one successful FAT recovery on DFR-05");
    fprintf(stdout, "DFR-05 FAT: %d files recovered across %d partition(s)\n", totalOk, parts);
    return g_failures ? 1 : 0;
}

static int RunNtfsImage(const std::wstring& imagePath)
{
    if (!std::filesystem::exists(imagePath)) {
        fprintf(stdout, "SKIP: %ls not found\n", imagePath.c_str());
        return 0;
    }

    fprintf(stdout, "\n=== DFR-05 NTFS recover smoke: %ls ===\n", imagePath.c_str());

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");
    if (!reader.IsOpen())
        return 1;

    std::vector<MbrEntry> entries;
    CHECK_MSG(ParseMbr(reader, entries), "MBR parse failed");

    uint64_t start = 0;
    uint64_t sectors = 0;
    for (auto& e : entries) {
        if (e.type == 0x07) {
            start = e.startSector;
            sectors = e.totalSectors;
            break;
        }
    }
    if (start == 0 && !entries.empty()) {
        start = entries[0].startSector;
        sectors = entries[0].totalSectors;
    }
    CHECK_MSG(start > 0 && sectors > 0, "NTFS partition not found");

    Partition part;
    part.startSector  = start;
    part.totalSectors = sectors;
    part.sizeBytes    = sectors * reader.GetSectorSize();
    part.fsType       = L"NTFS";

    std::wstring outDir = MakeTempDir(L"dfr05ntfs");
    int ok = RecoverDeletedOnPartition(reader, part, L"NTFS", outDir, 24);

    RemoveTree(outDir);
    reader.Close();

    CHECK_MSG(ok >= 1, "expected at least one successful NTFS recovery on DFR-05");
    fprintf(stdout, "DFR-05 NTFS: %d files recovered\n", ok);
    return g_failures ? 1 : 0;
}

int main()
{
    std::wstring root = RepoRoot();
    if (root.empty()) {
        fprintf(stdout, "SKIP: PHOENIX_SOURCE_DIR not set\n");
        return 0;
    }

    namespace fs = std::filesystem;
    int rc = 0;
    rc |= RunFatImage((fs::path(root) / "testdata" / "cfreds" / "dfr-05-fat.dd").wstring());
    rc |= RunNtfsImage((fs::path(root) / "testdata" / "cfreds" / "dfr-05-ntfs.dd").wstring());
    // Braid (interleaved fragmentation) — best-effort smoke:
    rc |= RunFatImage((fs::path(root) / "testdata" / "cfreds" / "dfr-05-braid-fat.dd").wstring());
    rc |= RunNtfsImage((fs::path(root) / "testdata" / "cfreds" / "dfr-05-braid-ntfs.dd").wstring());

    if (rc == 0 && g_failures == 0)
        fprintf(stdout, "\nOK: CFReDS DFR-05 recover smoke\n");
    else if (g_failures > 0)
        fprintf(stderr, "\nCFReDS DFR-05 recover smoke: %d failure(s)\n", g_failures);

    return (g_failures > 0 || rc != 0) ? 1 : 0;
}

#else

int main()
{
    fprintf(stdout, "SKIP: DFR-05 recover smoke requires volume I/O platform\n");
    return 0;
}

#endif
