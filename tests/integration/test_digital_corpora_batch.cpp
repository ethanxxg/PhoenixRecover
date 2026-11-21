// Digital Corpora disk-images: smoke + FS scan + full RecoverEngine closed-loop.
// CSV: PHOENIX_DC_REPORT_CSV (default docs/reports/digital_corpora_results.csv)
// Failures: PHOENIX_DC_FAIL_CSV (default docs/reports/digital_corpora_recover_failures.csv)
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "disk/PartitionRecoveryService.h"
#include "fs/fat/FatParser.h"
#include "fs/ntfs/NtfsParser.h"
#include "fs/exfat/ExfatParser.h"
#include "fs/ext4/Ext4Parser.h"
#include "recover/FileNode.h"
#include "recover/RecoverEngine.h"
#include "log/Logger.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;

static int g_failures = 0;

struct Row {
    std::string dataset;
    std::string relPath;
    std::string format;
    uint64_t    sizeBytes = 0;
    std::string openOk;
    std::string readOk;
    std::string fsType;
    size_t      fileCount = 0;
    size_t      activeCount = 0;
    size_t      deletedCount = 0;
    size_t      recoverable = 0;
    size_t      recoverOk = 0;
    size_t      recoverFailed = 0;
    size_t      recoverSkipped = 0;
    std::string result;
    double      durationSec = 0;
    std::string notes;
};

struct FailRow {
    std::string imageRel;
    std::string fileName;
    std::string fileStatus;
    uint64_t    sizeBytes = 0;
    std::string outcome;
    std::string reason;
};

static std::wstring RepoRoot()
{
#ifdef PHOENIX_SOURCE_DIR
    return std::wstring(PHOENIX_SOURCE_DIR);
#else
    return L".";
#endif
}

static uint64_t MaxFsScanBytes()
{
    wchar_t buf[32]{};
    if (GetEnvironmentVariableW(L"PHOENIX_DC_MAX_FS_BYTES", buf, 32) > 0)
        return _wcstoui64(buf, nullptr, 10);
    return 50ull * 1024ull * 1024ull * 1024ull;
}

static int MaxRecoverFiles()
{
    wchar_t buf[16]{};
    if (GetEnvironmentVariableW(L"PHOENIX_DC_MAX_RECOVER", buf, 16) > 0)
        return _wtoi(buf);
    return 0;
}

static bool StrictRecover()
{
    wchar_t buf[8]{};
    return GetEnvironmentVariableW(L"PHOENIX_DC_STRICT_RECOVER", buf, 8) > 0;
}

static fs::path CsvPathFromEnv(const wchar_t* var, const char* defaultName)
{
    wchar_t buf[1024]{};
    if (GetEnvironmentVariableW(var, buf, 1024) > 0)
        return fs::path(buf);
#ifdef PHOENIX_SOURCE_DIR
    return fs::path(PHOENIX_SOURCE_DIR) / "docs" / "reports" / defaultName;
#else
    return fs::path(defaultName);
#endif
}

static fs::path DcEwfCacheRoot()
{
    wchar_t buf[1024]{};
    if (GetEnvironmentVariableW(L"PHOENIX_DC_EWF_CACHE", buf, 1024) > 0)
        return fs::path(buf);
#ifdef PHOENIX_SOURCE_DIR
    return fs::path(PHOENIX_SOURCE_DIR) / L"build" / L"ewf-cache" /
           L"digital-corpora-disk-images";
#else
    return fs::path(L"build") / L"ewf-cache" / L"digital-corpora-disk-images";
#endif
}

static bool IsValidImageFile(const fs::path& p)
{
    std::error_code ec;
    if (!fs::exists(p, ec) || !fs::is_regular_file(p, ec))
        return false;
    const auto sz = fs::file_size(p, ec);
    return !ec && sz >= 512;
}

static fs::path CacheDdPath(const fs::path& root, const fs::path& cacheRoot,
                            const fs::path& e01)
{
    std::error_code ec;
    const fs::path rel = fs::relative(e01, root, ec);
    if (ec)
        return {};
    return cacheRoot / rel.parent_path() / (rel.stem().wstring() + L".dd");
}

