// Batch validation for NIST CFReDS DFR Linux ext2/3/4 images (*-ext.dd).

// Scans testdata/dfr-test-images, testdata/ext4-variants, testdata/DFR, testdata/cfreds;
// writes CSV to PHOENIX_DFR_LINUX_REPORT_CSV.

#include "common/Platform.h"



#if PHOENIX_ENABLE_VOLUME_IO



#include "disk/Partition.h"

#include "disk/PartitionRecoveryService.h"

#include "disk/SectorReader.h"

#include "fs/FileSystem.h"

#include "recover/FileNode.h"
#include "recover/RecoverEngine.h"

#include "scan/FileSystemModuleService.h"

#include "scan/ScanEngine.h"

#include "common/Utils.h"



#include <chrono>

#include <cstdio>

#include <filesystem>

#include <string>

#include <vector>

#include <algorithm>

#include <system_error>



namespace fs = std::filesystem;



static int g_failures = 0;



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

};

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
    std::error_code ec;
    const uint64_t sz = fs::file_size(fs::path(path), ec);
    return ec ? 0 : sz;
}

static std::wstring MakeTempDir(const wchar_t* tag)
{
    std::error_code ec;
    fs::path base = fs::temp_directory_path(ec);
    if (ec)
        base = fs::path(L".");
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path dir = base / (std::wstring(L"PhoenixRecover_dfr_") + tag + L"_" + std::to_wstring(stamp));
    fs::create_directories(dir, ec);
    return dir.wstring();
}

static void RemoveTree(const std::wstring& root)
{
    std::error_code ec;
    fs::remove_all(fs::path(root), ec);
}

static int MaxRecoverPerImage()
{
    const std::wstring v = Utils::GetEnvironmentVariableW(L"PHOENIX_DFR_LINUX_MAX_RECOVER");
    if (v.empty())
        return 16;
    return static_cast<int>(std::max(0L, std::wcstol(v.c_str(), nullptr, 10)));
}

static bool KeepTestOutput()
{
    const std::wstring v = Utils::GetEnvironmentVariableW(L"PHOENIX_KEEP_TEST_OUTPUT");
    return !v.empty() && v != L"0";
}

static void RemoveFileBestEffort(const std::wstring& path)
{
    std::error_code ec;
    fs::remove(fs::path(path), ec);
}



static bool IsLinuxExtImageName(const std::wstring& name)

{

    std::wstring lower = name;

    for (auto& c : lower)

        c = static_cast<wchar_t>(towlower(c));

    if (lower.find(L"-osx.") != std::wstring::npos)

        return false;

    return lower.find(L"-ext.") != std::wstring::npos ||

           lower.find(L"recycle-ext") != std::wstring::npos;

}



static std::wstring DatasetIdFromPath(const fs::path& p)

{

    return p.stem().wstring();

}



static int RootPriority(const fs::path& p)

{

    const std::string s = p.string();

    if (s.find("dfr-test-images") != std::string::npos)

        return 3;

    if (s.find("ext4-variants") != std::string::npos)

        return 2;

    if (s.find("\\DFR\\") != std::string::npos || s.find("/DFR/") != std::string::npos)

        return 1;

    if (s.find("cfreds") != std::string::npos)

        return 0;

    return -1;

}



static std::vector<std::wstring> SplitCommaList(const std::wstring& s)
{
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == L',' || c == L';') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        if (c != L' ' && c != L'\t')
            cur += c;
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

static void CollectImages(const fs::path& root, std::vector<fs::path>& out)

{

    if (!fs::exists(root))

        return;

    for (const auto& ent : fs::recursive_directory_iterator(

             root, fs::directory_options::skip_permission_denied)) {

        if (!ent.is_regular_file())

            continue;

        if (ent.path().extension().wstring() != L".dd")

            continue;

        if (!IsLinuxExtImageName(ent.path().filename().wstring()))

            continue;

        out.push_back(ent.path());

    }

}



static const PartitionRecoveryService::Candidate* PickExt4Candidate(

    const std::vector<PartitionRecoveryService::Candidate>& candidates)

{

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

    if (!extCand && !candidates.empty()) {

        for (const auto& c : candidates) {

            if (c.totalSectors > 0) {

                extCand = &c;

                break;

            }

        }

    }

    return extCand;

}



static Row ValidateImage(const fs::path& imagePath, int maxRecover)

