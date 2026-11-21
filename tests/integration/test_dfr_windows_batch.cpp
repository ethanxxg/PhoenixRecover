// Batch validation for NIST CFReDS DFR Windows filesystem images (FAT / NTFS / exFAT).
// Scans testdata/DFR and testdata/cfreds; writes CSV to path in PHOENIX_DFR_REPORT_CSV.
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

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;

static int g_failures = 0;

struct MbrEntry {
    uint64_t startSector = 0;
    uint64_t totalSectors = 0;
    uint8_t  type = 0;
};

struct Row {
    std::wstring datasetId;
    std::wstring fsKind;
    std::wstring imagePath;
    std::wstring result;
    double       durationSec = 0;
    size_t       totalFiles = 0;
    size_t       deletedFiles = 0;
    int          recoverAttempted = 0;
    int          recoverOk = 0;
    std::wstring notes;
    bool         nistChecked = false;
    uint32_t     nistExpDel = 0;
    uint32_t     nistExpFull = 0;
};

static bool DetectVbrFs(SectorReader& reader, uint64_t startSector, std::wstring& outFs,
                        uint64_t& outSectors)
{
    std::vector<uint8_t> sec(512);
    if (!reader.ReadSectors(startSector, sec.data(), 1))
        return false;
    if (sec[510] != 0x55 || sec[511] != 0xAA)
        return false;
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
    if (!reader.ReadSectors(1, hdr.data(), 1))
        return false;
    if (memcmp(hdr.data(), "EFI PART", 8) != 0)
        return false;

    const uint32_t entryLba = *reinterpret_cast<const uint32_t*>(hdr.data() + 72);
    const uint32_t entryCount = *reinterpret_cast<const uint32_t*>(hdr.data() + 80);
    const uint32_t entrySize = *reinterpret_cast<const uint32_t*>(hdr.data() + 84);
    if (entryLba == 0 || entryCount == 0 || entrySize < 128)
        return false;

    const uint32_t tableBytes = entryCount * entrySize;
    std::vector<uint8_t> table(tableBytes);
    if (!reader.ReadSectors(entryLba, table.data(),
                            (tableBytes + 511) / reader.GetSectorSize()))
        return false;

    for (uint32_t i = 0; i < entryCount; ++i) {
        const uint8_t* e = table.data() + i * entrySize;
        bool allZero = true;
        for (int b = 0; b < 16; ++b) {
            if (e[b] != 0) {
                allZero = false;
                break;
            }
        }
        if (allZero)
            continue;

        const uint64_t start = *reinterpret_cast<const uint64_t*>(e + 32);
        const uint64_t length = *reinterpret_cast<const uint64_t*>(e + 40);
        if (start == 0 || length == 0)
            continue;

        std::wstring fs;
        uint64_t vbrSectors = 0;
        if (!DetectVbrFs(reader, start, fs, vbrSectors))
            continue;
        if (fs == L"exFAT" || fs.find(L"FAT") != std::wstring::npos || fs == L"NTFS") {
            entries.push_back({start, length, 0});
        }
    }
    return !entries.empty();
}

static bool ParseMbr(SectorReader& reader, std::vector<MbrEntry>& entries)
{
    std::vector<uint8_t> mbr(512);
    if (!reader.ReadSectors(0, mbr.data(), 1))
        return false;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return false;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = mbr.data() + 446 + i * 16;
        const uint8_t type = e[4];
        if (type == 0)
            continue;
        const uint32_t start = *reinterpret_cast<const uint32_t*>(e + 8);
        const uint32_t count = *reinterpret_cast<const uint32_t*>(e + 12);
        if (start > 0 && count > 0)
            entries.push_back({start, count, type});
    }
    return !entries.empty();
}

static bool IsWindowsImageName(const std::wstring& name)
{
    std::wstring lower = name;
    for (auto& c : lower)
        c = static_cast<wchar_t>(towlower(c));
    if (lower.find(L"-ext.") != std::wstring::npos ||
        lower.find(L"-osx.") != std::wstring::npos)
        return false;
    return lower.find(L"-fat.") != std::wstring::npos ||
           lower.find(L"-ntfs.") != std::wstring::npos ||
           lower.find(L"-xfat.") != std::wstring::npos ||
           lower.find(L"braid-fat") != std::wstring::npos ||
           lower.find(L"braid-ntfs") != std::wstring::npos;
}