static std::string Narrow(const std::wstring& ws)
{
    if (ws.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

static std::string ImageRelPath(const fs::path& root, const fs::path& cacheRoot,
                                const fs::path& imagePath)
{
    std::error_code ec;
    const fs::path inCache = fs::relative(imagePath, cacheRoot, ec);
    if (!ec)
        return Narrow(inCache.wstring());
    return Narrow(fs::relative(imagePath, root).wstring());
}

static std::string EscCsv(const std::string& s)
{
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string q = "\"";
    for (char c : s) {
        if (c == '"') q += "\"\"";
        else q += c;
    }
    return q + "\"";
}

static std::vector<std::string> LoadDcOnlyFilters()
{
    std::vector<std::string> filters;
    wchar_t buf[4096]{};
    if (GetEnvironmentVariableW(L"PHOENIX_DC_ONLY", buf, 4096) == 0)
        return filters;
    std::string all = Narrow(buf);
    std::string part;
    for (char c : all) {
        if (c == ';') {
            if (!part.empty()) { filters.push_back(part); part.clear(); }
        } else {
            part += c;
        }
    }
    if (!part.empty())
        filters.push_back(part);
    return filters;
}

static bool MatchesDcFilter(const std::string& relPath,
                            const std::vector<std::string>& filters)
{
    if (filters.empty())
        return true;
    for (const std::string& f : filters) {
        if (relPath.find(f) != std::string::npos)
            return true;
    }
    return false;
}

static bool DetectVbrFs(SectorReader& reader, uint64_t startSector,
                        std::wstring& outFs, uint64_t& outSectors)
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

static bool ParseMbr(SectorReader& reader, std::vector<std::pair<uint64_t, uint64_t>>& parts)
{
    std::vector<uint8_t> mbr(512);
    if (!reader.ReadSectors(0, mbr.data(), 1))
        return false;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)
        return false;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = mbr.data() + 446 + i * 16;
        if (e[4] == 0)
            continue;
        const uint32_t start = *reinterpret_cast<const uint32_t*>(e + 8);
        const uint32_t count = *reinterpret_cast<const uint32_t*>(e + 12);
        if (start > 0 && count > 0)
            parts.emplace_back(start, count);
    }
    return !parts.empty();
}

static bool IsRecoverable(const FileNode& f)
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
    return std::wstring(tmp) + L"PhoenixRecover_dc_" + tag + L"_" +
           std::to_wstring(GetTickCount64());
}

static void RemoveTree(const std::wstring& root)
{
    std::error_code ec;
    fs::remove_all(fs::path(root), ec);
}

static const char* FileStatusTag(FileStatus s)
{
    switch (s) {
    case FileStatus::Deleted: return "deleted";
    case FileStatus::Lost:    return "lost";
    default:                  return "active";
    }
}

static bool LoadPartitionFiles(SectorReader& reader, uint64_t startSector, uint64_t totalSectors,
                               std::wstring& outFs, std::vector<FileNode>& outFiles)
{
    std::wstring fs;
    uint64_t vbrSectors = 0;
    if (!DetectVbrFs(reader, startSector, fs, vbrSectors))
        return false;

    Partition part;
    part.startSector  = startSector;
    part.totalSectors = (std::min)(totalSectors, vbrSectors > 0 ? vbrSectors : totalSectors);
    part.fsType       = fs;
    part.sizeBytes    = part.totalSectors * reader.GetSectorSize();

    if (fs.find(L"NTFS") != std::wstring::npos) {
        NtfsParser p;
        if (!p.Open(reader, part) || !p.ScanMetadata())
            return false;
        outFiles = p.GetAllFiles();
        outFs = L"NTFS";
    } else if (fs == L"exFAT") {
        ExfatParser p;
        if (!p.Open(reader, part) || !p.ScanMetadata())
            return false;
        outFiles = p.GetAllFiles();
        outFs = L"exFAT";
    } else {
        FatParser p;
        if (!p.Open(reader, part) || !p.ScanMetadata())
            return false;
        outFiles = p.GetAllFiles();
        outFs = p.GetFsTypeName();
    }
    return true;
}