{

    Row row;

    row.imagePath = imagePath.wstring();

    row.datasetId = DatasetIdFromPath(imagePath);

    row.fsKind    = L"ext";



    const auto t0 = std::chrono::steady_clock::now();



    if (!fs::exists(imagePath)) {

        row.result = L"SKIP";

        row.notes  = L"file missing";

        return row;

    }



    SectorReader reader;

    if (!reader.OpenImage(row.imagePath)) {

        row.result = L"FAIL";

        row.notes  = L"OpenImage failed";

        row.durationSec =

            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        return row;

    }



    PartitionRecoveryService::Options opt;

    auto candidates = PartitionRecoveryService::ProbeVolume(reader, opt, nullptr, nullptr);

    const auto* extCand = PickExt4Candidate(candidates);

    if (!extCand) {

        reader.Close();

        row.result = L"FAIL";

        row.notes  = L"no ext4 partition candidate";

        row.durationSec =

            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        return row;

    }



    Partition part = PartitionRecoveryService::ToPartition(*extCand, -1);

    if (part.fsType.empty())

        part.fsType = L"ext4";
    row.fsKind = part.fsType;

    ScanResult scanMeta;

    auto fsParser = FileSystemModuleService::CreateParserForPartition(reader, part, scanMeta);

    if (!fsParser || !fsParser->Open(reader, part) || !fsParser->ScanMetadata()) {

        reader.Close();

        row.result = L"FAIL";

        row.notes  = L"ext4 Open/ScanMetadata failed";

        row.durationSec =

            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

        return row;

    }



    const auto& files = fsParser->GetAllFiles();

    row.totalFiles    = files.size();

    for (const auto& f : files) {

        if (f.status == FileStatus::Deleted && !f.isDirectory)

            ++row.deletedFiles;

    }

    const std::wstring outDir = MakeTempDir(row.datasetId.c_str());
    RecoverEngine engine;

    bool recoveredFromDeleted = false;
    int deletedRecoverable = 0;
    int allocatedRecoverable = 0;

    // 1) Recover ALL recoverable Deleted files (primary validation goal).
    for (const FileNode& node : files) {
        if (node.status != FileStatus::Deleted || !Recoverable(node))
            continue;

        ++deletedRecoverable;
        ++row.recoverAttempted;
        const FileRecoverResult rr = engine.RecoverFile(node, outDir, false, reader, part);
        if (rr.status != RecoverStatus::Success)
            continue;
        if (FileSizeOnDisk(rr.targetPath) == node.size) {
            ++row.recoverOk;
            recoveredFromDeleted = true;
        }
        if (!KeepTestOutput())
            RemoveFileBestEffort(rr.targetPath);
    }

    // 2) If there are no Deleted files, validate end-to-end recovery by recovering ALL
    // recoverable Allocated files (deep path listing scenarios like DFR-17 may have 0 deletes).
    if (deletedRecoverable == 0) {
        for (const FileNode& node : files) {
            if (node.isDirectory || !Recoverable(node))
                continue;
            if (node.status == FileStatus::Deleted)
                continue;

            ++allocatedRecoverable;
            ++row.recoverAttempted;
            const FileRecoverResult rr = engine.RecoverFile(node, outDir, false, reader, part);
            if (rr.status != RecoverStatus::Success)
                continue;
            if (FileSizeOnDisk(rr.targetPath) == node.size) {
                ++row.recoverOk;
            }
            if (!KeepTestOutput())
                RemoveFileBestEffort(rr.targetPath);
        }
    }

    if (!KeepTestOutput()) {
        RemoveTree(outDir);
    } else {
        row.notes += L" outDir=" + outDir;
    }
    reader.Close();

    row.durationSec =

        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();



    if (row.totalFiles == 0) {

        row.result = L"FAIL";

        row.notes  = L"ext4 scan returned zero file nodes";

        return row;
    }

    const int expectedOk =
        (deletedRecoverable > 0) ? deletedRecoverable : allocatedRecoverable;

    if (expectedOk > 0 && row.recoverOk >= expectedOk) {
        row.result = L"PASS";
        if (recoveredFromDeleted) {
            row.notes = L"scan+recover ok (all deleted, size match)" + row.notes;
        } else if (row.deletedFiles == 0) {
            row.notes = L"scan ok, no Deleted nodes; recovered all allocated (size match)" + row.notes;
        } else {
            row.notes = L"deleted found but recovered allocated only (size match)" + row.notes;
        }
    } else if (deletedRecoverable == 0 && allocatedRecoverable == 0) {
        row.result = L"WARN";
        row.notes  = L"scan ok, but no recoverable files found (deleted=0, allocated=0)" + row.notes;
    } else {
        row.result = L"PARTIAL";
        if (deletedRecoverable > 0) {
            row.notes = L"deleted found, not all deleted recovered (or size mismatch)" + row.notes;
        } else {
            row.notes = L"no deleted, not all allocated recovered (or size mismatch)" + row.notes;
        }
    }



    return row;

}



