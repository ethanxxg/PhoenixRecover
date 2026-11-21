// NIST FTK 3.3.0 full compare for dfr-test-images (all benchmarked cases).
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "fs/fat/FatParser.h"
#include "fs/ntfs/NtfsParser.h"
#include "fs/exfat/ExfatParser.h"
#include "recover/FileNode.h"
#include "recover/RecoverEngine.h"
#include "dfr_nist_benchmark.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;

static int g_failures = 0;

#define CHECK_MSG(expr, msg) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s - %s\n", #expr, (msg)); \
            ++g_failures; \
        } \
    } while (0)

struct MbrEntry { uint64_t startSector; uint64_t totalSectors; uint8_t type; };

static bool DetectVbrFs(SectorReader& reader, uint64_t startSector, std::wstring& outFs,
                        uint64_t& outSectors)
{
    std::vector<uint8_t> sec(512);
    if (!reader.ReadSectors(startSector, sec.data(), 1)) return false;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return false;
    if (memcmp(sec.data() + 3, "NTFS    ", 8) == 0) {
        outFs = L"NTFS";
        outSectors = *reinterpret_cast<const uint64_t*>(sec.data() + 40);
        return outSectors > 0;
    }
    if (memcmp(sec.data() + 3, "EXFAT   ", 8) == 0) {
        outFs = L"exFAT";
        outSectors = *reinterpret_cast<const uint64_t*>(sec.data() + 72);
        return outSectors > 0;
    }
    if (memcmp(sec.data() + 54, "FAT16   ", 8) == 0 || memcmp(sec.data() + 54, "FAT12   ", 8) == 0) {
        outFs = L"FAT16";
        const uint16_t tot16 = *reinterpret_cast<const uint16_t*>(sec.data() + 19);
        const uint32_t tot32 = *reinterpret_cast<const uint32_t*>(sec.data() + 32);
        outSectors = tot16 ? tot16 : tot32;
        return outSectors > 0;
    }
    if (memcmp(sec.data() + 82, "FAT32   ", 8) == 0) {
        outFs = L"FAT32";
        const uint16_t tot16 = *reinterpret_cast<const uint16_t*>(sec.data() + 19);
        const uint32_t tot32 = *reinterpret_cast<const uint32_t*>(sec.data() + 32);
        outSectors = tot16 ? tot16 : tot32;
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
        if (fs == L"exFAT" || fs.find(L"FAT") != std::wstring::npos || fs == L"NTFS")
            entries.push_back({start, length, 0});
    }
    return !entries.empty();
}

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

