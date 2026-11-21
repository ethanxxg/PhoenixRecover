// CFReDS file-carving-images: L0–L5 Graphic.dd vs official ImageLayouts + RawScan carve.
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
#include "file_carving_golden.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
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

static bool ReadImageMd5(SectorReader& reader, uint64_t offset, uint64_t size, char out33[33])
{
    if (size == 0 || size > 64u * 1024 * 1024)
        return false;
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    if (!reader.ReadBytes(offset, buf.data(), static_cast<uint32_t>(buf.size())))
        return false;
    return Md5BufferHexLower(buf.data(), buf.size(), out33);
}

struct RowResult {
    std::string image;
    std::string layout;
    std::string source;
    uint64_t    offset = 0;
    uint64_t    size = 0;
    uint64_t    embedBytes = 0;
    std::string refMd5;
    std::string embedMd5;
    bool        embedOk = false;
    bool        carveOk = false;
    uint64_t    carveOffset = 0;
    uint64_t    carveSize = 0;
    uint64_t    nodeOffset = 0;
    uint64_t    nodeSize = 0;
    std::string carveMd5;
    std::string carveLevel;
    std::string notes;
};

static void FinalizeCarveRow(RowResult& row, bool exactLayout, uint64_t embedLen,
                             const FileNode* hit, SectorReader& reader,
                             const char* refMd5)
{
    if (!row.carveOk || !hit) {
        row.carveLevel = "none";
        if (row.notes.empty())
            row.notes = row.carveOk ? "carve: internal error" : "carve: no matching node";
        return;
    }

    char nodeFullMd5[33]{};
    const bool nodeFullMd5Ok =
        hit->size <= 128u * 1024 * 1024 &&
        ReadImageMd5(reader, hit->rawOffset, hit->size, nodeFullMd5);
    const bool fullRecovery =
        embedLen >= row.size &&
        hit->rawOffset == row.offset &&
        hit->size == row.size &&
        nodeFullMd5Ok &&
        HexEqI(nodeFullMd5, refMd5);

    if (exactLayout ? row.carveOk : fullRecovery) {
        row.carveLevel = "full";
        row.notes.clear();
        return;
    }

    row.carveLevel = "partial";
    std::ostringstream n;
    n << "partial: embed=" << embedLen << "B ref=" << row.size << "B"
      << " node=" << hit->size << "B";
    if (hit->rawOffset != row.offset)
        n << " node_off=0x" << std::hex << hit->rawOffset << std::dec;
    if (hit->size != row.size) {
        const int64_t delta =
            static_cast<int64_t>(hit->size) - static_cast<int64_t>(row.size);
        if (delta < 0)
            n << " node_short=" << -delta << "B";
        else
            n << " node_long=" << delta << "B";
    }
    if (hit->size > embedLen)
        n << " node_spans_embed";
    if (embedLen < row.size)
        n << " layout_embed_only";
    row.notes = n.str();
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
                                         uint64_t embedLen)
{
    if (const FileNode* exact = FindCarveAtOffset(files, offset))
        return exact;
    const FileNode* best = nullptr;
    for (const FileNode& n : files) {
        if (n.rawOffset <= offset && offset + embedLen <= n.rawOffset + n.size) {
            if (!best || n.size < best->size)
                best = &n;
        }
    }
    return best;
}

static const FileNode* FindCarveFullMd5(SectorReader& reader,
                                        const std::vector<FileNode>& files,
                                        const char* refMd5,
                                        uint64_t hintOffset,
                                        uint64_t& outOff,
                                        uint64_t& outSize,
                                        char outCarveMd5[33])
{
    for (const FileNode& n : files) {
        if (n.size == 0 || n.size > 128u * 1024 * 1024)
            continue;
        char hex[33]{};
        if (!ReadImageMd5(reader, n.rawOffset, n.size, hex))
            continue;
        if (HexEqI(hex, refMd5)) {
            outOff = n.rawOffset;
            outSize = n.size;
            memcpy(outCarveMd5, hex, 33);
            return &n;
        }
    }
    // Prefer candidate starting at hint offset with matching prefix MD5 of source
    const FileNode* hint = FindCarveAtOffset(files, hintOffset);
    if (hint) {
        char hex[33]{};
        if (ReadImageMd5(reader, hint->rawOffset, hint->size, hex)) {
            outOff = hint->rawOffset;
            outSize = hint->size;
            memcpy(outCarveMd5, hex, 33);
        }
        return hint;
    }
    return nullptr;
}

