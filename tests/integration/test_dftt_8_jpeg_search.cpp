// DFTT Test Image #8 - JPEG Search #1 (NTFS partition, keyword/carve checklist).
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#include "dftt_golden.h"
#include "dftt_test_md5.h"
#include "scan/EmbeddedJpegExtract.h"
#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "fs/ntfs/NtfsParser.h"
#include "recover/FileNode.h"
#include "recover/RecoverEngine.h"
#include "scan/RawScan.h"

#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_embedRequired = 0;
static int g_embedOk = 0;

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
    return (fs::path(PHOENIX_SOURCE_DIR) / "testdata" / "dftt" / "8-jpeg-search" / "8-jpeg-search.dd")
        .wstring();
#else
    return L"";
#endif
}

static std::wstring TempDir()
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + L"PhoenixRecover_dftt8_" + std::to_wstring(GetTickCount64());
}

static bool Md5NodeViaRecover(SectorReader& reader, const Partition& part, const FileNode& node,
                              const std::wstring& outDir, char out33[33])
{
    RecoverEngine engine;
    FileRecoverResult rr = engine.RecoverFile(node, outDir, false, reader, part);
    if (rr.status != RecoverStatus::Success)
        return false;
    return DfttMd5FileHexLower(rr.targetPath.c_str(), out33);
}

static void CollectFsMd5(SectorReader& reader, const Partition& part,
                         const std::vector<FileNode>& files,
                         const std::wstring& outDir,
                         std::set<std::string>& outMd5)
{
    for (const FileNode& node : files) {
        if (node.isDirectory || node.size == 0 || node.size > 32u * 1024u * 1024u)
            continue;
        char hex[33]{};
        if (!Md5NodeViaRecover(reader, part, node, outDir, hex))
            continue;
        outMd5.insert(hex);
    }
}

static void CollectEmbeddedMd5(SectorReader& reader, const Partition& part,
                               const std::vector<FileNode>& files,
                               const std::wstring& outDir,
                               std::set<std::string>& outMd5)
{
    for (const FileNode& node : files) {
        if (node.isDirectory || node.size == 0 || node.size > 32u * 1024u * 1024u)
            continue;
        RecoverEngine engine;
        FileRecoverResult rr = engine.RecoverFile(node, outDir, false, reader, part);
        if (rr.status != RecoverStatus::Success)
            continue;
        std::vector<uint8_t> buf(static_cast<size_t>(node.size));
        FILE* fp = _wfopen(rr.targetPath.c_str(), L"rb");
        if (!fp)
            continue;
        const size_t got = fread(buf.data(), 1, buf.size(), fp);
        fclose(fp);
        if (got == 0)
            continue;
        std::vector<EmbeddedJpegSlice> slices;
        EmbeddedJpegExtract::CollectFromBuffer(buf.data(), got, slices);
        for (const EmbeddedJpegSlice& slice : slices) {
            char hex[33]{};
            if (DfttMd5BufferHexLower(slice.bytes.data(), slice.bytes.size(), hex))
                outMd5.insert(hex);
        }
    }
}

static void CollectCarveJpegMd5(SectorReader& reader, const ScanResult& scan,
                                std::set<std::string>& outMd5)
{
    for (const FileNode& f : scan.files) {
        std::wstring ext = f.extension;
        for (wchar_t& c : ext)
            c = static_cast<wchar_t>(towlower(c));
        if (ext != L"jpg" && ext != L"jpeg")
            continue;
        if (f.size == 0 || f.size > 16u * 1024u * 1024u)
            continue;
        std::vector<uint8_t> buf(static_cast<size_t>(f.size));
        if (!reader.ReadBytes(f.rawOffset, buf.data(), static_cast<uint32_t>(buf.size())))
            continue;
        char hex[33]{};
        if (!DfttMd5BufferHexLower(buf.data(), buf.size(), hex))
            continue;
        outMd5.insert(hex);
    }
}