static bool Recoverable(const FileNode& f)
{
    if (f.isDirectory || f.size == 0) return false;
    if (!f.residentData.empty() || !f.clusters.empty()) return true;
    return f.rawSize > 0;
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
    std::wstring dir = std::wstring(tmp) + L"PhoenixRecover_nistcmp_" +
                       std::to_wstring(GetTickCount64());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static int RecoverAll(SectorReader& reader, const Partition& part,
                      const std::wstring& outDir, size_t& deleted, int& fullOk)
{
    std::vector<FileNode> files;
    if (part.fsType.find(L"NTFS") != std::wstring::npos) {
        NtfsParser parser;
        if (!parser.Open(reader, part) || !parser.ScanMetadata()) return -1;
        files = parser.GetAllFiles();
    } else if (part.fsType.find(L"exFAT") != std::wstring::npos) {
        ExfatParser parser;
        if (!parser.Open(reader, part) || !parser.ScanMetadata()) return -1;
        files = parser.GetAllFiles();
    } else {
        FatParser parser;
        if (!parser.Open(reader, part) || !parser.ScanMetadata()) return -1;
        files = parser.GetAllFiles();
    }

    deleted = 0;
    for (const auto& f : files) {
        if (f.status == FileStatus::Deleted && !f.isDirectory) ++deleted;
    }

    RecoverEngine engine;
    fullOk = 0;
    for (const auto& node : files) {
        if (node.status != FileStatus::Deleted || !Recoverable(node)) continue;
        FileRecoverResult rr = engine.RecoverFile(node, outDir, false, reader, part);
        if (rr.status == RecoverStatus::Success &&
            FileSizeOnDisk(rr.targetPath) == node.size)
            ++fullOk;
    }
    return static_cast<int>(deleted);
}

static void RunImage(const fs::path& imagePath, const DfrNistExpect& exp)
{
    const std::wstring datasetId = imagePath.stem().wstring();
    if (!fs::exists(imagePath)) {
        fprintf(stdout, "SKIP missing %ls\n", imagePath.c_str());
        return;
    }

    SectorReader reader;
    if (!reader.OpenImage(imagePath.wstring())) {
        fprintf(stderr, "FAIL open %ls\n", imagePath.c_str());
        ++g_failures;
        return;
    }

    const std::wstring kind = [&] {
        std::wstring n = imagePath.filename().wstring();
        for (auto& c : n) c = static_cast<wchar_t>(towlower(c));
        if (n.find(L"-ntfs") != std::wstring::npos) return std::wstring(L"NTFS");
        if (n.find(L"-xfat") != std::wstring::npos) return std::wstring(L"exFAT");
        return std::wstring(L"FAT");
    }();

    std::wstring outDir = TempDir();
    size_t sumDel = 0;
    int sumFull = 0;
    bool ok = false;

    if (kind == L"NTFS") {
        std::vector<MbrEntry> entries;
        if (!ParseMbr(reader, entries)) {
            fprintf(stderr, "FAIL mbr %ls\n", datasetId.c_str());
            ++g_failures;
            reader.Close();
            return;
        }
        uint64_t start = 0, sectors = 0;
        for (const auto& e : entries) {
            if (e.type == 0x07) { start = e.startSector; sectors = e.totalSectors; break; }
        }
        if (start == 0 && !entries.empty()) {
            start = entries[0].startSector;
            sectors = entries[0].totalSectors;
        }
        Partition part;
        part.startSector = start;
        part.totalSectors = sectors;
        part.sizeBytes = sectors * reader.GetSectorSize();
        part.fsType = L"NTFS";
        size_t del = 0;
        int full = 0;
        ok = RecoverAll(reader, part, outDir, del, full) >= 0;
        sumDel = del;
        sumFull = full;
    } else if (kind == L"exFAT") {
        std::vector<MbrEntry> entries;
        if (!ParseGptDataPartitions(reader, entries) && !ParseMbr(reader, entries)) {
            fprintf(stderr, "FAIL partition table %ls\n", datasetId.c_str());
            ++g_failures;
            reader.Close();
            return;
        }
        for (const auto& e : entries) {
            std::wstring vbrFs;
            uint64_t vbrSec = 0;
            if (!DetectVbrFs(reader, e.startSector, vbrFs, vbrSec) || vbrFs != L"exFAT")
                continue;
            Partition part;
            part.startSector = e.startSector;
            part.totalSectors = (vbrSec > 0 && vbrSec <= e.totalSectors) ? vbrSec : e.totalSectors;
            part.sizeBytes = part.totalSectors * reader.GetSectorSize();
            part.fsType = L"exFAT";
            size_t del = 0;
            int full = 0;
            if (RecoverAll(reader, part, outDir, del, full) >= 0) {
                ok = true;
                sumDel += del;
                sumFull += full;
            }
        }
    } else {
        std::vector<MbrEntry> entries;
        if (!ParseMbr(reader, entries)) {
            fprintf(stderr, "FAIL mbr %ls\n", datasetId.c_str());
            ++g_failures;
            reader.Close();
            return;
        }
        for (const auto& e : entries) {
            if (e.type == 0x05 || e.type == 0x0F || e.type == 0x85) continue;
            Partition part;
            part.startSector = e.startSector;
            part.totalSectors = e.totalSectors;
            part.sizeBytes = e.totalSectors * reader.GetSectorSize();
            part.fsType = L"FAT";
            size_t del = 0;
            int full = 0;
            if (RecoverAll(reader, part, outDir, del, full) >= 0) {
                ok = true;
                sumDel += del;
                sumFull += full;
            }
        }
    }

    std::error_code ec;
    fs::remove_all(outDir, ec);
    reader.Close();

    if (!ok) {
        fprintf(stderr, "FAIL scan %ls\n", datasetId.c_str());
        ++g_failures;
        return;
    }

    fprintf(stdout, "%-28ls del=%zu [%u..%u] full=%d/%u\n",
            datasetId.c_str(), sumDel, exp.deletedMin, exp.deletedMax, sumFull, exp.fullMin);
    CHECK_MSG(DfrNistDeletedInRange(sumDel, exp), "deleted outside NIST band");
    CHECK_MSG(static_cast<uint32_t>(sumFull) >= exp.fullMin, "full recover below NIST");
}

static void CollectBenchmarkImages(const fs::path& root, std::vector<fs::path>& out)
{
    if (!fs::exists(root)) return;
    for (const auto& ent : fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied)) {
        if (!ent.is_regular_file() || ent.path().extension() != L".dd") continue;
        const std::wstring id = ent.path().stem().wstring();
        if (!DfrNistLookup(id).hasExpect) continue;
        out.push_back(ent.path());
    }
    std::sort(out.begin(), out.end());
}

int main()
{
#ifdef PHOENIX_SOURCE_DIR
    const fs::path root = fs::path(PHOENIX_SOURCE_DIR) / "testdata" / "dfr-test-images";
#else
    fprintf(stderr, "PHOENIX_SOURCE_DIR required\n");
    return 1;
#endif

    std::vector<fs::path> images;
    CollectBenchmarkImages(root, images);
    if (images.empty()) {
        fprintf(stdout, "SKIP: no benchmarked dfr-test-images .dd files\n");
        return 0;
    }

    fprintf(stdout, "NIST compare: %zu dfr-test-images with benchmark entries\n", images.size());
    for (const auto& p : images) {
        const DfrNistExpect exp = DfrNistLookup(p.stem().wstring());
        RunImage(p, exp);
    }

    if (g_failures)
        fprintf(stderr, "NIST compare: %d failure(s)\n", g_failures);
    else
        fprintf(stdout, "NIST compare: ALL OK (%zu images)\n", images.size());
    return g_failures ? 1 : 0;
}

#else
int main()
{
    fprintf(stdout, "SKIP: volume I/O disabled\n");
    return 0;
}
#endif
