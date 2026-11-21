// Synthetic NTFS deleted JPEG — scan, recover with stitch, FormatPayloadScore::Complete.
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "../fixtures/fragment_ntfs_structured_fixture.h"
#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "fs/ntfs/NtfsParser.h"
#include "fs/ntfs/NtfsStructs.h"
#include "../../common/Constants.h"
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

static std::wstring WriteTempImage(const std::vector<uint8_t>& image, const wchar_t* tag)
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path =
        std::wstring(tmp) + L"PhoenixRecover_" + tag + L"_" + std::to_wstring(GetTickCount64()) + L".img";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    DWORD written = 0;
    const BOOL ok =
        WriteFile(h, image.data(), static_cast<DWORD>(image.size()), &written, nullptr) &&
        written == image.size();
    CloseHandle(h);
    if (!ok) return L"";
    return path;
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

using FragmentNtfsStructuredFixture::kStructuredFileSize;

static bool VerifyDeletedRecordClusters(const std::vector<uint8_t>& image)
{
    constexpr uint64_t kMftOff = 4 * 1024;
    constexpr uint32_t kRec = 40;
    const uint8_t* rec = image.data() + kMftOff + kRec * 1024;
    if (memcmp(rec, Constants::NTFS_FILE_MAGIC, 4) != 0) {
        fprintf(stderr, "verify: bad FILE magic\n");
        return false;
    }
    const auto* hdr = reinterpret_cast<const MFT_RECORD_HEADER*>(rec);
    const uint8_t* p   = rec + hdr->attrOffset;
    const uint8_t* end = rec + hdr->bytesInUse;
    while (p + sizeof(ATTR_HEADER_COMMON) <= end) {
        const auto* ah = reinterpret_cast<const ATTR_HEADER_COMMON*>(p);
        if (ah->type == Constants::ATTR_END) break;
        if (ah->length < sizeof(ATTR_HEADER_COMMON)) break;
        if (ah->type == Constants::ATTR_DATA && ah->nonResident) {
            const auto* nr = reinterpret_cast<const ATTR_NONRESIDENT*>(p);
            const size_t maxR = (ah->length > nr->dataRunOffset) ? ah->length - nr->dataRunOffset : 0;
            const auto clusters =
                NtfsParser::DecodeDataRuns(p + nr->dataRunOffset, maxR);
            return !clusters.empty();
        }
        p += ah->length;
    }
    fprintf(stderr, "verify: no $DATA attr bytesInUse=%u\n", hdr->bytesInUse);
    return false;
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

static int RunScenario(FragmentNtfsStructuredFixture::NtfsChainMode mode, const wchar_t* label)
{
    std::vector<uint8_t> image;
    FragmentNtfsStructuredFixture::BuildNtfsStructuredVolume(
        image, FragmentNtfsStructuredFixture::StructuredFormat::Jpeg, mode);
    CHECK_MSG(VerifyDeletedRecordClusters(image), "fixture $DATA runs");

    const std::wstring path = WriteTempImage(image, label);
    CHECK_MSG(!path.empty(), "write temp image");
    if (path.empty()) return 1;

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(path), "OpenImage");
    if (!reader.IsOpen()) {
        std::filesystem::remove(path);
        return 1;
    }

    Partition part;
    part.startSector  = 0;
    part.totalSectors = image.size() / 512;
    part.sizeBytes    = image.size();
    part.fsType       = L"NTFS";

    NtfsParser parser;
    CHECK_MSG(parser.Open(reader, part), "NtfsParser::Open");
    CHECK_MSG(parser.ScanMetadata(), "NtfsParser::ScanMetadata");

    const std::wstring wantExt = FragmentNtfsStructuredFixture::FormatExtension(
        FragmentNtfsStructuredFixture::StructuredFormat::Jpeg);
    const auto files = parser.GetAllFiles();
    const FileNode* target = nullptr;
    for (const auto& f : files) {
        if (f.status != FileStatus::Deleted || f.isDirectory) continue;
        if (f.size != FragmentNtfsStructuredFixture::kStructuredFileSize) continue;
        if (f.size == FragmentNtfsStructuredFixture::kStructuredFileSize &&
            f.name.find(L"PHOENIX") != std::wstring::npos &&
            (f.extension == wantExt || f.extension == L"jp")) {
            target = &f;
            break;
        }
    }
    CHECK_MSG(target != nullptr, "deleted JPEG in NTFS scan results");
    if (!target) {
        reader.Close();
        std::filesystem::remove(path);
        return 1;
    }

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring outDir =
        std::wstring(tmp) + L"PhoenixRecover_ntfs_out_" + label + L"_" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(outDir.c_str(), nullptr);

    RecoverEngine engine;
    RecoverOptions opts;
    opts.formatAwareFatFragments = true;
    engine.SetOptions(opts);

    const FileRecoverResult rr = engine.RecoverFile(*target, outDir, false, reader, part);
    CHECK_MSG(rr.status == RecoverStatus::Success, "recover success");

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    uint64_t written = 0;
    if (GetFileAttributesExW(rr.targetPath.c_str(), GetFileExInfoStandard, &fad)) {
        written = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    }
    CHECK_MSG(written == target->size, "written size");

    const auto sc = ScoreFile(rr.targetPath, wantExt);
    fprintf(stdout, "  [%ls] recovered %ls score=%s size=%llu clusters=%zu\n",
            label,
            target->name.c_str(),
            ScoreLabel(sc),
            static_cast<unsigned long long>(written),
            target->clusters.size());

    CHECK_MSG(sc >= FormatPayloadScore::Complete, "expected Complete JPEG");

    std::error_code ec;
    std::filesystem::remove_all(outDir, ec);
    reader.Close();
    std::filesystem::remove(path);
    return 0;
}

int main()
{
    using Mode = FragmentNtfsStructuredFixture::NtfsChainMode;

    int rc = 0;
    rc |= RunScenario(Mode::Intact, L"ntfs_jpg_intact");
    rc |= RunScenario(Mode::BrokenBraid, L"ntfs_jpg_stitch");

    if (g_failures > 0 || rc != 0) {
        fprintf(stderr, "FragmentNtfsStructured_QualityGate: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: FragmentNtfsStructured_QualityGate\n");
    return 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: FragmentNtfsStructured_QualityGate requires volume I/O platform\n");
    return 0;
}

#endif
