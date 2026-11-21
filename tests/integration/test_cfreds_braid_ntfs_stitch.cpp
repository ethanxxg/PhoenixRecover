// CFReDS DFR-05-BRAID-NTFS — fragment stitch enabled recovery smoke + structured Complete tally.
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "disk/SectorReader.h"
#include "disk/Partition.h"
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

static std::wstring ImagePath()
{
#ifdef PHOENIX_SOURCE_DIR
    return (std::filesystem::path(PHOENIX_SOURCE_DIR) / "testdata" / "cfreds" /
            "dfr-05-braid-ntfs.dd")
        .wstring();
#else
    return L"";
#endif
}

struct MbrEntry {
    uint64_t startSector;
    uint64_t totalSectors;
    uint8_t  type;
};

static bool ParseMbr(SectorReader& reader, std::vector<MbrEntry>& entries)
{
    uint8_t mbr[512]{};
    if (!reader.ReadSectors(0, mbr, 1)) return false;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return false;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = mbr + 446 + i * 16;
        if (e[4] == 0) continue;
        uint32_t start = *reinterpret_cast<const uint32_t*>(e + 8);
        uint32_t count = *reinterpret_cast<const uint32_t*>(e + 12);
        if (start > 0 && count > 0) entries.push_back({ start, count, e[4] });
    }
    return !entries.empty();
}

static FormatPayloadScore ScoreFile(const std::wstring& path, const std::wstring& ext)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return FormatPayloadScore::Invalid;
    std::vector<uint8_t> buf(256 * 1024);
    DWORD got = 0;
    if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) || got == 0) {
        CloseHandle(h);
        return FormatPayloadScore::Weak;
    }
    CloseHandle(h);
    return FormatPayloadScoreUtil::Score(ext, buf.data(), static_cast<size_t>(got));
}

int main()
{
    const std::wstring imagePath = ImagePath();
    if (imagePath.empty() || !std::filesystem::exists(imagePath)) {
        fprintf(stdout, "SKIP: dfr-05-braid-ntfs.dd not found\n");
        return 0;
    }

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage");
    if (!reader.IsOpen()) return 1;

    std::vector<MbrEntry> entries;
    CHECK_MSG(ParseMbr(reader, entries), "MBR");

    uint64_t start = 0, sectors = 0;
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
    part.startSector  = start;
    part.totalSectors = sectors;
    part.sizeBytes    = sectors * reader.GetSectorSize();
    part.fsType       = L"NTFS";

    NtfsParser parser;
    CHECK_MSG(parser.Open(reader, part), "NtfsParser::Open");
    CHECK_MSG(parser.ScanMetadata(), "ScanMetadata");

    const auto files = parser.GetAllFiles();

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring outDir =
        std::wstring(tmp) + L"PhoenixRecover_braid_ntfs_" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(outDir.c_str(), nullptr);

    RecoverEngine engine;
    RecoverOptions opts;
    opts.formatAwareFatFragments = true;
    engine.SetOptions(opts);

    int recovered = 0;
    int structured = 0;
    int complete = 0;
    int improved = 0;
  constexpr int kMaxCompare = 12;
    int compared = 0;

    for (const auto& node : files) {
        if (compared >= kMaxCompare) break;
        if (node.status != FileStatus::Deleted || node.isDirectory || node.size == 0) continue;
        if (node.residentData.empty() && node.clusters.empty()) continue;

        std::wstring ext = node.extension;
        for (auto& c : ext) {
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        }
        if (!FormatPayloadScoreUtil::SupportsFragmentStitch(ext)) continue;

        ++structured;

        FileNode offNode = node;
        RecoverOptions offOpts = opts;
        offOpts.formatAwareFatFragments = false;
        engine.SetOptions(offOpts);
        const std::wstring offDir = outDir + L"\\off";
        CreateDirectoryW(offDir.c_str(), nullptr);
        const auto rrOff = engine.RecoverFile(offNode, offDir, false, reader, part);

        engine.SetOptions(opts);
        FileNode onNode = node;
        const std::wstring onDir = outDir + L"\\on";
        CreateDirectoryW(onDir.c_str(), nullptr);
        const auto rrOn = engine.RecoverFile(onNode, onDir, false, reader, part);

        if (rrOn.status != RecoverStatus::Success) continue;
        ++recovered;

        const auto scOn = ScoreFile(rrOn.targetPath, ext);
        if (scOn >= FormatPayloadScore::Complete) ++complete;

        if (rrOff.status == RecoverStatus::Success) {
            const auto scOff = ScoreFile(rrOff.targetPath, ext);
            if (static_cast<int>(scOn) > static_cast<int>(scOff)) ++improved;
        }
        ++compared;
    }

    std::error_code ec;
    std::filesystem::remove_all(outDir, ec);
    reader.Close();

    fprintf(stdout,
            "Braid NTFS stitch: structured=%d recovered=%d complete=%d improved=%d\n",
            structured,
            recovered,
            complete,
            improved);

    if (structured > 0) {
        CHECK_MSG(recovered >= 1, "expected >=1 structured recovery on braid NTFS sample");
    } else {
        fprintf(stdout, "NOTE: no stitchable structured deleted files; stitch path smoke only\n");
    }

    if (g_failures > 0) {
        fprintf(stderr, "CFReDS_BraidNtfs_FragmentStitch: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: CFReDS_BraidNtfs_FragmentStitch\n");
    return 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: CFReDS_BraidNtfs_FragmentStitch requires volume I/O platform\n");
    return 0;
}

#endif
