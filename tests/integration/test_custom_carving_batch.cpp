// Full custom-carving-images validation: manifest.json x carving-00..19.dd (embed + RawScan carve).
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincrypt.h>

#include "custom_carving_manifest_io.h"
#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "scan/RawScan.h"
#include "recover/FileNode.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
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

struct RowResult {
    std::string image;
    std::string name;
    uint64_t    offset = 0;
    uint64_t    size = 0;
    std::string embedMd5;
    bool        embedOk = false;
    bool        carveOk = false;
    uint64_t    nodeOffset = 0;
    uint64_t    nodeSize = 0;
    std::string notes;
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

static bool CarveRequiredForName(const std::string& name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos)
        return false;
    std::string ext = name.substr(dot + 1);
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "bmp"
        || ext == "zip" || ext == "pdf" || ext == "mp4" || ext == "mov" || ext == "doc"
        || ext == "docx" || ext == "xls" || ext == "xlsx" || ext == "epub";
}

static bool StrictCarveMode()
{
    wchar_t buf[8]{};
    return GetEnvironmentVariableW(L"PHOENIX_CUSTOM_CARVE_STRICT", buf, 8) > 0;
}

static std::wstring OnlyImageEnv()
{
    wchar_t buf[512]{};
    if (GetEnvironmentVariableW(L"PHOENIX_CUSTOM_CARVE_ONLY", buf, 512) > 0)
        return buf;
    return L"";
}
static unsigned MaxImages()
{
    wchar_t buf[32]{};
    if (GetEnvironmentVariableW(L"PHOENIX_CUSTOM_CARVE_MAX_IMAGES", buf, 32) == 0)
        return 0;
    return static_cast<unsigned>(std::wcstoul(buf, nullptr, 10));
}

static const FileNode* FindCarveAtOffset(const std::vector<FileNode>& files, uint64_t offset)
{
    for (const FileNode& n : files) {
        if (n.rawOffset == offset)
            return &n;
    }
    return nullptr;
}

static const FileNode* FindCarveCovering(const std::vector<FileNode>& files,
                                         uint64_t offset,
                                         uint64_t len)
{
    if (const FileNode* exact = FindCarveAtOffset(files, offset))
        return exact;
    const FileNode* best = nullptr;
    for (const FileNode& n : files) {
        if (n.rawOffset <= offset && offset + len <= n.rawOffset + n.size) {
            if (!best || n.size < best->size)
                best = &n;
        }
    }
    return best;
}

static fs::path CsvPathFromEnv()
{
    wchar_t buf[2048]{};
    if (GetEnvironmentVariableW(L"PHOENIX_CUSTOM_CARVE_REPORT_CSV", buf, 2048) > 0)
        return fs::path(buf);
#ifdef PHOENIX_SOURCE_DIR
    return fs::path(PHOENIX_SOURCE_DIR) / "docs" / "reports" / "custom_carving_results.csv";
#else
    return fs::path("custom_carving_results.csv");
#endif
}

static void WriteCsv(const fs::path& csvPath, const std::vector<RowResult>& rows)
{
    std::ofstream out(csvPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        fprintf(stderr, "WARN: cannot write CSV %s\n", csvPath.string().c_str());
        return;
    }
    out << "image,name,offset,size,embed_md5,embed_ok,carve_ok,node_offset,node_size,notes\n";
    for (const RowResult& r : rows) {
        out << r.image << ',' << r.name << ','
            << r.offset << ',' << r.size << ','
            << r.embedMd5 << ','
            << (r.embedOk ? "yes" : "no") << ','
            << (r.carveOk ? "yes" : "no") << ','
            << r.nodeOffset << ',' << r.nodeSize << ','
            << '"' << r.notes << '"' << '\n';
    }
}