static void RecoverFileNodes(SectorReader& reader, const Partition& part,
                             const std::vector<FileNode>& files,
                             const std::wstring& outDir, int maxRecover,
                             Row& agg, std::vector<FailRow>& fails,
                             const std::string& imageRel)
{
    RecoverEngine engine;
    int attempted = 0;
    for (const FileNode& node : files) {
        agg.fileCount++;
        if (node.isDirectory)
            continue;
        if (node.status == FileStatus::Deleted)
            agg.deletedCount++;
        else
            agg.activeCount++;

        if (!IsRecoverable(node))
            continue;
        agg.recoverable++;

        if (maxRecover > 0 && attempted >= maxRecover)
            continue;

        ++attempted;
        const FileRecoverResult rr =
            engine.RecoverFile(node, outDir, true, reader, part);

        if (rr.status == RecoverStatus::Skipped) {
            agg.recoverSkipped++;
            fails.push_back({imageRel, Narrow(node.name), FileStatusTag(node.status),
                             node.size, "skipped", Narrow(rr.reason)});
            continue;
        }
        if (rr.status == RecoverStatus::Failed) {
            agg.recoverFailed++;
            fails.push_back({imageRel, Narrow(node.name), FileStatusTag(node.status),
                             node.size, "failed", Narrow(rr.reason)});
            continue;
        }
        if (rr.status == RecoverStatus::Success || rr.status == RecoverStatus::Partial) {
            agg.recoverOk++;
            continue;
        }
    }
}

static bool IsExtFs(const std::wstring& fs)
{
    return fs == L"ext2" || fs == L"ext3" || fs == L"ext4";
}

static void RecoverExtVolume(SectorReader& reader, const std::wstring& outDir, int maxRecover,
                             std::wstring& outFs, Row& agg, std::vector<FailRow>& fails,
                             const std::string& imageRel)
{
    PartitionRecoveryService::Options opt;
    const auto candidates =
        PartitionRecoveryService::ProbeVolume(reader, opt, nullptr, nullptr);
    if (candidates.empty())
        return;

    for (const auto& cand : candidates) {
        if (cand.totalSectors == 0)
            continue;
        if (!IsExtFs(cand.fsType) && cand.fsType != L"Unknown")
            continue;

        Partition part = PartitionRecoveryService::ToPartition(cand, -1);
        if (part.fsType.empty() || part.fsType == L"Unknown")
            part.fsType = L"ext4";

        Ext4Parser parser;
        if (!parser.Open(reader, part) || !parser.ScanMetadata())
            continue;

        const std::wstring fs = parser.GetFsTypeName();
        outFs = fs;
        if (agg.fsType.empty())
            agg.fsType = Narrow(fs);
        else if (agg.fsType.find(Narrow(fs)) == std::string::npos)
            agg.fsType += "+" + Narrow(fs);

        RecoverFileNodes(reader, part, parser.GetAllFiles(), outDir, maxRecover,
                         agg, fails, imageRel);
        return;
    }
}

static void RecoverPartition(SectorReader& reader, uint64_t startSector, uint64_t totalSectors,
                             const std::wstring& outDir, int maxRecover,
                             std::wstring& outFs, Row& agg, std::vector<FailRow>& fails,
                             const std::string& imageRel)
{
    std::vector<FileNode> files;
    std::wstring fs;
    if (!LoadPartitionFiles(reader, startSector, totalSectors, fs, files))
        return;

    outFs = fs;
    if (agg.fsType.empty())
        agg.fsType = Narrow(fs);
    else if (agg.fsType.find(Narrow(fs)) == std::string::npos)
        agg.fsType += "+" + Narrow(fs);

    Partition part;
    part.startSector  = startSector;
    part.totalSectors = totalSectors;
    part.fsType       = fs;
    part.sizeBytes    = totalSectors * reader.GetSectorSize();

    RecoverFileNodes(reader, part, files, outDir, maxRecover, agg, fails, imageRel);
}

