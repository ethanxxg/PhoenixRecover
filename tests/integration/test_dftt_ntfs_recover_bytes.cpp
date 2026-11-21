// DFTT #7 NTFS Undelete - recover deleted files and verify MD5 (RCV-P0-02).
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincrypt.h>

#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "fs/ntfs/NtfsParser.h"
#include "recover/FileNode.h"
#include "recover/RecoverEngine.h"

#include <cctype>
#include <cwchar>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

static int g_failures = 0;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
            ++g_failures; \
        } \
    } while (0)

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
    namespace fs = std::filesystem;
    return (fs::path(PHOENIX_SOURCE_DIR) / "testdata" / "dftt" / "7-undel-ntfs" / "7-ntfs-undel.dd")
        .wstring();
#else
    return L"";
#endif
}

static const char kHexDigits[] = "0123456789abcdef";

static bool Md5FileHexLower(const std::wstring& path, char out33[33])
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool ok = false;
    std::vector<uint8_t> buf(256 * 1024);
    BYTE md5[16]{};
    DWORD cb = sizeof(md5);

    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        goto done;
    if (!CryptCreateHash(prov, CALG_MD5, 0, 0, &hash))
        goto done;

    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) || got == 0)
            break;
        if (!CryptHashData(hash, buf.data(), got, 0))
            goto done;
    }

    cb = sizeof(md5);
    if (!CryptGetHashParam(hash, HP_HASHVAL, md5, &cb, 0) || cb != 16)
        goto done;

    for (int i = 0; i < 16; ++i) {
        out33[i * 2]     = kHexDigits[md5[i] >> 4];
        out33[i * 2 + 1] = kHexDigits[md5[i] & 15];
    }
    out33[32] = '\0';
    ok = true;

done:
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    CloseHandle(h);
    return ok;
}

static bool HexEqI(const char* a, const char* b)
{
    for (; *a && *b; ++a, ++b) {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b)))
            return false;
    }
    return *a == *b;
}

static bool IsFileTime2004LeapDay(uint64_t ft)
{
    if (ft == 0)
        return false;
    FILETIME f{};
    f.dwLowDateTime  = static_cast<DWORD>(ft);
    f.dwHighDateTime = static_cast<DWORD>(ft >> 32);
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&f, &st))
        return false;
    return st.wYear == 2004 && st.wMonth == 2 && st.wDay == 29;
}

static const FileNode* FindByName(const std::vector<FileNode>& files, const wchar_t* name)
{
    for (const auto& f : files) {
        if (_wcsicmp(f.name.c_str(), name) == 0)
            return &f;
    }
    return nullptr;
}

static const FileNode* FindByPathSuffix(const std::vector<FileNode>& files,
                                        const wchar_t* pathSuffix)
{
    for (const auto& f : files) {
        if (f.originalPath.size() >= wcslen(pathSuffix)) {
            const wchar_t* tail =
                f.originalPath.c_str() + f.originalPath.size() - wcslen(pathSuffix);
            if (_wcsicmp(tail, pathSuffix) == 0)
                return &f;
        }
    }
    return nullptr;
}

static std::wstring MakeTempRecoverDir()
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"PhoenixRecover_dftt7_" +
                       std::to_wstring(GetTickCount64());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static std::wstring GetEnvRecoverOutDir()
{
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"PHOENIX_RECOVER_OUTDIR", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(buf);
}

static bool IsExternalOutDir(const std::wstring& dir)
{
    if (dir.empty()) return false;
    // If caller provides a fixed out dir, don't delete it.
    return !GetEnvRecoverOutDir().empty();
}

static void RemoveTree(const std::wstring& root)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(fs::path(root), ec);
}

