// ReFS recovery-byte gate: engine -> product path, byte-exact.
//
// fs/refs/RefsVolume.h (A1-5a) parsed ReFS correctly but had no route into the
// product: no FileSystem implementation, no probe entry, no cluster list for
// RecoverEngine.  This gate locks the new path end to end:
//
//   SectorReader::OpenImage
//     -> FileSystemModuleService probe chain (empty fsType)
//     -> RefsParser (FileSystem adapter over RefsVolume)
//     -> RecoverEngine::RecoverFile
//     -> sha256 vs testdata/refs-golden/files.tsv (independent truth table)
//
// Golden corpus: testdata/refs-golden/, produced by the ReFS engine's own
// fixture generator.  files.tsv is the independent truth: per-file name,
// size, dataKind (inline / extent) and sha256, keyed by case.  Two cases are
// exercised here so both checksum types and both data paths are covered:
//
//   refs_v314_crc64_basic  CRC64/NVME, 6 entries, one nested directory
//   refs_v314_sha256       SHA-256 volume checksums
//
// Also asserts the scan-source read-only invariant (whole-image FNV-1a).

#include "common/Platform.h"

#if !PHOENIX_OS_WINDOWS
int main()
{
    std::printf("SKIP: RefsRecoverBytes requires the Windows FS code path\n");
    return 0;
}
#else

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincrypt.h>

#include "common/Utils.h"
#include "disk/Partition.h"
#include "disk/SectorReader.h"
#include "fs/FileSystem.h"
#include "fs/refs/RefsParser.h"
#include "recover/FileNode.h"
#include "recover/RecoverEngine.h"
#include "scan/FileSystemModuleService.h"
#include "scan/ScanEngine.h"

#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(expr, msg)                                                       \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(expr)) {                                                         \
            ++g_fails;                                                         \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); \
        }                                                                      \
    } while (0)

namespace {

struct Expected
{
    std::string name;
    std::string sha256;
    std::string kind;      // inline | extent | none
    long long   size = 0;
    bool        isDir = false;
};

std::wstring GoldenDir()
{
    return std::wstring(PHOENIX_SOURCE_DIR) + L"/testdata/refs-golden";
}

std::vector<std::string> SplitTabs(const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '\t') { out.push_back(cur); cur.clear(); }
        else if (c != '\r') cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

bool LoadExpectations(const std::string& caseName, std::vector<Expected>& out,
                      const std::string& table = "files.tsv")
{
    const std::wstring path = GoldenDir() + L"/" + Utils::Utf8ToWide(table);
    std::ifstream f(Utils::WideToUtf8(path));
    if (!f) return false;

    std::string line;
    if (!std::getline(f, line)) return false;
    const std::vector<std::string> hdr = SplitTabs(line);
    int iCase = -1, iName = -1, iDir = -1, iSize = -1, iKind = -1, iSha = -1;
    for (size_t i = 0; i < hdr.size(); ++i) {
        if (hdr[i] == "case") iCase = static_cast<int>(i);
        else if (hdr[i] == "name") iName = static_cast<int>(i);
        else if (hdr[i] == "isDir") iDir = static_cast<int>(i);
        else if (hdr[i] == "size") iSize = static_cast<int>(i);
        else if (hdr[i] == "dataKind") iKind = static_cast<int>(i);
        else if (hdr[i] == "sha256") iSha = static_cast<int>(i);
    }
    if (iCase < 0 || iName < 0 || iSize < 0 || iSha < 0) return false;

    while (std::getline(f, line)) {
        const std::vector<std::string> c = SplitTabs(line);
        if (static_cast<int>(c.size()) <= std::max(std::max(iCase, iName), std::max(iSize, iSha)))
            continue;
        if (c[iCase] != caseName) continue;
        Expected e;
        e.name = c[iName];
        e.size = std::atoll(c[iSize].c_str());
        e.sha256 = c[iSha];
        if (e.sha256 == "-") e.sha256.clear();
        if (iKind >= 0 && iKind < static_cast<int>(c.size())) e.kind = c[iKind];
        if (iDir >= 0 && iDir < static_cast<int>(c.size())) e.isDir = (c[iDir] == "1");
        out.push_back(e);
    }
    return !out.empty();
}

uint64_t Fnv1aFile(const std::wstring& path)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return 0;
    uint64_t h = 1469598103934665603ULL;
    std::vector<uint8_t> buf(1u << 20);
    size_t n = 0;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) {
        for (size_t i = 0; i < n; ++i) { h ^= buf[i]; h *= 1099511628211ULL; }
    }
    fclose(f);
    return h;
}