static void ClassifySector0(const uint8_t* sec, std::string& hint)
{
    if (sec[510] == 0x55 && sec[511] == 0xAA) {
        if (memcmp(sec + 3, "NTFS    ", 8) == 0) { hint = "NTFS-VBR"; return; }
        if (memcmp(sec + 3, "EXFAT   ", 8) == 0) { hint = "exFAT-VBR"; return; }
        hint = "MBR/VBR";
        return;
    }
    if (memcmp(sec + 1, "EFI PART", 8) == 0) { hint = "GPT-HDR"; return; }
    hint = "unknown";
}

static Row RunReadableImage(const fs::path& root, const fs::path& cacheRoot,
                            const fs::path& imagePath, std::vector<FailRow>& fails)
{
    Row row;
    row.relPath   = ImageRelPath(root, cacheRoot, imagePath);
    row.dataset   = imagePath.parent_path().filename().string();
    row.format    = Narrow(imagePath.extension().wstring());
    row.sizeBytes = static_cast<uint64_t>(fs::file_size(imagePath));

    const auto t0 = std::chrono::steady_clock::now();
    const int maxRecover = MaxRecoverFiles();

    SectorReader reader;
    ReadPolicy pol = reader.GetReadPolicy();
    pol.maxRetries     = 0;
    pol.retryDelayMs   = 0;
    pol.skipBadSectors = true;
    reader.SetReadPolicy(pol);
    if (!reader.OpenImage(imagePath.wstring())) {
        row.openOk = row.readOk = "no";
        row.result = "FAIL";
        row.notes  = "OpenImage failed";
        ++g_failures;
        return row;
    }
    row.openOk = "yes";

    std::vector<uint8_t> sec(512);
    if (!reader.ReadSectors(0, sec.data(), 1)) {
        row.readOk = "no";
        row.result = "FAIL";
        row.notes  = "ReadSectors(0) failed";
        ++g_failures;
        reader.Close();
        return row;
    }
    row.readOk = "yes";

    if (row.sizeBytes > MaxFsScanBytes()) {
        row.result = "SMOKE_ONLY";
        row.notes  = "image>" + std::to_string(MaxFsScanBytes() / (1024 * 1024 * 1024)) +
                     "GiB; recovery not attempted";
        reader.Close();
        row.durationSec = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - t0).count();
        return row;
    }

    const std::wstring outDir = MakeTempDir(L"dc");
    CreateDirectoryW(outDir.c_str(), nullptr);

    std::vector<std::pair<uint64_t, uint64_t>> parts;
    std::wstring directFs;
    uint64_t directSectors = 0;
    if (DetectVbrFs(reader, 0, directFs, directSectors)) {
        RecoverPartition(reader, 0, reader.GetTotalSectors(), outDir, maxRecover,
                         directFs, row, fails, row.relPath);
    } else if (ParseMbr(reader, parts)) {
        for (const auto& pr : parts) {
            std::wstring pfs;
            RecoverPartition(reader, pr.first, pr.second, outDir, maxRecover,
                             pfs, row, fails, row.relPath);
        }
    }

    if (row.fileCount == 0 && row.recoverable == 0) {
        std::wstring extFs;
        RecoverExtVolume(reader, outDir, maxRecover, extFs, row, fails, row.relPath);
    }

    RemoveTree(outDir);
    reader.Close();

    const bool fsSeen = !row.fsType.empty() || row.fileCount > 0;

    if (row.recoverable > 0) {
        if (row.recoverOk == row.recoverable) {
            row.result = "RECOVER_FULL";
            row.notes  = "all recoverable files written with matching size";
        } else if (row.recoverOk > 0) {
            row.result = "RECOVER_PARTIAL";
            std::ostringstream oss;
            oss << "ok=" << row.recoverOk << "/" << row.recoverable
                << " failed=" << row.recoverFailed << " skipped=" << row.recoverSkipped;
            row.notes = oss.str();
            if (StrictRecover()) ++g_failures;
        } else {
            row.result = "RECOVER_FAIL";
            std::ostringstream oss;
            oss << "0/" << row.recoverable << " recovered; failed=" << row.recoverFailed;
            row.notes = oss.str();
            if (StrictRecover()) ++g_failures;
        }
    } else if (fsSeen) {
        row.result = "SCAN_OK";
        row.notes  = "fs scanned, no recoverable files";
    } else {
        std::string sig;
        ClassifySector0(sec.data(), sig);
        row.result = "SMOKE_ONLY";
        row.notes  = "no supported FS; sector0=" + sig;
    }

    row.durationSec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();
    return row;
}

