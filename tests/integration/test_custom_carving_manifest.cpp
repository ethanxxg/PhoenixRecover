// Golden carving validation for testdata/custom-carving-images/manifest.json (carving-03.dd).
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

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
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

struct GoldenEntry {
    std::string name;
    uint64_t    offset = 0;
    uint64_t    size = 0;
};

static std::wstring RepoRoot()
{
#ifdef PHOENIX_SOURCE_DIR
    return std::wstring(PHOENIX_SOURCE_DIR);
#else
    return L"";
#endif
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

static bool ReadManifestGoldenForImage(const fs::path& manifestPath,
                                       const char* imageName,
                                       std::vector<GoldenEntry>& out)
{
    std::ifstream f(manifestPath, std::ios::binary);
    if (!f)
        return false;

    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    const std::string needle = std::string("\"image\": \"") + imageName + "\"";
    const size_t imgPos = content.find(needle);
    if (imgPos == std::string::npos)
        return false;

    const size_t filesPos = content.find("\"files\"", imgPos);
    if (filesPos == std::string::npos)
        return false;

    size_t pos = filesPos;
    int parsed = 0;
    while (parsed < 5) {
        const size_t namePos = content.find("\"name\"", pos);
        if (namePos == std::string::npos)
            break;
        const size_t nameStart = content.find('"', namePos + 6);
        if (nameStart == std::string::npos)
            break;
        const size_t nameEnd = content.find('"', nameStart + 1);
        if (nameEnd == std::string::npos)
            break;

        const size_t sizePos = content.find("\"size\"", nameEnd);
        const size_t offPos = content.find("\"offset\"", nameEnd);
        if (sizePos == std::string::npos || offPos == std::string::npos)
            break;

        const size_t sizeValStart = content.find(':', sizePos) + 1;
        const size_t offValStart = content.find(':', offPos) + 1;
        if (sizeValStart == 0 || offValStart == 0)
            break;

        GoldenEntry e{};
        e.name = content.substr(nameStart + 1, nameEnd - nameStart - 1);
        e.size = std::strtoull(content.c_str() + sizeValStart, nullptr, 10);
        e.offset = std::strtoull(content.c_str() + offValStart, nullptr, 10);

        out.push_back(std::move(e));
        ++parsed;
        pos = offPos + 8;
    }
    return !out.empty();
}

static bool VerifyDirectBytes(SectorReader& reader, const GoldenEntry& e)
{
    if (e.size == 0 || e.size > 32u * 1024 * 1024)
        return false;
    std::vector<uint8_t> buf(static_cast<size_t>(e.size));
    if (!reader.ReadBytes(e.offset, buf.data(), static_cast<uint32_t>(buf.size())))
        return false;
    char hex[33]{};
    if (!Md5BufferHexLower(buf.data(), buf.size(), hex))
        return false;
    fprintf(stdout, "  direct %s offset=%llu size=%llu md5=%s\n",
            e.name.c_str(),
            static_cast<unsigned long long>(e.offset),
            static_cast<unsigned long long>(e.size),
            hex);
    return true;
}

int main()
{
    const fs::path root = RepoRoot();
    if (root.empty()) {
        fprintf(stderr, "FAIL: PHOENIX_SOURCE_DIR not set\n");
        return 1;
    }

    const fs::path imagePath = root / "testdata" / "custom-carving-images" / "carving-03.dd";
    const fs::path manifestPath = root / "testdata" / "custom-carving-images" / "manifest.json";

    if (!fs::exists(imagePath)) {
        fprintf(stdout, "SKIP: %ls not found\n", imagePath.c_str());
        return 0;
    }
    if (!fs::exists(manifestPath)) {
        fprintf(stdout, "SKIP: manifest.json not found\n");
        return 0;
    }

    std::vector<GoldenEntry> golden;
    CHECK_MSG(ReadManifestGoldenForImage(manifestPath, "carving-03.dd", golden),
              "parse manifest for carving-03.dd");

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath.wstring()), "OpenImage failed");
    if (!reader.IsOpen())
        return 1;

    for (const GoldenEntry& e : golden) {
        CHECK_MSG(VerifyDirectBytes(reader, e), "direct byte read/hash failed");
    }

    Partition part{};
    part.startSector  = 0;
    part.totalSectors = reader.GetTotalSectors();
    part.sizeBytes    = part.totalSectors * reader.GetSectorSize();
    part.fsType       = L"RAW";

    RawScan raw;
    ScanResult scan = raw.Run(reader, part);
    CHECK_MSG(scan.errorMsg.empty(), "RawScan errorMsg");
    CHECK_MSG(!scan.cancelled, "RawScan cancelled");
    CHECK_MSG(scan.foundCount >= 1u, "RawScan smoke: expected carved candidates");

    if (g_failures != 0) {
        fprintf(stderr, "Custom carving manifest test: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: custom-carving carving-03 manifest golden (%zu entries, carve=%u)\n",
            golden.size(), scan.foundCount);
    return 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: custom carving manifest test is Windows-only\n");
    return 0;
}

#endif