int main()

{

    std::wstring repo;

#ifdef PHOENIX_SOURCE_DIR

    repo = PHOENIX_SOURCE_DIR;

#else

    repo = Utils::GetEnvironmentVariableW(L"PHOENIX_SOURCE_DIR");

#endif

    if (repo.empty()) {

        fprintf(stderr, "FAIL: PHOENIX_SOURCE_DIR not set\n");

        return 1;

    }



    std::vector<fs::path> images;

    const fs::path repoPath(repo);

    const std::wstring scanRoots = Utils::GetEnvironmentVariableW(L"PHOENIX_DFR_LINUX_SCAN_ROOTS");
    if (scanRoots.empty()) {
        CollectImages(repoPath / "testdata" / "dfr-test-images", images);
        CollectImages(repoPath / "testdata" / "ext4-variants", images);
        CollectImages(repoPath / "testdata" / "DFR", images);
        CollectImages(repoPath / "testdata" / "cfreds", images);
    } else {
        for (const std::wstring& rel : SplitCommaList(scanRoots)) {
            CollectImages(repoPath / "testdata" / rel, images);
        }
    }



    std::vector<fs::path> unique;

    for (const auto& p : images) {

        const std::wstring id = DatasetIdFromPath(p);

        auto it               = std::find_if(unique.begin(), unique.end(), [&](const fs::path& q) {

            return DatasetIdFromPath(q) == id;

        });

        if (it == unique.end()) {

            unique.push_back(p);

            continue;

        }

        if (RootPriority(p) > RootPriority(*it))

            *it = p;

    }

    images = std::move(unique);

    std::sort(images.begin(), images.end());



    if (images.empty()) {

        fprintf(stdout,

                "SKIP: no Linux DFR *-ext.dd images under testdata/dfr-test-images, ext4-variants, DFR, or cfreds\n");

        return 0;

    }



    std::wstring csvPath = Utils::GetEnvironmentVariableW(L"PHOENIX_DFR_LINUX_REPORT_CSV");

    if (csvPath.empty())

        csvPath = Utils::GetEnvironmentVariableW(L"PHOENIX_DFR_REPORT_CSV");

    if (csvPath.empty())

        csvPath = Utils::GetTempDirectoryW() + L"PhoenixRecover_dfr_linux_report.csv";



    FILE* csv = nullptr;

#if defined(_MSC_VER)

    _wfopen_s(&csv, csvPath.c_str(), L"wt, ccs=UTF-8");

#else

    const std::string csvUtf8 = Utils::WideToUtf8(csvPath);

    csv                       = fopen(csvUtf8.c_str(), "wb");

    if (csv) {

        static const unsigned char bom[] = {0xEF, 0xBB, 0xBF};

        fwrite(bom, 1, sizeof(bom), csv);

    }

#endif

    if (!csv) {

        fprintf(stderr, "FAIL: cannot open CSV\n");

        return 1;

    }



    fwprintf(csv,

             L"dataset_id,fs_kind,result,duration_sec,total_files,deleted_files,recover_ok,"

             L"image_path,notes\n");



    int pass = 0;

    int partial = 0;

    int fail = 0;

    int warn = 0;

    const int maxRecover = MaxRecoverPerImage();



    fprintf(stdout, "DFR Linux ext2/3/4 batch: %zu image(s) (max_recover=%d)\n",

            images.size(), maxRecover);



    for (const auto& p : images) {

        Row row = ValidateImage(p, maxRecover);

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



        fprintf(stdout,

                "  %-32ls %-6ls %-8ls %.2fs nodes=%zu del=%zu rec=%d/%d  %ls\n",

                row.datasetId.c_str(),

                row.fsKind.c_str(),

                row.result.c_str(),

                row.durationSec,

                row.totalFiles,

                row.deletedFiles,

                row.recoverOk,

                row.recoverAttempted,

                row.notes.c_str());



        if (row.result == L"PASS")

            ++pass;

        else if (row.result == L"PARTIAL")

            ++partial;

        else if (row.result == L"WARN")

            ++warn;

        else if (row.result == L"FAIL")

            ++fail;

    }



    fclose(csv);

    fwprintf(stdout, L"\nCSV: %ls\n", csvPath.c_str());

    fprintf(stdout, "Summary: PASS=%d PARTIAL=%d WARN=%d FAIL=%d (total=%zu)\n",

            pass, partial, warn, fail, images.size());



    if (fail > 0)

        ++g_failures;



    return g_failures > 0 ? 1 : 0;

}



#else



#include <cstdio>

int main()

{

    fprintf(stdout, "SKIP: DFR Linux batch requires volume I/O platform\n");

    return 0;

}



#endif