static fs::path CsvPathWithTimestamp(const fs::path& csvPath)
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[13]{};
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min);

    const std::string stem = csvPath.stem().string();
    const std::string ext  = csvPath.has_extension() ? csvPath.extension().string() : ".csv";
    return csvPath.parent_path() / (stem + stamp + ext);
}

static fs::path WriteCsv(const fs::path& csvPath, const std::vector<RowResult>& rows)
{
    const fs::path tmpPath = csvPath.string() + ".tmp";
    std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        fprintf(stderr, "WARN: cannot write CSV temp %s\n", tmpPath.string().c_str());
        return {};
    }
    out << "image,layout,source,offset,size,embed_bytes,ref_md5,embed_md5,embed_ok,carve_ok,"
           "carve_level,carve_offset,carve_size,node_offset,node_size,carve_md5,notes\n";
    for (const RowResult& r : rows) {
        out << r.image << ',' << r.layout << ',' << r.source << ','
            << r.offset << ',' << r.size << ',' << r.embedBytes << ','
            << r.refMd5 << ',' << r.embedMd5 << ','
            << (r.embedOk ? "yes" : "no") << ',' << (r.carveOk ? "yes" : "no") << ','
            << r.carveLevel << ','
            << r.carveOffset << ',' << r.carveSize << ','
            << r.nodeOffset << ',' << r.nodeSize << ','
            << r.carveMd5 << ',' << '"' << r.notes << '"' << '\n';
    }
    out.close();

    std::error_code ec;
    fs::remove(csvPath, ec);
    ec.clear();
    fs::rename(tmpPath, csvPath, ec);
    if (!ec)
        return csvPath;

    const fs::path stamped = CsvPathWithTimestamp(csvPath);
    ec.clear();
    fs::rename(tmpPath, stamped, ec);
    if (!ec) {
        fprintf(stderr,
                "WARN: target CSV unavailable (%s), wrote timestamped %s\n",
                csvPath.string().c_str(), stamped.string().c_str());
        return stamped;
    }

    fprintf(stderr, "WARN: CSV write failed for %s and %s (%s)\n",
            csvPath.string().c_str(), stamped.string().c_str(), ec.message().c_str());
    fs::remove(tmpPath, ec);
    return {};
}

