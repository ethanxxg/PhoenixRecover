// CFReDS DFR-05 (incl. braid) — quality curve for recovered bytes.
// Exports a sample of deleted files and scores them with FormatPayloadScore.
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
#include "recover/FormatPayloadScore.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

static int g_failures = 0;

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

static uint64_t FileSizeOnDisk(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return 0;
    uint64_t hi = fad.nFileSizeHigh;
    return (hi << 32) | fad.nFileSizeLow;
}

static bool Recoverable(const FileNode& f)
{
    if (f.isDirectory || f.size == 0) return false;
    if (!f.residentData.empty()) return true;
    if (!f.clusters.empty()) return true;
    return f.rawSize > 0;
}

static std::wstring LowerExt(std::wstring ext)
{
    for (auto& c : ext) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return ext;
}

static const char* ScoreLabel(FormatPayloadScore s)
{
    switch (s) {
    case FormatPayloadScore::Complete: return "Complete";
    case FormatPayloadScore::Partial:  return "Partial";
    case FormatPayloadScore::Weak:     return "Weak";
    case FormatPayloadScore::Invalid:  return "Invalid";
    }
    return "Unknown";
}

static int ScoreRecoveredFile(const std::wstring& path,
                              const std::wstring& extLower,
                              size_t headBytes,
                              FormatPayloadScore& outScore)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        outScore = FormatPayloadScore::Invalid;
        return 0;
    }
    std::vector<uint8_t> buf(headBytes);
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr);
    CloseHandle(h);
    if (!ok || got == 0) {
        outScore = FormatPayloadScore::Weak;
        return 0;
    }
    outScore = FormatPayloadScoreUtil::Score(extLower, buf.data(), static_cast<size_t>(got));
    return 1;
}

struct MbrEntry { uint64_t startSector; uint64_t totalSectors; uint8_t type; };
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

static int ExportAndScoreFat(const std::wstring& imagePath,
                             const wchar_t* datasetId,
                             FILE* csv,
                             int maxFiles)
{
    if (!std::filesystem::exists(imagePath)) {
        fprintf(stdout, "SKIP: %ls not found\n", imagePath.c_str());
        return 0;
    }

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");
    if (!reader.IsOpen()) return 1;

    std::vector<MbrEntry> entries;
    if (!ParseMbr(reader, entries)) {
        fprintf(stdout, "SKIP: %ls has no parseable MBR (avoid raw-volume offset probing)\n",
                imagePath.c_str());
        reader.Close();
        return 0;
    }

    std::wstring outDir = MakeTempDir(L"dfr05q_fat");
    RecoverEngine engine;

    int attempted = 0;
    int ok = 0;
    int scored = 0;
    bool anyFsOpened = false;

    for (size_t i = 0; i < entries.size(); ++i) {
        auto& e = entries[i];
        if (e.type == 0x05 || e.type == 0x0F || e.type == 0x85) continue;

        Partition part;
        part.startSector  = e.startSector;
        part.totalSectors = e.totalSectors;
        part.sizeBytes    = e.totalSectors * reader.GetSectorSize();
        part.fsType       = L"FAT";

        FatParser parser;
        if (!parser.Open(reader, part) || !parser.ScanMetadata()) {
            continue;
        }
        anyFsOpened = true;
        const std::wstring fsName = parser.GetFsTypeName();
        auto files = parser.GetAllFiles();
        for (const FileNode& node : files) {
            if (node.status != FileStatus::Deleted || !Recoverable(node)) continue;
            if (maxFiles > 0 && attempted >= maxFiles) break;

            ++attempted;
            FileRecoverResult rr = engine.RecoverFile(node, outDir, false, reader, part);
            if (rr.status != RecoverStatus::Success) continue;

            const uint64_t written = FileSizeOnDisk(rr.targetPath);
            if (written != node.size) continue;
            ++ok;

            std::wstring extLower = LowerExt(node.extension);
            if (extLower.empty()) {
                // Probe from file header
                std::vector<uint8_t> probe(512);
                HANDLE h = CreateFileW(rr.targetPath.c_str(), GENERIC_READ,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    DWORD got = 0;
                    if (ReadFile(h, probe.data(), static_cast<DWORD>(probe.size()), &got, nullptr) && got > 0) {
                        extLower = FormatPayloadScoreUtil::ProbeExtension(probe.data(), static_cast<size_t>(got));
                    }
                    CloseHandle(h);
                }
            }

            FormatPayloadScore sc = FormatPayloadScore::Weak;
            if (ScoreRecoveredFile(rr.targetPath, extLower, 256 * 1024, sc)) {
                ++scored;
                fwprintf(csv,
                         L"%ls,%ls,%ls,%ls,%llu,%hs\n",
                         datasetId,
                         fsName.c_str(),
                         extLower.c_str(),
                         node.name.c_str(),
                         static_cast<unsigned long long>(node.size),
                         ScoreLabel(sc));
            }
        }
        if (maxFiles > 0 && attempted >= maxFiles) break;
    }

    RemoveTree(outDir);
    reader.Close();

    fprintf(stdout, "  [%ls] FAT export attempted=%d ok=%d qualityScored=%d\n", datasetId, attempted, ok, scored);
    // Only enforce a minimum if we managed to open a filesystem.
    if (anyFsOpened) {
        CHECK_MSG(ok >= 1, "expected at least 1 successful FAT recovery");
    }
    return 0;
}