bool Sha256OfFile(const std::wstring& path, char hex[65], long long& sizeOut)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool ok = false;
    sizeOut = 0;
    if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash))
    {
        std::vector<uint8_t> buf(1u << 20);
        ok = true;
        size_t n = 0;
        while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) {
            sizeOut += static_cast<long long>(n);
            if (!CryptHashData(hash, buf.data(), static_cast<DWORD>(n), 0)) { ok = false; break; }
        }
        if (ok) {
            BYTE digest[32]{};
            DWORD cb = sizeof(digest);
            if (CryptGetHashParam(hash, HP_HASHVAL, digest, &cb, 0) && cb == 32) {
                static const char hexd[] = "0123456789abcdef";
                for (int i = 0; i < 32; ++i) {
                    hex[i * 2]     = hexd[digest[i] >> 4];
                    hex[i * 2 + 1] = hexd[digest[i] & 15];
                }
                hex[64] = '\0';
            } else {
                ok = false;
            }
        }
    }
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    fclose(f);
    return ok;
}

std::wstring MakeOutDir(const std::string& caseName)
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring tag = Utils::Utf8ToWide(caseName);
    std::wstring dir = std::wstring(tmp) + L"phx_refs_" + tag + L"_" +
                       std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void RunCase(const std::string& caseName)
{
    std::printf("-- %s\n", caseName.c_str());

    std::vector<Expected> expected;
    if (!LoadExpectations(caseName, expected)) {
        std::printf("SKIP %s: files.tsv has no rows\n", caseName.c_str());
        ++g_checks;
        ++g_fails;
        return;
    }

    const std::wstring image = GoldenDir() + L"/" +
                               Utils::Utf8ToWide(caseName) + L".bin";
    const uint64_t before = Fnv1aFile(image);
    CHECK(before != 0, "golden ReFS image is readable");

    SectorReader reader;
    CHECK(reader.OpenImage(image), "SectorReader opens the ReFS image");
    if (!reader.IsOpen()) return;

    const uint32_t ss = reader.GetSectorSize();
    Partition part;
    part.startSector  = 0;
    part.totalSectors = 0;
    {
        FILE* f = _wfopen(image.c_str(), L"rb");
        if (f) {
            _fseeki64(f, 0, SEEK_END);
            const long long n = _ftelli64(f);
            fclose(f);
            part.totalSectors = static_cast<uint64_t>(n) / (ss ? ss : 512);
        }
    }
    part.sizeBytes = part.totalSectors * (ss ? ss : 512);
    part.fsType    = L"";                    // force the production probe chain

    ScanResult scanResult;
    std::unique_ptr<FileSystem> parser =
        FileSystemModuleService::CreateParserForPartition(reader, part, scanResult);
    CHECK(parser != nullptr, "probe chain recognises the ReFS volume");
    if (!parser) {
        std::printf("   probe error=%ls\n", scanResult.errorMsg.c_str());
        reader.Close();
        return;
    }
    CHECK(dynamic_cast<RefsParser*>(parser.get()) != nullptr,
          "probe chain resolves to RefsParser with empty fsType");
    CHECK(parser->Open(reader, part), "RefsParser::Open succeeds");
    CHECK(parser->ScanMetadata(), "RefsParser::ScanMetadata succeeds");

    const std::vector<FileNode> nodes = parser->GetAllFiles();
    std::printf("   enumerated %zu nodes for %zu expected entries\n",
                nodes.size(), expected.size());
    CHECK(nodes.size() >= expected.size(),
          "every expected entry was enumerated (nested directory included)");

    const std::wstring outDir = MakeOutDir(caseName);
    RecoverEngine engine;

    for (const Expected& e : expected) {
        const std::wstring wname = Utils::Utf8ToWide(e.name);
        const FileNode* node = nullptr;
        for (const FileNode& n : nodes) {
            if (n.name == wname) { node = &n; break; }
        }
        std::printf("   %-14s ", e.name.c_str());
        if (!node) {
            std::printf("MISSING\n");
            ++g_checks; ++g_fails;
            std::fprintf(stderr, "FAIL: node not found: %s\n", e.name.c_str());
            continue;
        }
        CHECK(node->isDirectory == e.isDir, "directory flag matches files.tsv");
        if (e.isDir) { std::printf("dir ok\n"); continue; }

        CHECK(static_cast<long long>(node->size) == e.size,
              "node size matches files.tsv");
        CHECK(node->recoverability != RecoverabilityLevel::Lost,
              "node is not reported as unrecoverable");

        const FileRecoverResult rr = engine.RecoverFile(*node, outDir, false, reader, part);
        if (rr.status != RecoverStatus::Success || rr.targetPath.empty()) {
            std::printf("RECOVER FAILED\n");
            ++g_checks; ++g_fails;
            std::fprintf(stderr, "FAIL: RecoverEngine failed for %s\n", e.name.c_str());
            continue;
        }
        char      hex[65]{};
        long long got = 0;
        const bool hashed = Sha256OfFile(rr.targetPath, hex, got);
        CHECK(hashed, "recovered output is readable");
        CHECK(got == e.size, "recovered byte count equals the logical size");
        if (hashed && !e.sha256.empty()) {
            const bool same = (std::strcmp(hex, e.sha256.c_str()) == 0);
            ++g_checks;
            if (!same) {
                ++g_fails;
                std::fprintf(stderr, "FAIL: sha256 mismatch for %s\n  got  %s\n  want %s\n",
                             e.name.c_str(), hex, e.sha256.c_str());
            }
            std::printf("%s %s\n", same ? "bytes ok" : "BYTES WRONG", e.kind.c_str());
        } else {
            std::printf("size ok\n");
        }
        DeleteFileW(rr.targetPath.c_str());
    }

    // The nested file must carry its directory in the path, otherwise two
    // same-named files in different directories would collide on recovery.
    const FileNode* nested = nullptr;
    for (const FileNode& n : nodes) {
        if (n.name == L"inside.txt") { nested = &n; break; }
    }
    if (nested) {
        CHECK(nested->originalPath.find(L"subdir") != std::wstring::npos,
              "nested file keeps its directory in originalPath");
    }

    RemoveDirectoryW(outDir.c_str());
    reader.Close();

    const uint64_t after = Fnv1aFile(image);
    CHECK(after == before, "whole-image FNV-1a unchanged (scan source is read-only)");
}

