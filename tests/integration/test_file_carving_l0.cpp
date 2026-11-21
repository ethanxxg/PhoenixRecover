// NIST File Carving L0_Graphic.dd: verify embedded reference graphic + RawScan smoke.
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincrypt.h>

#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "scan/RawScan.h"
#include "recover/FileNode.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#pragma comment(lib, "advapi32.lib")

namespace fs = std::filesystem;

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

static bool Md5FileHexLower(const std::wstring& path, char out33[33])
{
    std::vector<uint8_t> buf(4 * 1024 * 1024);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool ok = false;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        { if (hash) CryptDestroyHash(hash); if (prov) CryptReleaseContext(prov, 0); CloseHandle(h); return false; }
    if (!CryptCreateHash(prov, CALG_MD5, 0, 0, &hash))
        { if (hash) CryptDestroyHash(hash); if (prov) CryptReleaseContext(prov, 0); CloseHandle(h); return false; }

    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) || got == 0)
            break;
        if (!CryptHashData(hash, buf.data(), got, 0))
            { if (hash) CryptDestroyHash(hash); if (prov) CryptReleaseContext(prov, 0); CloseHandle(h); return false; }
    }

    BYTE md5[16]{};
    DWORD cb = sizeof(md5);
    if (!CryptGetHashParam(hash, HP_HASHVAL, md5, &cb, 0) || cb != 16)
        { if (hash) CryptDestroyHash(hash); if (prov) CryptReleaseContext(prov, 0); CloseHandle(h); return false; }

    static const char* hexd = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        out33[i * 2]     = hexd[md5[i] >> 4];
        out33[i * 2 + 1] = hexd[md5[i] & 15];
    }
    out33[32] = '\0';
    ok = true;

    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    CloseHandle(h);
    return ok;
}

static bool Md5BufferHexLower(const uint8_t* data, size_t len, char out33[33])
{
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool ok = false;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return false;
    if (!CryptCreateHash(prov, CALG_MD5, 0, 0, &hash))
        { if (hash) CryptDestroyHash(hash); if (prov) CryptReleaseContext(prov, 0); return false; }
    size_t off = 0;
    while (off < len) {
        DWORD chunk = static_cast<DWORD>((std::min)(size_t(1u << 30), len - off));
        if (!CryptHashData(hash, data + off, chunk, 0))
            { if (hash) CryptDestroyHash(hash); if (prov) CryptReleaseContext(prov, 0); return false; }
        off += chunk;
    }
    BYTE md5[16]{};
    DWORD cb = sizeof(md5);
    if (!CryptGetHashParam(hash, HP_HASHVAL, md5, &cb, 0) || cb != 16)
        { if (hash) CryptDestroyHash(hash); if (prov) CryptReleaseContext(prov, 0); return false; }
    static const char* hexd = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) {
        out33[i * 2]     = hexd[md5[i] >> 4];
        out33[i * 2 + 1] = hexd[md5[i] & 15];
    }
    out33[32] = '\0';
    ok = true;
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
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

static bool FindEmbeddedReference(SectorReader& reader,
                                  const std::vector<uint8_t>& ref,
                                  const char* refMd5,
                                  uint64_t& outOffset)
{
    constexpr size_t kChunk = 4 * 1024 * 1024;
    const uint64_t total = reader.GetTotalBytes();
    if (ref.empty() || total < ref.size())
        return false;

    std::vector<uint8_t> window(ref.size() + kChunk);
    for (uint64_t off = 0; off + ref.size() <= total; off += kChunk - ref.size() + 1) {
        const uint32_t readLen =
            static_cast<uint32_t>((std::min)(uint64_t(window.size()), total - off));
        if (!reader.ReadBytes(off, window.data(), readLen))
            continue;
        for (uint32_t i = 0; i + ref.size() <= readLen; ++i) {
            if (memcmp(window.data() + i, ref.data(), ref.size()) != 0)
                continue;
            char hex[33]{};
            if (!Md5BufferHexLower(ref.data(), ref.size(), hex))
                return false;
            if (!HexEqI(hex, refMd5))
                continue;
            outOffset = off + i;
            return true;
        }
    }
    return false;
}

int main()
{
    const fs::path root = RepoRoot();
    if (root.empty()) {
        fprintf(stderr, "FAIL: PHOENIX_SOURCE_DIR not set\n");
        return 1;
    }

    const fs::path imagePath =
        root / "testdata" / "file-carving-images" / "Images" / "L0_Graphic.dd";
    const fs::path refPath =
        root / "testdata" / "file-carving-images" / "OriginalFiles" / "Graphic" / "09260002.jpg";

    if (!fs::exists(imagePath)) {
        fprintf(stdout,
                "SKIP: L0_Graphic.dd not found (run testdata/tools/Expand-FileCarvingImages.ps1)\n");
        return 0;
    }
    if (!fs::exists(refPath)) {
        fprintf(stdout, "SKIP: reference 09260002.jpg not found under OriginalFiles/Graphic\n");
        return 0;
    }

    std::ifstream refFile(refPath, std::ios::binary);
    std::vector<uint8_t> ref((std::istreambuf_iterator<char>(refFile)),
                             std::istreambuf_iterator<char>());

    char refMd5[33]{};
    CHECK_MSG(Md5FileHexLower(refPath.wstring(), refMd5), "reference MD5 failed");
    fprintf(stdout, "Reference 09260002.jpg md5=%s size=%zu\n", refMd5, ref.size());

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath.wstring()), "OpenImage failed");
    if (!reader.IsOpen())
        return 1;

    uint64_t embedOffset = 0;
    CHECK_MSG(FindEmbeddedReference(reader, ref, refMd5, embedOffset),
              "reference bytes not found embedded in L0_Graphic.dd");
    fprintf(stdout, "Embedded golden offset=0x%llX\n",
            static_cast<unsigned long long>(embedOffset));

    Partition part{};
    part.startSector  = 0;
    part.totalSectors = reader.GetTotalSectors();
    part.sizeBytes    = reader.GetTotalBytes();
    part.fsType       = L"RAW";

    RawScan raw;
    ScanResult result = raw.Run(reader, part);
    CHECK_MSG(result.errorMsg.empty(), "scan errorMsg");
    CHECK_MSG(!result.cancelled, "scan cancelled");
    CHECK_MSG(result.foundCount >= 1u, "RawScan smoke: expected carved candidates");
    fprintf(stdout, "RawScan smoke: foundCount=%u\n", result.foundCount);

    if (g_failures != 0) {
        fprintf(stderr, "File carving L0 test: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: file-carving L0_Graphic embedded golden + RawScan smoke\n");
    return 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: file carving L0 test is Windows-only\n");
    return 0;
}

#endif