static int ExportAndScoreNtfs(const std::wstring& imagePath,
                              const wchar_t* datasetId,
                              FILE* csv,
                              int maxFiles)
{
    if (!std::filesystem::exists(imagePath)) {
        fprintf(stdout, "SKIP: %ls not found\n", imagePath.c_str());
        return 0;
    }

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");
    if (!reader.IsOpen()) return 1;

    std::vector<MbrEntry> entries;
    CHECK_MSG(ParseMbr(reader, entries), "MBR parse failed");

    uint64_t start = 0, sectors = 0;
    for (auto& e : entries) {
        if (e.type == 0x07) { start = e.startSector; sectors = e.totalSectors; break; }
    }
    if (start == 0 && !entries.empty()) { start = entries[0].startSector; sectors = entries[0].totalSectors; }
    CHECK_MSG(start > 0 && sectors > 0, "NTFS partition not found");

    Partition part;
    part.startSector  = start;
    part.totalSectors = sectors;
    part.sizeBytes    = sectors * reader.GetSectorSize();
    part.fsType       = L"NTFS";

    NtfsParser parser;
    CHECK_MSG(parser.Open(reader, part), "NtfsParser::Open failed");
    CHECK_MSG(parser.ScanMetadata(), "NtfsParser::ScanMetadata failed");

    auto files = parser.GetAllFiles();

    std::wstring outDir = MakeTempDir(L"dfr05q_ntfs");
    RecoverEngine engine;

    int attempted = 0;
    int ok = 0;
    int scored = 0;

    for (const FileNode& node : files) {
        if (node.status != FileStatus::Deleted || !Recoverable(node)) continue;
        if (maxFiles > 0 && attempted >= maxFiles) break;
        ++attempted;

        FileRecoverResult rr = engine.RecoverFile(node, outDir, false, reader, part);
        if (rr.status != RecoverStatus::Success) continue;
        const uint64_t written = FileSizeOnDisk(rr.targetPath);
        if (written != node.size) continue;
        ++ok;

        std::wstring extLower = LowerExt(node.extension);
        if (extLower.empty()) {
            std::vector<uint8_t> probe(512);
            HANDLE h = CreateFileW(rr.targetPath.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD got = 0;
                if (ReadFile(h, probe.data(), static_cast<DWORD>(probe.size()), &got, nullptr) && got > 0) {
                    extLower = FormatPayloadScoreUtil::ProbeExtension(probe.data(), static_cast<size_t>(got));
                }
                CloseHandle(h);
            }
        }

        FormatPayloadScore sc = FormatPayloadScore::Weak;
        if (ScoreRecoveredFile(rr.targetPath, extLower, 256 * 1024, sc)) {
            ++scored;
            fwprintf(csv,
                     L"%ls,NTFS,%ls,%ls,%llu,%hs\n",
                     datasetId,
                     extLower.c_str(),
                     node.name.c_str(),
                     static_cast<unsigned long long>(node.size),
                     ScoreLabel(sc));
        }
    }

    RemoveTree(outDir);
    reader.Close();

    fprintf(stdout, "  [%ls] NTFS export attempted=%d ok=%d qualityScored=%d\n", datasetId, attempted, ok, scored);
    CHECK_MSG(ok >= 1, "expected at least 1 successful NTFS recovery");
    return 0;
}

int main()
{
    const std::wstring root = RepoRoot();
    if (root.empty()) {
        fprintf(stdout, "SKIP: PHOENIX_SOURCE_DIR not set\n");
        return 0;
    }
    namespace fs = std::filesystem;

    const std::wstring outDir = MakeTempDir(L"dfr05_quality");
    const std::wstring csvPath = (fs::path(outDir) / "dfr05_quality_curve.csv").wstring();

    FILE* f = nullptr;
    _wfopen_s(&f, csvPath.c_str(), L"wt, ccs=UTF-8");
    CHECK_MSG(f != nullptr, "cannot open csv for write");
    if (!f) return 1;

    fwprintf(f, L"dataset_id,fs,ext,name,size_bytes,score\n");

    int rc = 0;
    // Note: DFTT-11 contains carved/bad-sector patterns that can be extremely slow to traverse
    // when running in a generic "export some deleted files" loop. Keep this test focused on
    // CFReDS DFR-05 quality baselines to avoid flakiness/hangs.
    rc |= ExportAndScoreFat((fs::path(root) / "testdata" / "cfreds" / "dfr-05-fat.dd").wstring(),
                            L"CFREDS-DFR-05-FAT", f, 32);
    rc |= ExportAndScoreNtfs((fs::path(root) / "testdata" / "cfreds" / "dfr-05-ntfs.dd").wstring(),
                             L"CFREDS-DFR-05-NTFS", f, 48);
    rc |= ExportAndScoreFat((fs::path(root) / "testdata" / "cfreds" / "dfr-05-braid-fat.dd").wstring(),
                            L"CFREDS-DFR-05-BRAID-FAT", f, 32);
    rc |= ExportAndScoreNtfs((fs::path(root) / "testdata" / "cfreds" / "dfr-05-braid-ntfs.dd").wstring(),
                             L"CFREDS-DFR-05-BRAID-NTFS", f, 48);

    fclose(f);

    const bool keep = (GetEnvironmentVariableW(L"PHOENIX_KEEP_TEST_OUTPUT", nullptr, 0) > 0);
    if (keep) {
        fprintf(stdout, "KEEP: %ls\n", csvPath.c_str());
    } else {
        RemoveTree(outDir);
    }

    if (g_failures > 0 || rc != 0) {
        return 1;
    }
    fprintf(stdout, "OK: CFReDS DFR-05 quality curve\n");
    return 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: DFR-05 quality curve requires volume I/O platform\n");
    return 0;
}

#endif