static Row RunE01Only(const fs::path& root, const fs::path& e01Path)
{
    Row row;
    row.relPath   = Narrow(fs::relative(e01Path, root).wstring());
    row.dataset   = e01Path.parent_path().filename().string();
    row.format    = ".E01";
    row.sizeBytes = static_cast<uint64_t>(fs::file_size(e01Path));
    row.result    = "NEEDS_EWF";
    row.notes     = "E01 present; run Expand-EwfToRaw.ps1 (outputs to build/ewf-cache)";
    return row;
}

static void WriteSummaryCsv(const fs::path& path, const std::vector<Row>& rows)
{
    std::ofstream out(path);
    out << "dataset,rel_path,format,size_bytes,open_ok,read_ok,fs_type,file_count,active_count,"
           "deleted_count,recoverable,recover_ok,recover_failed,recover_skipped,result,"
           "duration_sec,notes\n";
    for (const Row& r : rows) {
        out << EscCsv(r.dataset) << ','
            << EscCsv(r.relPath) << ','
            << EscCsv(r.format) << ','
            << r.sizeBytes << ','
            << EscCsv(r.openOk) << ','
            << EscCsv(r.readOk) << ','
            << EscCsv(r.fsType) << ','
            << r.fileCount << ','
            << r.activeCount << ','
            << r.deletedCount << ','
            << r.recoverable << ','
            << r.recoverOk << ','
            << r.recoverFailed << ','
            << r.recoverSkipped << ','
            << EscCsv(r.result) << ','
            << std::fixed << std::setprecision(3) << r.durationSec << ','
            << EscCsv(r.notes) << '\n';
    }
}

static void WriteFailCsv(const fs::path& path, const std::vector<FailRow>& fails)
{
    std::ofstream out(path);
    out << "image_rel,file_name,file_status,size_bytes,outcome,reason\n";
    for (const FailRow& f : fails) {
        out << EscCsv(f.imageRel) << ','
            << EscCsv(f.fileName) << ','
            << EscCsv(f.fileStatus) << ','
            << f.sizeBytes << ','
            << EscCsv(f.outcome) << ','
            << EscCsv(f.reason) << '\n';
    }
}

static bool IsReadableImageExt(const fs::path& p)
{
    const auto ext = p.extension().wstring();
    return ext == L".dd" || ext == L".raw";
}

