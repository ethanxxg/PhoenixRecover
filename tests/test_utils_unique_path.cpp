#include "common/Utils.h"

#include <filesystem>
#include <fstream>
#include <string>

#define CHECK(cond) \
    do {            \
        if (!(cond)) return 1; \
    } while (0)

static std::wstring TempDir()
{
    std::wstring d = Utils::GetTempDirectoryW();
    if (d.empty()) return L".";
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) {
        d.pop_back();
    }
    return d.empty() ? L"." : d;
}

static void TouchFile(const std::wstring& path)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream f(std::filesystem::path(path), std::ios::binary);
    (void)f;
}

static std::filesystem::path P(const std::wstring& s)
{
    return std::filesystem::path(s).make_preferred();
}

int main()
{
    const std::wstring dir = TempDir() + L"/PhoenixRecover_test_unique";
    Utils::CreateDirectoryRecursive(dir);

    const std::wstring a = dir + L"/a.txt";
    const std::wstring b = dir + L"/a (1).txt";
    const std::wstring c = dir + L"/a (2).txt";

    // Clean slate (best-effort)
    {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(c), ec);
        std::filesystem::remove(std::filesystem::path(b), ec);
        std::filesystem::remove(std::filesystem::path(a), ec);
    }

    // When no conflict, keep path.
    CHECK(P(Utils::MakeUniqueFilePath(a)) == P(a));

    // After creating a.txt, next should be a (1).txt
    TouchFile(a);
    CHECK(Utils::FileExists(a));
    CHECK(P(Utils::MakeUniqueFilePath(a)) == P(b));

    // If a (1).txt also exists, next should be a (2).txt
    TouchFile(b);
    CHECK(Utils::FileExists(b));
    CHECK(P(Utils::MakeUniqueFilePath(a)) == P(c));

    return 0;
}

