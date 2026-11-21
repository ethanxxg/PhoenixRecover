// Quick NIST FTK 3.3.0 spot-check for critical dfr-test-images exFAT cases.
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "fs/exfat/ExfatParser.h"
#include "recover/FileNode.h"
#include "recover/RecoverEngine.h"
#include "dfr_nist_benchmark.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK_MSG(expr, msg) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s - %s\n", #expr, (msg)); \
            ++g_failures; \
        } \
    } while (0)

struct MbrEntry { uint64_t startSector; uint64_t totalSectors; uint8_t type; };

static bool DetectExfat(SectorReader& reader, uint64_t start, Partition& out);

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

static bool DetectVbrFs(SectorReader& reader, uint64_t startSector, std::wstring& outFs,
                        uint64_t& outSectors)
{
    std::vector<uint8_t> sec(512);
    if (!reader.ReadSectors(startSector, sec.data(), 1)) return false;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return false;
    if (memcmp(sec.data() + 3, "EXFAT   ", 8) == 0) {
        outFs = L"exFAT";
        outSectors = *reinterpret_cast<const uint64_t*>(sec.data() + 72);
        return outSectors > 0;
    }
    return false;
}

static bool ParseGptDataPartitions(SectorReader& reader, std::vector<MbrEntry>& entries)
{
    std::vector<uint8_t> hdr(512);
    if (!reader.ReadSectors(1, hdr.data(), 1)) return false;
    if (memcmp(hdr.data(), "EFI PART", 8) != 0) return false;
    const uint32_t entryLba = *reinterpret_cast<const uint32_t*>(hdr.data() + 72);
    const uint32_t entryCount = *reinterpret_cast<const uint32_t*>(hdr.data() + 80);
    const uint32_t entrySize = *reinterpret_cast<const uint32_t*>(hdr.data() + 84);
    if (entryLba == 0 || entryCount == 0 || entrySize < 128) return false;
    const uint32_t tableBytes = entryCount * entrySize;
    std::vector<uint8_t> table(tableBytes);
    if (!reader.ReadSectors(entryLba, table.data(),
                            (tableBytes + 511) / reader.GetSectorSize()))
        return false;
    for (uint32_t i = 0; i < entryCount; ++i) {
        const uint8_t* e = table.data() + i * entrySize;
        bool allZero = true;
        for (int b = 0; b < 16; ++b) {
            if (e[b] != 0) { allZero = false; break; }
        }
        if (allZero) continue;
        const uint64_t start = *reinterpret_cast<const uint64_t*>(e + 32);
        const uint64_t length = *reinterpret_cast<const uint64_t*>(e + 40);
        if (start == 0 || length == 0) continue;
        std::wstring fs;
        uint64_t vbrSectors = 0;
        if (!DetectVbrFs(reader, start, fs, vbrSectors)) continue;
        if (fs == L"exFAT") entries.push_back({start, length, 0});
    }
    return !entries.empty();
}

static bool FindExfatPartition(SectorReader& reader, Partition& out)
{
    std::vector<MbrEntry> entries;
    if (ParseGptDataPartitions(reader, entries)) {
        for (const auto& e : entries) {
            if (DetectExfat(reader, e.startSector, out)) return true;
        }
    }
    if (ParseMbr(reader, entries)) {
        for (const auto& e : entries) {
            if (DetectExfat(reader, e.startSector, out)) return true;
        }
    }
    return false;
}

static bool DetectExfat(SectorReader& reader, uint64_t start, Partition& out)
{
    std::vector<uint8_t> sec(512);
    if (!reader.ReadSectors(start, sec.data(), 1)) return false;
    if (memcmp(sec.data() + 3, "EXFAT   ", 8) != 0) return false;
    const uint64_t volLen = *reinterpret_cast<const uint64_t*>(sec.data() + 72);
    out.startSector = start;
    out.totalSectors = volLen > 0 ? volLen : 0;
    out.sizeBytes = out.totalSectors * reader.GetSectorSize();
    out.fsType = L"exFAT";
    return out.totalSectors > 0;
}

static uint64_t FileSizeOnDisk(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

static std::wstring TempDir()
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"PhoenixRecover_nist_" +
                       std::to_wstring(GetTickCount64());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static void RunCase(const std::wstring& imagePath)
{
    const std::wstring datasetId = std::filesystem::path(imagePath).stem().wstring();
    const DfrNistExpect exp = DfrNistLookup(datasetId);
    if (!exp.hasExpect) {
        fprintf(stderr, "SKIP no benchmark %ls\n", datasetId.c_str());
        return;
    }

    SectorReader reader;
    if (!reader.OpenImage(imagePath)) {
        fprintf(stderr, "FAIL open %ls\n", imagePath.c_str());
        ++g_failures;
        return;
    }

    Partition part{};
    if (!FindExfatPartition(reader, part)) {
        fprintf(stderr, "FAIL no exFAT %ls\n", imagePath.c_str());
        ++g_failures;
        reader.Close();
        return;
    }

    ExfatParser parser;
    if (!parser.Open(reader, part) || !parser.ScanMetadata()) {
        fprintf(stderr, "FAIL scan %ls\n", imagePath.c_str());
        ++g_failures;
        reader.Close();
        return;
    }

    const auto files = parser.GetAllFiles();
    size_t del = 0;
    for (const auto& f : files) {
        if (f.status == FileStatus::Deleted && !f.isDirectory) ++del;
    }

    std::wstring outDir = TempDir();
    RecoverEngine engine;
    size_t full = 0;
    for (const auto& node : files) {
        if (node.status != FileStatus::Deleted || node.isDirectory || node.size == 0)
            continue;
        if (node.clusters.empty() && node.rawSize == 0) continue;
        FileRecoverResult rr = engine.RecoverFile(node, outDir, false, reader, part);
        if (rr.status == RecoverStatus::Success &&
            FileSizeOnDisk(rr.targetPath) == node.size)
            ++full;
    }

    std::error_code ec;
    std::filesystem::remove_all(outDir, ec);
    reader.Close();

    fprintf(stdout, "%ls: del=%zu [%u..%u] full=%zu/%u\n",
            datasetId.c_str(), del, exp.deletedMin, exp.deletedMax, full, exp.fullMin);
    CHECK_MSG(DfrNistDeletedInRange(del, exp), "deleted outside NIST band");
    CHECK_MSG(full >= exp.fullMin, "full recover below NIST minimum");
}

int main()
{
#ifdef PHOENIX_SOURCE_DIR
    const std::wstring root = std::wstring(PHOENIX_SOURCE_DIR) + L"\\testdata\\dfr-test-images";
#else
    fprintf(stderr, "PHOENIX_SOURCE_DIR required\n");
    return 1;
#endif

    RunCase(root + L"\\dfr-05-braid\\dfr-05-braid-xfat.dd");
    RunCase(root + L"\\dfr-05-nest\\dfr-05-nest-xfat.dd");
    RunCase(root + L"\\dfr-12\\dfr-12-xfat.dd");
    RunCase(root + L"\\dfr-05\\dfr-05-xfat.dd");
    RunCase(root + L"\\dfr-02\\dfr-02-xfat.dd");
    RunCase(root + L"\\dfr-03\\dfr-03-xfat.dd");

    if (g_failures)
        fprintf(stderr, "NIST spot-check: %d failure(s)\n", g_failures);
    else
        fprintf(stdout, "NIST spot-check: ALL OK\n");
    return g_failures ? 1 : 0;
}

#else
int main()
{
    fprintf(stdout, "SKIP: volume I/O disabled\n");
    return 0;
}
#endif