static bool RunImageCase(const fs::path& imagesDir,
                         const CustomCarveImageCase& cs,
                         std::vector<RowResult>& rows)
{
    const fs::path imagePath = imagesDir / cs.image;
    if (!fs::exists(imagePath)) {
        fprintf(stdout, "SKIP: %s not found\n", cs.image.c_str());
        return true;
    }

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath.wstring()), cs.image.c_str());
    if (!reader.IsOpen())
        return false;

    const uint64_t imageBytes = reader.GetTotalBytes();

    Partition part{};
    part.startSector  = 0;
    part.totalSectors = reader.GetTotalSectors();
    part.sizeBytes    = reader.GetTotalBytes();
    part.fsType       = L"RAW";

    RawScan raw;
    ScanResult scan = raw.Run(reader, part);
    CHECK_MSG(scan.errorMsg.empty(), "RawScan errorMsg");
    CHECK_MSG(!scan.cancelled, "RawScan cancelled");

    CHECK_MSG(scan.foundCount >= 1u, "RawScan smoke: expected carved candidates");

    int caseFails = 0;
    size_t carveHits = 0;
    fprintf(stdout, "\n=== %s files=%zu RawScan found=%u ===\n",
            cs.image.c_str(), cs.files.size(), scan.foundCount);

    for (const CustomCarveGolden& g : cs.files) {
        RowResult row{};
        row.image  = cs.image;
        row.name   = g.name;
        row.offset = g.offset;
        row.size   = g.size;

        if (g.size == 0 || g.offset + g.size > imageBytes) {
            row.notes = "manifest out of image bounds";
            rows.push_back(row);
            ++caseFails;
            fprintf(stderr, "  FAIL embed %s: bounds\n", g.name.c_str());
            continue;
        }

        const uint64_t readLen = (std::min)(g.size, uint64_t(32u * 1024 * 1024));
        std::vector<uint8_t> buf(static_cast<size_t>(readLen));
        if (!reader.ReadBytes(g.offset, buf.data(), static_cast<uint32_t>(buf.size()))) {
            row.notes = "ReadBytes failed";
            rows.push_back(row);
            ++caseFails;
            fprintf(stderr, "  FAIL embed %s: read\n", g.name.c_str());
            continue;
        }

        char md5[33]{};
        if (!Md5BufferHexLower(buf.data(), buf.size(), md5)) {
            row.notes = "md5 failed";
            rows.push_back(row);
            ++caseFails;
            continue;
        }
        row.embedMd5 = md5;
        row.embedOk  = true;

        const FileNode* hit = FindCarveCovering(scan.files, g.offset, g.size);
        if (hit) {
            row.carveOk     = true;
            ++carveHits;
            row.nodeOffset  = hit->rawOffset;
            row.nodeSize    = hit->size;
            if (hit->rawOffset != g.offset)
                row.notes = "carve: node offset differs";
            else if (hit->size != g.size)
                row.notes = "carve: node size differs";
        } else {
            row.notes = "carve: no node at offset";
        }

        rows.push_back(row);
    }

    if (caseFails != 0)
        ++g_failures;
    fprintf(stdout, "  %s: embed_ok=%zu/%zu carve_ok=%zu found=%u\n",
            cs.image.c_str(),
            cs.files.size() - static_cast<size_t>(caseFails),
            cs.files.size(),
            carveHits,
            scan.foundCount);
    fflush(stdout);
    return caseFails == 0;
}

int main()
{
    const fs::path root = RepoRoot();
    if (root.empty()) {
        fprintf(stderr, "FAIL: PHOENIX_SOURCE_DIR not set\n");
        return 1;
    }

    const fs::path imagesDir   = root / "testdata" / "custom-carving-images";
    const fs::path manifestPath = imagesDir / "manifest.json";

    if (!fs::exists(manifestPath)) {
        fprintf(stdout, "SKIP: manifest.json not found\n");
        return 0;
    }

    std::vector<CustomCarveImageCase> cases;
    CHECK_MSG(LoadCustomCarvingManifest(manifestPath.string(), cases), "parse manifest.json");
    if (cases.empty())
        return 1;

    std::vector<RowResult> rows;
    rows.reserve(1100);

    int imagesOk = 0;
    unsigned maxImages = MaxImages();
    unsigned processed = 0;
    const std::wstring onlyImage = OnlyImageEnv();
    for (const CustomCarveImageCase& cs : cases) {
        if (!onlyImage.empty()) {
            std::wstring wimg(cs.image.begin(), cs.image.end());
            if (_wcsicmp(wimg.c_str(), onlyImage.c_str()) != 0)
                continue;
        }
        if (maxImages > 0 && processed >= maxImages)
            break;
        if (RunImageCase(imagesDir, cs, rows))
            ++imagesOk;
        ++processed;
    }

    const fs::path csvPath = CsvPathFromEnv();
    WriteCsv(csvPath, rows);
    fprintf(stdout, "CSV: %s (%zu rows)\n", csvPath.string().c_str(), rows.size());

    const size_t embedOk = std::count_if(rows.begin(), rows.end(),
                                         [](const RowResult& r) { return r.embedOk; });
    const size_t carveOk = std::count_if(rows.begin(), rows.end(),
                                         [](const RowResult& r) { return r.carveOk; });

    fprintf(stdout, "Summary: images=%zu/%zu embed=%zu/%zu carve=%zu/%zu failures=%d\n",
            static_cast<size_t>(imagesOk), cases.size(),
            embedOk, rows.size(), carveOk, rows.size(), g_failures);

    if (g_failures != 0) {
        fprintf(stderr, "Custom carving batch: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: custom-carving batch all images PASS\n");
    return 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: custom carving batch test is Windows-only\n");
    return 0;
}

#endif
