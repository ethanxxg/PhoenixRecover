// Synthetic FAT16 deleted structured files — scan, recover, Complete gate per format.
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "../fixtures/fragment_fat_structured_fixture.h"
#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "fs/fat/FatParser.h"
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

static int RunScenario(FragmentFatStructuredFixture::StructuredFormat format,
                       FragmentFatStructuredFixture::FatChainMode mode,
                       const wchar_t* label)
{
    std::vector<uint8_t> image;
    FragmentFatStructuredFixture::BuildFat16StructuredVolume(image, format, mode);

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
    part.fsType       = L"FAT16";

    FatParser parser;
    CHECK_MSG(parser.Open(reader, part), "FatParser::Open");
    CHECK_MSG(parser.ScanMetadata(), "FatParser::ScanMetadata");

    const std::wstring wantExt = FragmentFatStructuredFixture::FormatExtension(format);
    const auto files = parser.GetAllFiles();
    const FileNode* target = nullptr;
    for (const auto& f : files) {
        if (f.status != FileStatus::Deleted || f.isDirectory) continue;
        if (f.size != FragmentFatStructuredFixture::kStructuredFileSize) continue;
        if (f.extension == wantExt) {
            target = &f;
            break;
        }
    }
    CHECK_MSG(target != nullptr, "deleted structured file in scan results");
    if (!target) {
        reader.Close();
        std::filesystem::remove(path);
        return 1;
    }

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring outDir =
        std::wstring(tmp) + L"PhoenixRecover_out_" + label + L"_" + std::to_wstring(GetTickCount64());
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

    const std::wstring ext = target->extension.empty() ? wantExt : target->extension;
    const auto sc = ScoreFile(rr.targetPath, ext);
    fprintf(stdout, "  [%ls] recovered %ls score=%s size=%llu\n",
            label,
            target->name.c_str(),
            ScoreLabel(sc),
            static_cast<unsigned long long>(written));

    CHECK_MSG(sc >= FormatPayloadScore::Complete, "expected Complete structured payload");

    std::error_code ec;
    std::filesystem::remove_all(outDir, ec);
    reader.Close();
    std::filesystem::remove(path);
    return 0;
}

int main()
{
    using Fmt = FragmentFatStructuredFixture::StructuredFormat;
    using Mode = FragmentFatStructuredFixture::FatChainMode;

    struct Row {
        Fmt fmt;
        Mode mode;
        const wchar_t* label;
    };
    const Row rows[] = {
        { Fmt::Jpeg, Mode::Intact,           L"frag_jpg_intact" },
        { Fmt::Jpeg, Mode::BrokenWithDecoy,  L"frag_jpg_stitch" },
        { Fmt::Png,  Mode::Intact,           L"frag_png_intact" },
        { Fmt::Png,  Mode::BrokenWithDecoy,  L"frag_png_stitch" },
        { Fmt::Pdf,  Mode::Intact,           L"frag_pdf_intact" },
        { Fmt::Pdf,  Mode::BrokenWithDecoy,  L"frag_pdf_stitch" },
        { Fmt::Zip,  Mode::Intact,           L"frag_zip_intact" },
        { Fmt::Zip,  Mode::BrokenWithDecoy,  L"frag_zip_stitch" },
    };

    int rc = 0;
    for (const Row& row : rows) {
        rc |= RunScenario(row.fmt, row.mode, row.label);
    }

    if (g_failures > 0 || rc != 0) {
        fprintf(stderr, "FragmentFatStructured_QualityGate: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: FragmentFatStructured_QualityGate (jpg/png/pdf/zip x2 modes)\n");
    return 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: FragmentFatStructured_QualityGate requires volume I/O platform\n");
    return 0;
}

#endif