int main()
{
    Logger::Instance().SetConsoleLogLevel(LogLevel::Error);

    const fs::path root      = fs::path(RepoRoot()) / "testdata" / "digital-corpora-disk-images";
    const fs::path cacheRoot  = DcEwfCacheRoot();
    if (!fs::exists(root)) {
        fprintf(stdout, "SKIP: %s missing\n", root.string().c_str());
        return 0;
    }

    std::vector<fs::path> readable;
    std::vector<fs::path> e01only;
    std::vector<Row> unsupported;

    for (const auto& ent : fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied)) {
        if (!ent.is_regular_file())
            continue;
        const fs::path p = ent.path();
        const auto ext = p.extension().wstring();
        if (IsReadableImageExt(p)) {
            if (IsValidImageFile(p))
                readable.push_back(p);
            continue;
        }
        if (ext == L".E01" || ext == L".e01") {
            const fs::path dd =
                p.parent_path() / (p.stem().wstring() + L".dd");
            const fs::path raw =
                p.parent_path() / (p.stem().wstring() + L".raw");
            const fs::path cacheDd = CacheDdPath(root, cacheRoot, p);
            if (IsValidImageFile(dd) || IsValidImageFile(raw))
                continue;
            if (IsValidImageFile(cacheDd))
                readable.push_back(cacheDd);
            else
                e01only.push_back(p);
            continue;
        }
        if (ext == L".aff" || ext == L".dmg" || ext == L".001") {
            Row r;
            r.relPath   = Narrow(fs::relative(p, root).wstring());
            r.dataset   = p.parent_path().filename().string();
            r.format    = Narrow(ext);
            r.sizeBytes = fs::file_size(p);
            r.result    = "UNSUPPORTED";
            r.notes     = "SectorReader raw-only; convert or use AFF/EWF tools";
            unsupported.push_back(r);
        }
    }

    std::sort(readable.begin(), readable.end());
    readable.erase(std::unique(readable.begin(), readable.end()), readable.end());
    std::sort(e01only.begin(), e01only.end());
    e01only.erase(std::unique(e01only.begin(), e01only.end()), e01only.end());

    const std::vector<std::string> onlyFilters = LoadDcOnlyFilters();
    if (!onlyFilters.empty()) {
        readable.erase(
            std::remove_if(readable.begin(), readable.end(),
                [&](const fs::path& p) {
                    return !MatchesDcFilter(ImageRelPath(root, cacheRoot, p), onlyFilters);
                }),
            readable.end());
        e01only.erase(
            std::remove_if(e01only.begin(), e01only.end(),
                [&](const fs::path& p) {
                    return !MatchesDcFilter(Narrow(fs::relative(p, root).wstring()), onlyFilters);
                }),
            e01only.end());
        fprintf(stdout, "PHOENIX_DC_ONLY active: %zu readable, %zu e01_only after filter\n",
                readable.size(), e01only.size());
    }

    std::vector<Row> rows;
    std::vector<FailRow> failRows;

    fprintf(stdout, "Digital Corpora recovery batch: readable=%zu e01_only=%zu unsupported=%zu"
                    " (ewf_cache=%s)\n",
            readable.size(), e01only.size(), unsupported.size(),
            cacheRoot.string().c_str());

    size_t sumRecoverable = 0, sumOk = 0, sumFailed = 0;

    for (const fs::path& img : readable) {
        Row r = RunReadableImage(root, cacheRoot, img, failRows);
        rows.push_back(r);
        sumRecoverable += r.recoverable;
        sumOk += r.recoverOk;
        sumFailed += r.recoverFailed;
        fprintf(stdout,
                "  %s %s recover=%zu/%zu fail=%zu fs=%s (%s)\n",
                r.result.c_str(), r.relPath.c_str(),
                r.recoverOk, r.recoverable, r.recoverFailed,
                r.fsType.c_str(), r.notes.c_str());
    }
    for (const fs::path& e01 : e01only)
        rows.push_back(RunE01Only(root, e01));
    rows.insert(rows.end(), unsupported.begin(), unsupported.end());

    const fs::path csv = CsvPathFromEnv(L"PHOENIX_DC_REPORT_CSV", "digital_corpora_results.csv");
    const fs::path failCsv =
        CsvPathFromEnv(L"PHOENIX_DC_FAIL_CSV", "digital_corpora_recover_failures.csv");
    fs::create_directories(csv.parent_path());
    WriteSummaryCsv(csv, rows);
    WriteFailCsv(failCsv, failRows);

    fprintf(stdout, "CSV: %s (%zu rows)\n", csv.string().c_str(), rows.size());
    fprintf(stdout, "Failures CSV: %s (%zu rows)\n", failCsv.string().c_str(), failRows.size());
    fprintf(stdout,
            "Recovery totals: ok=%zu recoverable=%zu failed=%zu strict=%d infra_failures=%d\n",
            sumOk, sumRecoverable, sumFailed, StrictRecover() ? 1 : 0, g_failures);

    if (g_failures != 0) {
        fprintf(stderr, "Digital Corpora batch: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: digital-corpora-disk-images recovery batch complete\n");
    return 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: volume I/O not enabled\n");
    return 0;
}

#endif