int main()
{
    std::wstring imagePath = ImagePath();
    if (imagePath.empty() || !std::filesystem::exists(imagePath)) {
        fprintf(stdout, "SKIP: DFTT 7-ntfs-undel.dd not found at %ls\n", imagePath.c_str());
        return 0;
    }

    struct Golden {
        const wchar_t* name;       // for logging
        const wchar_t* pathSuffix; // match originalPath tail; nullptr -> FindByName(name)
        const char*    md5;
        bool           required;
    };
    static const Golden kGold[] = {
        {L"res1.dat",  nullptr,              "9036637712b491904cd0bfbdbe648453", true},
        {L"sing1.dat", nullptr,              "59b20779f69ff9f0ac5fcd2c38835a79", true},
        {L"mult1.dat", nullptr,              "ffd27bd782bdce67750b6b9ee069d2ef", true},
        {L"mult1.dat:ADS", nullptr,          "ba1b9eedb1c091ddca253d35dde8f616", true},
        {L"frag1.dat", nullptr,              "7a3bc5b763bef201202108f4ba128149", true},
        {L"frag2.dat", nullptr,              "0e80ab84ef0087e60dfc67b88a1cf13e", true},
        {L"mult2.dat", L"\\dir1\\mult2.dat", "59cf0e9cd107bc1e75afb7374f6e05bb", true},
        {L"frag3.dat", L"\\dir1\\dir2\\frag3.dat",
         "21121699487f3fbbdb9a4b3391b6d3e0", true},
        {L"sing2.dat", L"\\dir3\\sing2.dat", "c229626f6a71b167ad7e50c4f2fccdb1", true},
    };

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");
    if (!reader.IsOpen())
        return 1;

    Partition part{};
    part.startSector  = 0;
    part.totalSectors = reader.GetTotalSectors();
    part.sizeBytes    = reader.GetTotalBytes();
    part.fsType       = L"NTFS";

    NtfsParser parser;
    CHECK_MSG(parser.Open(reader, part), "NtfsParser::Open failed");
    CHECK_MSG(parser.ScanMetadata(), "NtfsParser::ScanMetadata failed");

    auto files = parser.GetAllFiles();
    std::wstring outDir = GetEnvRecoverOutDir();
    if (outDir.empty()) {
        outDir = MakeTempRecoverDir();
    } else {
        CreateDirectoryW(outDir.c_str(), nullptr);
        fprintf(stdout, "NOTE: using PHOENIX_RECOVER_OUTDIR=%ls\n", outDir.c_str());
    }
    RecoverEngine engine;

    int requiredOk = 0;
    int optionalOk = 0;
    constexpr int kRequiredCount =
        static_cast<int>(sizeof(kGold) / sizeof(kGold[0]));

    for (const Golden& g : kGold) {
        const FileNode* node = nullptr;
        if (g.pathSuffix)
            node = FindByPathSuffix(files, g.pathSuffix);
        if (!node)
            node = FindByName(files, g.name);

        if (g.required)
            CHECK_MSG(node != nullptr, "missing deleted node in scan");
        if (!node) {
            if (g.required)
                fprintf(stderr, "missing %ls\n", g.name);
            continue;
        }

        FileRecoverResult rr =
            engine.RecoverFile(*node, outDir, false, reader, part);
        if (g.required)
            CHECK_MSG(rr.status == RecoverStatus::Success, "RecoverFile failed");
        if (rr.status != RecoverStatus::Success)
            continue;

        char hex[33]{};
        if (!Md5FileHexLower(rr.targetPath, hex)) {
            if (g.required)
                CHECK_MSG(false, "Md5FileHexLower failed");
            continue;
        }
        if (HexEqI(hex, g.md5)) {
            fprintf(stdout, "OK %ls md5=%s\n", g.name, hex);
            if (g.required)
                ++requiredOk;
            else
                ++optionalOk;
        } else if (g.required) {
            fprintf(stderr, "MD5 mismatch %ls: got %s expected %s\n", g.name, hex, g.md5);
            ++g_failures;
        } else {
            fprintf(stdout, "WARN optional %ls MD5 mismatch (fragment/subdir)\n", g.name);
        }
    }

    CHECK_MSG(requiredOk == kRequiredCount,
              "all DFTT #7 golden files must recover with expected MD5");

    bool sawLeapDay = false;
    for (const FileNode& n : files) {
        if (IsFileTime2004LeapDay(n.modifyTime) || IsFileTime2004LeapDay(n.createTime)) {
            sawLeapDay = true;
            break;
        }
    }
    CHECK_MSG(sawLeapDay, "DFTT #7 image timestamps must include 2004-02-29");

    if (!IsExternalOutDir(outDir)) {
        RemoveTree(outDir);
    } else {
        fprintf(stdout, "NOTE: keeping recovered outputs at %ls\n", outDir.c_str());
    }
    reader.Close();

    if (g_failures != 0) {
        fprintf(stderr, "DFTT NTFS recover bytes: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: DFTT #7 NTFS recover bytes (required=%d optional=%d)\n",
            requiredOk, optionalOk);
    return 0;
}

#else

int main()
{
    fprintf(stdout, "SKIP: DFTT NTFS recover bytes test is Windows-only\n");
    return 0;
}

#endif