static std::wstring FsKindFromName(const std::wstring& name)
{
    std::wstring lower = name;
    for (auto& c : lower)
        c = static_cast<wchar_t>(towlower(c));
    if (lower.find(L"braid-ntfs") != std::wstring::npos || lower.find(L"-ntfs.") != std::wstring::npos)
        return L"NTFS";
    if (lower.find(L"braid-fat") != std::wstring::npos || lower.find(L"-fat.") != std::wstring::npos)
        return L"FAT";
    if (lower.find(L"-xfat.") != std::wstring::npos)
        return L"exFAT";
    return L"Unknown";
}

static std::wstring DatasetIdFromPath(const fs::path& p)
{
    return p.stem().wstring();
}

static void CollectImages(const fs::path& root, std::vector<fs::path>& out)
{
    if (!fs::exists(root))
        return;
    for (const auto& ent : fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied)) {
        if (!ent.is_regular_file())
            continue;
        const auto ext = ent.path().extension().wstring();
        if (ext != L".dd")
            continue;
        if (!IsWindowsImageName(ent.path().filename().wstring()))
            continue;
        out.push_back(ent.path());
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

static bool Recoverable(const FileNode& f)
{
    if (f.isDirectory || f.size == 0)
        return false;
    if (!f.residentData.empty() || !f.clusters.empty())
        return true;
    return f.rawSize > 0;
}

static uint64_t FileSizeOnDisk(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return 0;
    return (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

static std::wstring MakeTempDir(const wchar_t* tag)
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir =
        std::wstring(tmp) + L"PhoenixRecover_dfr_" + tag + L"_" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static void RemoveTree(const std::wstring& root)
{
    std::error_code ec;
    fs::remove_all(fs::path(root), ec);
}

static int RecoverDeleted(SectorReader& reader,
                          const Partition& part,
                          const std::wstring& fsLabel,
                          const std::wstring& outDir,
                          int maxFiles,
                          size_t& totalFiles,
                          size_t& deletedFiles,
                          int& recoverOk)
{
    std::vector<FileNode> files;
    std::wstring fsType = part.fsType;

    if (fsType.find(L"NTFS") != std::wstring::npos) {
        NtfsParser parser;
        if (!parser.Open(reader, part) || !parser.ScanMetadata())
            return -1;
        files = parser.GetAllFiles();
        fsType = L"NTFS";
    } else if (fsType.find(L"exFAT") != std::wstring::npos) {
        ExfatParser parser;
        if (!parser.Open(reader, part) || !parser.ScanMetadata())
            return -1;
        files = parser.GetAllFiles();
        fsType = L"exFAT";
    } else {
        FatParser parser;
        if (!parser.Open(reader, part) || !parser.ScanMetadata())
            return -1;
        files = parser.GetAllFiles();
        fsType = parser.GetFsTypeName();
    }

    totalFiles = files.size();
    deletedFiles = 0;
    for (const auto& f : files) {
        if (f.status == FileStatus::Deleted && !f.isDirectory)
            ++deletedFiles;
    }

    RecoverEngine engine;
    int attempted = 0;
    recoverOk = 0;

    for (const FileNode& node : files) {
        if (node.status != FileStatus::Deleted || !Recoverable(node))
            continue;
        if (maxFiles > 0 && attempted >= maxFiles)
            break;
        ++attempted;
        FileRecoverResult rr = engine.RecoverFile(node, outDir, false, reader, part);
        if (rr.status != RecoverStatus::Success)
            continue;
        if (FileSizeOnDisk(rr.targetPath) == node.size)
            ++recoverOk;
    }

    (void)fsLabel;
    (void)fsType;
    return attempted;
}

static Row ValidateImage(const fs::path& imagePath, int maxRecover)
{
    Row row;
    row.imagePath = imagePath.wstring();
    row.datasetId = DatasetIdFromPath(imagePath);
    row.fsKind = FsKindFromName(imagePath.filename().wstring());

    const auto t0 = std::chrono::steady_clock::now();

    if (!fs::exists(imagePath)) {
        row.result = L"SKIP";
        row.notes = L"file missing";
        return row;
    }

    SectorReader reader;
    if (!reader.OpenImage(row.imagePath)) {
        row.result = L"FAIL";
        row.notes = L"OpenImage failed";
        row.durationSec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return row;
    }

    if (reader.GetSectorSize() == 0 || reader.GetTotalSectors() == 0) {
        row.result = L"FAIL";
        row.notes = L"invalid image geometry";
        reader.Close();
        row.durationSec =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return row;
    }

    const std::wstring outDir = MakeTempDir(row.datasetId.c_str());
    const std::wstring kind = row.fsKind;

    if (kind == L"exFAT") {
        std::vector<MbrEntry> entries;
        if (!ParseGptDataPartitions(reader, entries)) {
            if (!ParseMbr(reader, entries)) {
                MbrEntry whole{0, reader.GetTotalSectors(), 0};
                entries.push_back(whole);
            }
        }

        int totalAttempted = 0;
        size_t sumDeleted = 0;
        int bestRecover = 0;
        bool anyScan = false;

        for (const auto& e : entries) {
            std::wstring vbrFs;
            uint64_t vbrSec = 0;
            if (!DetectVbrFs(reader, e.startSector, vbrFs, vbrSec))
                continue;
            if (vbrFs != L"exFAT")
                continue;

            Partition part;
            part.startSector = e.startSector;
            part.totalSectors = e.totalSectors;
            if (vbrSec > 0 && vbrSec <= e.totalSectors)
                part.totalSectors = vbrSec;
            part.sizeBytes = part.totalSectors * reader.GetSectorSize();
            part.fsType = L"exFAT";

            size_t tf = 0;
            size_t del = 0;
            int rok = 0;
            const int att =
                RecoverDeleted(reader, part, L"exFAT", outDir, maxRecover, tf, del, rok);
            if (att >= 0) {
                anyScan = true;
                row.totalFiles += tf;
                sumDeleted += del;
                totalAttempted += att;
                bestRecover += rok;
            }
        }

        row.deletedFiles = sumDeleted;
        row.recoverOk = bestRecover;
        row.recoverAttempted = totalAttempted;

        RemoveTree(outDir);
        reader.Close();

        if (!anyScan) {
            row.result = L"FAIL";
            row.notes = L"exFAT partition not found or ExfatParser scan failed";
        } else if (row.deletedFiles == 0) {
            row.result = L"WARN";
            row.notes = L"scan ok, no deleted files";
        } else if (row.recoverOk >= 1) {
            row.result = L"PASS";
            row.notes = L"scan+recover ok";
        } else {
            row.result = L"PARTIAL";
            row.notes = L"deleted found, recover failed";
        }
    } else if (kind == L"NTFS") {
        std::vector<MbrEntry> entries;
        if (!ParseMbr(reader, entries)) {
            row.result = L"FAIL";
            row.notes = L"MBR parse failed";
            RemoveTree(outDir);
            reader.Close();
            row.durationSec =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            return row;
        }

        uint64_t start = 0;
        uint64_t sectors = 0;
        for (const auto& e : entries) {
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

        Partition part;
        part.startSector = start;
        part.totalSectors = sectors;
        part.sizeBytes = sectors * reader.GetSectorSize();
        part.fsType = L"NTFS";

        int attempted =
            RecoverDeleted(reader, part, L"NTFS", outDir, maxRecover, row.totalFiles,
                           row.deletedFiles, row.recoverOk);
        RemoveTree(outDir);
        reader.Close();

        if (attempted < 0) {
            row.result = L"FAIL";
            row.notes = L"NtfsParser scan failed";
        } else if (row.deletedFiles == 0) {
            row.result = L"WARN";
            row.notes = L"scan ok, no deleted files";
        } else if (row.recoverOk >= 1) {
            row.result = L"PASS";
            row.notes = L"scan+recover ok";
        } else {
            row.result = L"PARTIAL";
            row.notes = L"deleted found, recover failed";
        }
    } else {
        std::vector<MbrEntry> entries;
        if (!ParseMbr(reader, entries)) {
            row.result = L"FAIL";
            row.notes = L"MBR parse failed";
            RemoveTree(outDir);
            reader.Close();
            row.durationSec =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            return row;
        }

        int totalAttempted = 0;
        size_t sumDeleted = 0;
        int bestRecover = 0;
        bool anyScan = false;

        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            if (e.type == 0x05 || e.type == 0x0F || e.type == 0x85)
                continue;

            Partition part;
            part.startSector = e.startSector;
            part.totalSectors = e.totalSectors;
            part.sizeBytes = e.totalSectors * reader.GetSectorSize();
            part.fsType = L"FAT";

            size_t tf = 0;
            size_t del = 0;
            int rok = 0;
            const int att = RecoverDeleted(reader, part, L"FAT", outDir, maxRecover, tf, del, rok);
            if (att >= 0) {
                anyScan = true;
                row.totalFiles += tf;
                sumDeleted += del;
                totalAttempted += att;
                bestRecover += rok;
            }
        }

        row.deletedFiles = sumDeleted;
        row.recoverOk = bestRecover;
        row.recoverAttempted = totalAttempted;

        RemoveTree(outDir);
        reader.Close();

        if (!anyScan) {
            row.result = L"FAIL";
            row.notes = L"FatParser scan failed on all partitions";
        } else if (row.deletedFiles == 0) {
            row.result = L"WARN";
            row.notes = L"scan ok, no deleted files";
        } else if (row.recoverOk >= 1) {
            row.result = L"PASS";
            row.notes = L"scan+recover ok";
        } else {
            row.result = L"PARTIAL";
            row.notes = L"deleted found, recover failed";
        }
    }

    row.durationSec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return row;
}

static std::wstring GetEnvW(const wchar_t* name)
{
    wchar_t buf[4096]{};
    const DWORD n = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf))
        return L"";
    return std::wstring(buf);
}

static int SourcePriority(const fs::path& p)
{
    const std::string s = p.string();
    if (s.find("\\DFR\\") != std::string::npos || s.find("/DFR/") != std::string::npos)
        return 0;
    if (s.find("\\cfreds\\") != std::string::npos || s.find("/cfreds/") != std::string::npos)
        return 1;
    if (s.find("\\dfr-test-images\\") != std::string::npos ||
        s.find("/dfr-test-images/") != std::string::npos)
        return 2;
    return 3;
}

static int ParseMaxImagesEnv()
{
    const std::wstring v = GetEnvW(L"PHOENIX_DFR_WINDOWS_MAX_IMAGES");
    if (v.empty())
        return 0;
    try {
        const int n = std::stoi(v);
        return n > 0 ? n : 0;
    } catch (...) {
        return 0;
    }
}

static int ParseMaxRecoverEnv()
{
    const std::wstring v = GetEnvW(L"PHOENIX_DFR_MAX_RECOVER");
    if (v.empty()) {
        // Quick CTest default; use PHOENIX_DFR_MAX_RECOVER=0 for unlimited.
        if (!GetEnvW(L"PHOENIX_DFR_NIST_STRICT").empty())
            return 0;
        return 16;
    }
    try {
        return std::stoi(v);
    } catch (...) {
        return 16;
    }
}

static bool ParseNistStrictEnv()
{
    const std::wstring v = GetEnvW(L"PHOENIX_DFR_NIST_STRICT");
    return v == L"1" || v == L"true" || v == L"TRUE";
}

static std::wstring ParseOnlySubdirEnv()
{
    return GetEnvW(L"PHOENIX_DFR_ONLY_SUBDIR");
}

static void ApplyNistValidation(Row& row, bool nistStrict)
{
    if (!nistStrict)
        return;

    const DfrNistExpect exp = DfrNistLookup(row.datasetId);
    if (!exp.hasExpect) {
        row.notes += L" (no NIST benchmark entry)";
        return;
    }

    row.nistChecked = true;
    row.nistExpDel  = exp.deletedMin;
    row.nistExpFull = exp.fullMin;

    if (row.result == L"FAIL" || row.result == L"SKIP")
        return;

    if (exp.deletedMin == 0 && exp.deletedMax == 0) {
        if (row.deletedFiles != 0) {
            row.result = L"NIST_FAIL";
            row.notes  = L"unexpected deleted files";
        } else {
            row.result = L"PASS";
            row.notes  = L"NIST ok (no deleted, list-only)";
        }
        return;
    }

    if (!DfrNistDeletedInRange(row.deletedFiles, exp)) {
        row.result = L"NIST_FAIL";
        row.notes  = L"deleted count outside NIST band";
        return;
    }
    if (static_cast<uint32_t>(row.recoverOk) < exp.fullMin) {
        row.result = L"NIST_FAIL";
        row.notes  = L"recover_ok < NIST Full";
        return;
    }
    row.result = L"PASS";
    row.notes  = L"NIST ok";
}

int main()
{
    std::wstring repo;
#ifdef PHOENIX_SOURCE_DIR
    repo = PHOENIX_SOURCE_DIR;
#else
    repo = GetEnvW(L"PHOENIX_SOURCE_DIR");
#endif
    if (repo.empty()) {
        fprintf(stderr, "FAIL: PHOENIX_SOURCE_DIR not set\n");
        return 1;
    }

    std::vector<fs::path> images;
    const fs::path dfrRoot = fs::path(repo) / "testdata" / "DFR";
    const fs::path cfredsRoot = fs::path(repo) / "testdata" / "cfreds";
    const fs::path dfrTestRoot = fs::path(repo) / "testdata" / "dfr-test-images";

    CollectImages(dfrRoot, images);
    CollectImages(cfredsRoot, images);
    CollectImages(dfrTestRoot, images);

    const std::wstring onlySubdir = ParseOnlySubdirEnv();
    if (!onlySubdir.empty()) {
        std::vector<fs::path> filtered;
        for (const auto& p : images) {
            const std::wstring sp = p.wstring();
            if (sp.find(onlySubdir) != std::wstring::npos)
                filtered.push_back(p);
        }
        images = std::move(filtered);
        fprintf(stdout, "Filter PHOENIX_DFR_ONLY_SUBDIR=%ls -> %zu image(s)\n",
                onlySubdir.c_str(), images.size());
    }

    // Prefer DFR > cfreds > dfr-test-images when same dataset id.
    std::vector<fs::path> unique;
    for (const auto& p : images) {
        const std::wstring id = DatasetIdFromPath(p);
        auto it = std::find_if(unique.begin(), unique.end(), [&](const fs::path& q) {
            return DatasetIdFromPath(q) == id;
        });
        if (it == unique.end()) {
            unique.push_back(p);
            continue;
        }
        if (SourcePriority(p) < SourcePriority(*it))
            *it = p;
    }
    images = std::move(unique);
    std::sort(images.begin(), images.end());

    const int maxImages = ParseMaxImagesEnv();
    const int maxRecover = ParseMaxRecoverEnv();
    const bool nistStrict = ParseNistStrictEnv();

    if (maxRecover == 0)
        fprintf(stdout, "Recover: unlimited (PHOENIX_DFR_MAX_RECOVER=0)\n");
    else
        fprintf(stdout, "Recover: up to %d deleted file(s) per partition\n", maxRecover);
    if (nistStrict)
        fprintf(stdout, "NIST strict validation enabled\n");
    if (maxImages > 0 && static_cast<int>(images.size()) > maxImages) {
        fprintf(stdout, "Limiting to %d of %zu image(s) (PHOENIX_DFR_WINDOWS_MAX_IMAGES)\n",
                maxImages, images.size());
        images.resize(static_cast<size_t>(maxImages));
    }

    if (images.empty()) {
        fprintf(stdout,
                "SKIP: no Windows DFR .dd images under testdata/DFR, cfreds, or dfr-test-images\n");
        return 0;
    }

    std::wstring csvPath = GetEnvW(L"PHOENIX_DFR_REPORT_CSV");
    if (csvPath.empty()) {
        wchar_t tmp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tmp);
        csvPath = std::wstring(tmp) + L"PhoenixRecover_dfr_windows_report.csv";
    }

    FILE* csv = nullptr;
    _wfopen_s(&csv, csvPath.c_str(), L"wt, ccs=UTF-8");
    if (!csv) {
        fprintf(stderr, "FAIL: cannot open CSV %ls\n", csvPath.c_str());
        return 1;
    }

    if (nistStrict) {
        fwprintf(csv,
                 L"dataset_id,fs_kind,result,duration_sec,total_files,deleted_files,recover_ok,"
                 L"nist_exp_del,nist_exp_full,nist_checked,image_path,notes\n");
    } else {
        fwprintf(csv,
                 L"dataset_id,fs_kind,result,duration_sec,total_files,deleted_files,recover_ok,"
                 L"image_path,notes\n");
    }

    int pass = 0;
    int partial = 0;
    int fail = 0;
    int warn = 0;
    int nistFail = 0;

    fprintf(stdout, "DFR Windows batch: %zu image(s)\n", images.size());

    for (const auto& p : images) {
        Row row = ValidateImage(p, maxRecover);
        ApplyNistValidation(row, nistStrict);

        if (nistStrict) {
            fwprintf(csv,
                     L"%ls,%ls,%ls,%.3f,%zu,%zu,%d,%u,%u,%s,%ls,%ls\n",
                     row.datasetId.c_str(),
                     row.fsKind.c_str(),
                     row.result.c_str(),
                     row.durationSec,
                     row.totalFiles,
                     row.deletedFiles,
                     row.recoverOk,
                     row.nistExpDel,
                     row.nistExpFull,
                     row.nistChecked ? L"yes" : L"no",
                     row.imagePath.c_str(),
                     row.notes.c_str());
        } else {
            fwprintf(csv,
                     L"%ls,%ls,%ls,%.3f,%zu,%zu,%d,%ls,%ls\n",
                     row.datasetId.c_str(),
                     row.fsKind.c_str(),
                     row.result.c_str(),
                     row.durationSec,
                     row.totalFiles,
                     row.deletedFiles,
                     row.recoverOk,
                     row.imagePath.c_str(),
                     row.notes.c_str());
        }

        if (row.nistChecked) {
            fprintf(stdout,
                    "  %-28ls %-6ls %-8ls %.2fs del=%zu/%u rec=%d/%u  %ls\n",
                    row.datasetId.c_str(),
                    row.fsKind.c_str(),
                    row.result.c_str(),
                    row.durationSec,
                    row.deletedFiles,
                    row.nistExpDel,
                    row.recoverOk,
                    row.nistExpFull,
                    row.notes.c_str());
        } else {
            fprintf(stdout,
                    "  %-28ls %-6ls %-8ls %.2fs del=%zu rec_ok=%d  %ls\n",
                    row.datasetId.c_str(),
                    row.fsKind.c_str(),
                    row.result.c_str(),
                    row.durationSec,
                    row.deletedFiles,
                    row.recoverOk,
                    row.notes.c_str());
        }

        if (row.result == L"PASS")
            ++pass;
        else if (row.result == L"PARTIAL")
            ++partial;
        else if (row.result == L"WARN")
            ++warn;
        else if (row.result == L"NIST_FAIL") {
            ++nistFail;
        } else if (row.result == L"FAIL") {
            ++fail;
        }
    }

    fclose(csv);
    fwprintf(stdout, L"\nCSV: %ls\n", csvPath.c_str());
    if (nistStrict) {
        fprintf(stdout,
                "Summary: PASS=%d NIST_FAIL=%d PARTIAL=%d WARN=%d FAIL=%d (total=%zu)\n",
                pass, nistFail, partial, warn, fail, images.size());
        if (nistFail > 0 || fail > 0)
            ++g_failures;
    } else {
        fprintf(stdout, "Summary: PASS=%d PARTIAL=%d WARN=%d FAIL=%d (total=%zu)\n", pass,
                partial, warn, fail, images.size());
        if (fail > 0)
            ++g_failures;
    }

    return g_failures > 0 ? 1 : 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: DFR Windows batch requires volume I/O platform\n");
    return 0;
}

#endif
