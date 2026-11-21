// Smoke: open every raw .dd / .raw under PHOENIX_TESTDATA_SMOKE_ROOTS (comma-separated
// relative paths under testdata/, default "dftt,cfreds,dfr-test-images,custom-carving-images,
// digital-corpora-disk-images"). Skips missing paths; does not fail on individual open errors
// unless PHOENIX_TESTDATA_SMOKE_STRICT=1.
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#include "disk/SectorReader.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <algorithm>

namespace fs = std::filesystem;

static int g_failures = 0;
static int g_opened   = 0;
static int g_skipped  = 0;

static std::wstring RepoRoot()
{
#ifdef PHOENIX_SOURCE_DIR
    return std::wstring(PHOENIX_SOURCE_DIR);
#else
    return L".";
#endif
}

static std::vector<std::wstring> SplitComma(const std::wstring& s)
{
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == L',' || c == L';') {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else if (c != L' ' && c != L'\t')
            cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

static bool IsRawImageExt(const fs::path& p)
{
    const auto ext = p.extension().wstring();
    return ext == L".dd" || ext == L".raw";
}

static void ScanDir(const fs::path& dir, std::vector<fs::path>& out)
{
    if (!fs::exists(dir)) return;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) continue;
        if (IsRawImageExt(it->path())) out.push_back(it->path());
    }
}

static bool StrictMode()
{
    wchar_t buf[8]{};
    return GetEnvironmentVariableW(L"PHOENIX_TESTDATA_SMOKE_STRICT", buf, 8) > 0;
}

int main()
{
    const fs::path repo(RepoRoot());
    const fs::path testdata = repo / "testdata";

    std::wstring rootsEnv;
    {
        wchar_t buf[2048]{};
        if (GetEnvironmentVariableW(L"PHOENIX_TESTDATA_SMOKE_ROOTS", buf, 2048) > 0)
            rootsEnv = buf;
    }
    if (rootsEnv.empty())
        rootsEnv = L"dftt,cfreds,dfr-test-images,custom-carving-images,digital-corpora-disk-images";

    std::vector<fs::path> images;
    for (const auto& rel : SplitComma(rootsEnv))
        ScanDir(testdata / rel, images);

    std::sort(images.begin(), images.end());
    images.erase(std::unique(images.begin(), images.end()), images.end());

    if (images.empty()) {
        fprintf(stdout, "SKIP: no .dd/.raw under configured smoke roots\n");
        return 0;
    }

    fprintf(stdout, "Testdata image smoke: %zu file(s)\n", images.size());

    for (const auto& img : images) {
        SectorReader reader;
        if (!reader.OpenImage(img.wstring())) {
            fprintf(stderr, "OPEN_FAIL: %ls\n", img.wstring().c_str());
            if (StrictMode()) ++g_failures;
            else ++g_skipped;
            continue;
        }
        std::vector<uint8_t> sec(512);
        if (!reader.ReadSectors(0, sec.data(), 1)) {
            fprintf(stderr, "READ_FAIL: %ls\n", img.wstring().c_str());
            if (StrictMode()) ++g_failures;
            else ++g_skipped;
            reader.Close();
            continue;
        }
        ++g_opened;
        fprintf(stdout, "OK: %ls sectors=%llu\n", img.wstring().c_str(), reader.GetTotalSectors());
        reader.Close();
    }

    fprintf(stdout, "Summary: opened=%d skipped/fail=%d strict=%d\n", g_opened, g_skipped, StrictMode() ? 1 : 0);
    return g_failures > 0 ? 1 : 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: volume I/O not enabled\n");
    return 0;
}

#endif