int main()
{
    std::wstring imagePath = ImagePath();
    if (imagePath.empty() || !std::filesystem::exists(imagePath)) {
        fprintf(stdout, "SKIP: DFTT 8-jpeg-search.dd not found at %ls\n", imagePath.c_str());
        return 0;
    }

    char imgMd5[33]{};
    CHECK_MSG(DfttMd5FileHexLower(imagePath.c_str(), imgMd5), "image md5");
    CHECK_MSG(DfttHexEqI(imgMd5, "9bdb9c76b80e90d155806a1fc7846db5"), "unexpected image MD5");

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage");
    if (!reader.IsOpen())
        return 1;

    Partition part{};
    part.startSector  = 0;
    part.totalSectors = reader.GetTotalSectors();
    part.sizeBytes    = part.totalSectors * reader.GetSectorSize();
    part.fsType       = L"NTFS";

    NtfsParser parser;
    CHECK_MSG(parser.Open(reader, part), "NtfsParser::Open");
    CHECK_MSG(parser.ScanMetadata(), "NtfsParser::ScanMetadata");
    const auto fsFiles = parser.GetAllFiles();

    RawScan raw;
    ScanResult carved = raw.Run(reader, part);
    CHECK_MSG(carved.errorMsg.empty(), "RawScan error");
    CHECK_MSG(!carved.cancelled, "RawScan cancelled");

    const std::wstring outDir = TempDir();
    CreateDirectoryW(outDir.c_str(), nullptr);

    std::set<std::string> fsMd5;
    std::set<std::string> carveMd5;
    std::set<std::string> embedMd5;
    CollectFsMd5(reader, part, fsFiles, outDir, fsMd5);
    CollectEmbeddedMd5(reader, part, fsFiles, outDir, embedMd5);
    CollectCarveJpegMd5(reader, carved, carveMd5);

    int fsRequired = 0;
    int fsOk = 0;
    int carveOk = 0;
    int forbidViolations = 0;

    for (const DfttJpegGolden& g : kDftt8JpegGold) {
        switch (g.expect) {
        case DfttJpegExpect::RequiredFs:
            ++fsRequired;
            if (fsMd5.count(g.md5)) {
                ++fsOk;
                fprintf(stdout, "  OK FS %s\n", g.label);
            } else {
                fprintf(stderr, "FAIL: FS missing %s md5=%s\n", g.label, g.md5);
                ++g_failures;
            }
            if (carveMd5.count(g.md5)) {
                ++carveOk;
                fprintf(stdout, "  OK carve %s\n", g.label);
            } else {
                fprintf(stdout, "  WARN: carve miss %s (FS ok)\n", g.label);
            }
            break;
        case DfttJpegExpect::ForbidCarve:
            if (carveMd5.count(g.md5)) {
                fprintf(stderr, "FAIL: forbid carve hit %s md5=%s\n", g.label, g.md5);
                ++forbidViolations;
                ++g_failures;
            } else {
                fprintf(stdout, "  OK forbid carve %s\n", g.label);
            }
            break;
        case DfttJpegExpect::RequiredEmbed:
        case DfttJpegExpect::OptionalEmbed:
            ++g_embedRequired;
            if (fsMd5.count(g.md5) || carveMd5.count(g.md5) || embedMd5.count(g.md5)) {
                ++g_embedOk;
                fprintf(stdout, "  OK embed %s\n", g.label);
            } else if (g.expect == DfttJpegExpect::RequiredEmbed) {
                fprintf(stderr, "FAIL: embed missing %s md5=%s\n", g.label, g.md5);
                ++g_failures;
            } else {
                fprintf(stdout, "  WARN optional miss %s\n", g.label);
            }
            break;
        default:
            break;
        }
    }

    CHECK_MSG(fsOk == fsRequired, "required NTFS JPEG files must recover with golden MD5");
    CHECK_MSG(forbidViolations == 0, "invalid decoy files must not carve as JPEG");
    CHECK_MSG(g_embedOk == g_embedRequired, "embedded/ADS JPEG scenarios must be found");

    std::error_code ec;
    std::filesystem::remove_all(outDir, ec);
    reader.Close();

    if (g_failures != 0) {
        fprintf(stderr, "DFTT #8 JPEG search: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout,
            "OK: DFTT #8 JPEG search FS=%d/%d carve_hits=%d embed=%d/%d\n",
            fsOk, fsRequired, carveOk, g_embedOk, g_embedRequired);
    return 0;
}

#else

int main()
{
    fprintf(stdout, "SKIP: DFTT #8 JPEG search test is Windows-only\n");
    return 0;
}

#endif
