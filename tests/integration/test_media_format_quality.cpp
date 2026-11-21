// Structured-format quality baseline using shipped media testdata (no disk image).
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "recover/FormatPayloadScore.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
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

static bool ReadFilePrefix(const std::wstring& path, size_t maxBytes, std::vector<uint8_t>& out)
{
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) return false;
    out.resize(maxBytes);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(maxBytes));
    const auto got = static_cast<size_t>(f.gcount());
    out.resize(got);
    return got > 0;
}

static int ScoreMediaFile(const std::wstring& path,
                          const wchar_t* datasetId,
                          const wchar_t* ext,
                          FILE* csv,
                          FormatPayloadScore minExpected)
{
    if (!std::filesystem::exists(path)) {
        fprintf(stdout, "SKIP: %ls not found\n", path.c_str());
        return 0;
    }

    std::vector<uint8_t> buf;
    CHECK_MSG(ReadFilePrefix(path, 512 * 1024, buf), "read file prefix");
    if (buf.empty()) return 1;

    const auto sc = FormatPayloadScoreUtil::Score(ext, buf.data(), buf.size());
    const std::wstring name = std::filesystem::path(path).filename().wstring();
    const uint64_t sz = static_cast<uint64_t>(std::filesystem::file_size(path));

    fwprintf(csv,
             L"%ls,MEDIA,%ls,%ls,%llu,%hs\n",
             datasetId,
             ext,
             name.c_str(),
             static_cast<unsigned long long>(sz),
             ScoreLabel(sc));

    fprintf(stdout,
            "  %ls -> %s (min expected %s)\n",
            name.c_str(),
            ScoreLabel(sc),
            ScoreLabel(minExpected));

    CHECK_MSG(static_cast<int>(sc) >= static_cast<int>(minExpected), "score below baseline");
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
    const fs::path media = fs::path(root) / "testdata" / "media";

    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring csvPath =
        std::wstring(tmp) + L"PhoenixRecover_media_quality_" + std::to_wstring(GetTickCount64()) +
        L".csv";

    FILE* f = nullptr;
    _wfopen_s(&f, csvPath.c_str(), L"wt, ccs=UTF-8");
    CHECK_MSG(f != nullptr, "open csv");
    if (!f) return 1;
    fwprintf(f, L"dataset_id,fs,ext,name,size_bytes,score\n");

    int rc = 0;
    // Real JPEGs may score Partial when EOI is beyond the scored prefix; Invalid must not happen.
    rc |= ScoreMediaFile((media / "xiph-poster_big.jpg").wstring(),
                         L"MEDIA-XIPH-POSTER",
                         L"jpg",
                         f,
                         FormatPayloadScore::Partial);
    rc |= ScoreMediaFile((media / "sample-5s.mp4").wstring(),
                         L"MEDIA-SAMPLELIB-MP4",
                         L"mp4",
                         f,
                         FormatPayloadScore::Partial);

    fclose(f);

    const bool keep = (GetEnvironmentVariableW(L"PHOENIX_KEEP_TEST_OUTPUT", nullptr, 0) > 0);
    if (keep) {
        fprintf(stdout, "KEEP: %ls\n", csvPath.c_str());
    } else {
        std::error_code ec;
        fs::remove(csvPath, ec);
    }

    if (g_failures > 0 || rc != 0) return 1;
    fprintf(stdout, "OK: media format quality baseline\n");
    return 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: media format quality requires volume I/O platform\n");
    return 0;
}

#endif