// A deleted ReFS file has no live directory row.  Its name + extents survive
// in an MSB+ page unreachable from the checkpoint roots, and its bytes survive
// in orphaned clusters.  The engine must emit it as FileStatus::Deleted and
// RecoverEngine must reproduce the exact bytes from deleted.tsv truth.
void RunDeletedCase(const std::string& caseName)
{
    std::printf("-- %s (deleted residue)\n", caseName.c_str());

    std::vector<Expected> expected;
    if (!LoadExpectations(caseName, expected, "deleted.tsv")) {
        ++g_checks; ++g_fails;
        std::fprintf(stderr, "FAIL: deleted.tsv has no rows for %s\n", caseName.c_str());
        return;
    }
    CHECK(expected.size() == 1, "exactly one deleted truth row in deleted.tsv");

    const std::wstring image = GoldenDir() + L"/" +
                               Utils::Utf8ToWide(caseName) + L".bin";
    const uint64_t before = Fnv1aFile(image);
    CHECK(before != 0, "golden ReFS image is readable");

    SectorReader reader;
    CHECK(reader.OpenImage(image), "SectorReader opens the ReFS image");
    if (!reader.IsOpen()) return;

    const uint32_t ss = reader.GetSectorSize();
    Partition part;
    part.startSector = 0;
    part.totalSectors = 0;
    {
        FILE* f = _wfopen(image.c_str(), L"rb");
        if (f) {
            _fseeki64(f, 0, SEEK_END);
            const long long n = _ftelli64(f);
            fclose(f);
            part.totalSectors = static_cast<uint64_t>(n) / (ss ? ss : 512);
        }
    }
    part.sizeBytes = part.totalSectors * (ss ? ss : 512);
    part.fsType = L"";

    ScanResult scanResult;
    std::unique_ptr<FileSystem> parser =
        FileSystemModuleService::CreateParserForPartition(reader, part, scanResult);
    CHECK(parser != nullptr, "probe chain recognises the ReFS volume");
    if (!parser) { reader.Close(); return; }
    CHECK(parser->Open(reader, part), "RefsParser::Open succeeds");
    CHECK(parser->ScanMetadata(), "RefsParser::ScanMetadata succeeds");

    const std::vector<FileNode> nodes = parser->GetAllFiles();
    int liveCount = 0, deletedCount = 0;
    for (const FileNode& n : nodes) {
        if (n.status == FileStatus::Normal)  ++liveCount;
        if (n.status == FileStatus::Deleted) ++deletedCount;
    }
    std::printf("   enumerated %zu nodes (%d live, %d deleted)\n",
                nodes.size(), liveCount, deletedCount);
    CHECK(liveCount == 6, "six live entries remain Normal");
    CHECK(deletedCount == 1, "exactly one deleted residue node emitted");
    // The stale page must never be re-counted as a live file.
    CHECK(nodes.size() == 7, "live set plus one deleted node, no duplicates");

    const std::wstring outDir = MakeOutDir(caseName);
    RecoverEngine engine;
    for (const Expected& e : expected) {
        const std::wstring wname = Utils::Utf8ToWide(e.name);
        const FileNode* node = nullptr;
        for (const FileNode& n : nodes) {
            if (n.name == wname) { node = &n; break; }
        }
        std::printf("   %-14s ", e.name.c_str());
        if (!node) {
            std::printf("MISSING\n");
            ++g_checks; ++g_fails;
            std::fprintf(stderr, "FAIL: deleted node not found: %s\n", e.name.c_str());
            continue;
        }
        CHECK(node->status == FileStatus::Deleted, "residue node status is Deleted");
        CHECK(static_cast<long long>(node->size) == e.size,
              "deleted node size matches deleted.tsv");
        CHECK(node->recoverability != RecoverabilityLevel::Lost,
              "deleted node keeps recoverable clusters");

        const FileRecoverResult rr = engine.RecoverFile(*node, outDir, false, reader, part);
        if (rr.status != RecoverStatus::Success || rr.targetPath.empty()) {
            std::printf("RECOVER FAILED\n");
            ++g_checks; ++g_fails;
            std::fprintf(stderr, "FAIL: RecoverEngine failed for deleted %s\n", e.name.c_str());
            continue;
        }
        char      hex[65]{};
        long long got = 0;
        const bool hashed = Sha256OfFile(rr.targetPath, hex, got);
        CHECK(hashed, "recovered deleted output is readable");
        CHECK(got == e.size, "recovered byte count equals deleted logical size");
        if (hashed && !e.sha256.empty()) {
            const bool same = (std::strcmp(hex, e.sha256.c_str()) == 0);
            ++g_checks;
            if (!same) {
                ++g_fails;
                std::fprintf(stderr, "FAIL: deleted sha256 mismatch for %s\n"
                                     "  got  %s\n  want %s\n",
                             e.name.c_str(), hex, e.sha256.c_str());
            }
            std::printf("%s deleted\n", same ? "bytes ok" : "BYTES WRONG");
        }
        DeleteFileW(rr.targetPath.c_str());
    }

    RemoveDirectoryW(outDir.c_str());
    reader.Close();
    const uint64_t after = Fnv1aFile(image);
    CHECK(after == before, "whole-image FNV-1a unchanged (scan source is read-only)");
}

} // namespace

int main()
{
    RunCase("refs_v314_crc64_basic");
    RunCase("refs_v314_sha256");
    RunDeletedCase("refs_v314_deleted_residual");

    std::printf("%s: RefsRecoverBytes %d checks, %d failure(s)\n",
                g_fails ? "FAIL" : "OK", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

#endif