static bool RunCase(const fs::path& imagesDir,
                    const fs::path& originalsDir,
                    const FileCarveImageCase& cs,
                    std::vector<RowResult>& rows)
{
    const fs::path imagePath = imagesDir / cs.imageName;
    if (!fs::exists(imagePath)) {
        fprintf(stdout, "SKIP: %s not found\n", cs.imageName);
        return true;
    }

    fprintf(stdout, "RunCase: %s ...\n", cs.imageName);
    fflush(stdout);

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath.wstring()), cs.imageName);
    if (!reader.IsOpen())
        return false;

    Partition part{};
    part.startSector  = 0;
    part.totalSectors = reader.GetTotalSectors();
    part.sizeBytes    = reader.GetTotalBytes();
    part.fsType       = L"RAW";

    RawScan raw;
    ScanResult scan = raw.Run(reader, part);
    CHECK_MSG(scan.errorMsg.empty(), "RawScan errorMsg");
    CHECK_MSG(!scan.cancelled, "RawScan cancelled");

    fprintf(stdout, "\n=== %s (%s) RawScan found=%u ===\n",
            cs.imageName, cs.layoutTag, scan.foundCount);

    int caseFails = 0;
    for (size_t i = 0; i < cs.fileCount; ++i) {
        const FileCarveGolden& g = cs.files[i];
        RowResult row{};
        row.image  = cs.imageName;
        row.layout = cs.layoutTag;
        row.source = g.sourceName;
        row.offset = g.startSector * 512u;
        row.size   = g.sizeBytes;

        const fs::path refPath = originalsDir / g.sourceName;
        char refMd5[33]{};
        if (!fs::exists(refPath) || !Md5FileHexLower(refPath.wstring(), refMd5)) {
            row.notes = "missing original file";
            rows.push_back(row);
            ++caseFails;
            fprintf(stderr, "  FAIL embed %s: no original\n", g.sourceName);
            continue;
        }
        row.refMd5 = refMd5;

        const uint64_t embedLen = g.embedBytes ? g.embedBytes : g.sizeBytes;
        row.embedBytes = embedLen;
        const uint64_t srcOff   = g.sourceByteOffset;
        std::vector<uint8_t> refPrefix(static_cast<size_t>(embedLen));
        {
            std::ifstream rf(refPath, std::ios::binary);
            rf.seekg(static_cast<std::streamoff>(srcOff), std::ios::beg);
            rf.read(reinterpret_cast<char*>(refPrefix.data()),
                    static_cast<std::streamsize>(refPrefix.size()));
            if (static_cast<size_t>(rf.gcount()) != refPrefix.size()) {
                row.notes = "original shorter than embedBytes+sourceByteOffset";
                ++caseFails;
                rows.push_back(row);
                continue;
            }
        }
        char refPrefixMd5[33]{};
        Md5BufferHexLower(refPrefix.data(), refPrefix.size(), refPrefixMd5);

        char embedMd5[33]{};
        row.embedOk = ReadImageMd5(reader, row.offset, embedLen, embedMd5) &&
                      HexEqI(embedMd5, refPrefixMd5);
        row.embedMd5 = embedMd5;

        if (!row.embedOk) {
            row.notes = "embed MD5 mismatch vs OriginalFiles";
            ++caseFails;
            fprintf(stderr, "  FAIL embed %s offset=0x%llX ref=%s got=%s\n",
                    g.sourceName,
                    static_cast<unsigned long long>(row.offset),
                    refMd5, embedMd5);
        }

        if (g.exactLayout) {
            const FileNode* hit = FindCarveAtOffset(scan.files, row.offset);
            if (!hit) {
                row.carveOk = false;
                row.carveLevel = "none";
                row.notes += "; no carve at official offset";
                ++caseFails;
                fprintf(stderr, "  FAIL carve %s: no hit at 0x%llX\n",
                        g.sourceName, static_cast<unsigned long long>(row.offset));
            } else {
                row.carveOffset = hit->rawOffset;
                row.carveSize   = hit->size;
                row.nodeOffset  = hit->rawOffset;
                row.nodeSize    = hit->size;
                char carveMd5[33]{};
                row.carveOk = (hit->size == row.size) &&
                              ReadImageMd5(reader, hit->rawOffset, hit->size, carveMd5) &&
                              HexEqI(carveMd5, refMd5);
                row.carveMd5 = carveMd5;
                if (!row.carveOk) {
                    row.carveLevel = "none";
                    row.notes += "; carve size/md5 mismatch";
                    ++caseFails;
                    fprintf(stderr,
                            "  FAIL carve %s: off=0x%llX size=%llu/%llu md5=%s\n",
                            g.sourceName,
                            static_cast<unsigned long long>(hit->rawOffset),
                            static_cast<unsigned long long>(hit->size),
                            static_cast<unsigned long long>(row.size),
                            carveMd5);
                } else {
                    row.carveLevel = "full";
                    fprintf(stdout, "  OK carve %s exact @0x%llX size=%llu\n",
                            g.sourceName,
                            static_cast<unsigned long long>(row.offset),
                            static_cast<unsigned long long>(row.size));
                }
            }
        } else {
            const FileNode* hit = FindCarveCovering(scan.files, row.offset, embedLen);
            if (hit) {
                char carvePrefixMd5[33]{};
                row.nodeOffset  = hit->rawOffset;
                row.nodeSize    = hit->size;
                row.carveOffset = row.offset;
                row.carveSize   = embedLen;
                if (ReadImageMd5(reader, row.offset, embedLen, carvePrefixMd5) &&
                    HexEqI(carvePrefixMd5, refPrefixMd5)) {
                    row.carveOk  = true;
                    row.carveMd5 = carvePrefixMd5;
                    FinalizeCarveRow(row, false, embedLen, hit, reader, refMd5);
                    fprintf(stdout,
                            "  OK carve %s header @0x%llX level=%s node=%llu embed=%llu\n",
                            g.sourceName,
                            static_cast<unsigned long long>(row.offset),
                            row.carveLevel.c_str(),
                            static_cast<unsigned long long>(hit->size),
                            static_cast<unsigned long long>(embedLen));
                } else {
                    row.carveMd5 = carvePrefixMd5;
                    row.carveLevel = "none";
                    row.notes += "; carve prefix MD5 mismatch";
                    ++caseFails;
                }
            } else {
                row.carveLevel = "none";
                row.notes += "; fragmented: no carve at first fragment offset";
                ++caseFails;
                fprintf(stderr, "  FAIL carve %s: no hit at fragment 0x%llX\n",
                        g.sourceName, static_cast<unsigned long long>(row.offset));
            }
        }
        rows.push_back(row);
    }

    if (caseFails != 0) {
        g_failures += caseFails;
        fprintf(stderr, "%s: %d file(s) failed\n", cs.imageName, caseFails);
    } else {
        fprintf(stdout, "%s: ALL OK (%zu files)\n", cs.imageName, cs.fileCount);
    }
    return caseFails == 0;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    const fs::path root = RepoRoot();
    if (root.empty()) {
        fprintf(stderr, "FAIL: PHOENIX_SOURCE_DIR not set\n");
        return 1;
    }

    const fs::path imagesDir    = root / "testdata" / "file-carving-images" / "Images";
    const fs::path originalsDir = root / "testdata" / "file-carving-images" / "OriginalFiles" / "Graphic";

    if (!fs::exists(imagesDir / "L0_Graphic.dd")) {
        fprintf(stdout, "SKIP: file-carving-images/Images/L0_Graphic.dd not found\n");
        return 0;
    }

    std::vector<RowResult> rows;
    int imagesOk = 0;
    for (size_t i = 0; i < kGraphicCarveCaseCount; ++i) {
        if (RunCase(imagesDir, originalsDir, kGraphicCarveCases[i], rows))
            ++imagesOk;
    }

    const char* csvEnv = std::getenv("PHOENIX_CARVE_REPORT_CSV");
    fs::path csvPath;
    if (csvEnv && *csvEnv)
        csvPath = fs::path(csvEnv);
    else
        csvPath = root / "docs" / "reports" / "file_carving_graphic_results.csv";

    try {
        fs::create_directories(csvPath.parent_path());
        const fs::path written = WriteCsv(csvPath, rows);
        if (!written.empty())
            fprintf(stdout, "\nCSV: %s\n", written.string().c_str());
        else
            fprintf(stderr, "WARN: CSV not written\n");
    } catch (...) {
        fprintf(stderr, "WARN: could not write CSV\n");
    }

    fprintf(stdout, "\nSummary: %d/%zu images ALL OK, failures=%d\n",
            imagesOk, kGraphicCarveCaseCount, g_failures);

    int l0l1Fails = 0;
    int embedFails = 0;
    int carveOnlyFails = 0;
    int fullCount = 0;
    int partialCount = 0;
    for (const RowResult& r : rows) {
        if (r.carveLevel == "full")
            ++fullCount;
        else if (r.carveLevel == "partial")
            ++partialCount;
        if (!r.embedOk)
            ++embedFails;
        else if (!r.carveOk)
            ++carveOnlyFails;
        if ((r.layout == "L0_contiguous" || r.layout == "L1_frag_in_order") &&
            (!r.embedOk || !r.carveOk))
            ++l0l1Fails;
    }

    fprintf(stdout,
            "Regression gate: L0+L1 fails=%d, embed fails=%d, carve-only (L2-L5)=%d\n",
            l0l1Fails, embedFails, carveOnlyFails);
    fprintf(stdout, "Carve levels: full=%d partial=%d none=%d\n",
            fullCount, partialCount,
            static_cast<int>(rows.size()) - fullCount - partialCount);

    if (l0l1Fails != 0 || embedFails != 0 || carveOnlyFails != 0)
        return 1;

    fprintf(stdout, "OK: file-carving-images L0-L5 Graphic vs CFReDS layouts\n");
    return 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: file carving graphic test is Windows-only\n");
    return 0;
}

#endif
