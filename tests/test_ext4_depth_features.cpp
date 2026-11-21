// ext4 depth gate: features that the ext2/ext3 legacy golden (W6) cannot reach.
//
// The fixture images are produced by tools/make_ext4_fixture.py, an independent
// oracle that writes the on-disk structures itself (superblock, group
// descriptors, inode table, extent trees, dirent v2).  No ext4 implementation
// participates in producing the expected bytes; the manifest carries the
// sha256 of every payload.
//
// Variants and what each pins:
//
//   ext4-base       extents (3-block + 6-block runs), legacy indirect map
//                   (12 direct + 1 single-indirect), a deleted file whose
//                   dirent survives only in the rec_len slack, and an orphan
//                   inode with no dirent at all.
//   ext4-uninit     a preallocated file whose extent is marked UNINITIALIZED
//                   (ee_len = 32768 + n).  The kernel defines this as zeros;
//                   reading the blocks' stale 0xAA bytes is a recovery bug.
//   ext4-bigalloc   bigalloc volume (cluster = 2 blocks).  ee_start/ee_len are
//                   CLUSTERS, so a block-based reading lands on the wrong
//                   offset for the whole volume.
//   ext4-inline     inline_data: a 40-byte file and a small directory stored
//                   inside the inode body.  Without support the file body is
//                   lost and the whole subtree under the inline directory
//                   disappears.
//   ext4-sparse     depth-1 extent tree with three extents separated by holes,
//                   plus a legacy indirect map with a hole.  Missing hole
//                   markers shift every byte after the gap (size matches,
//                   sha256 does not).
//   ext4-journal    JBD2 replay.  The inode-table block and the root directory
//                   block of a new file exist only in the committed log; the
//                   home copies are stale (inode slot zeroed, no dirent).  A
//                   third transaction logs a directory block that is then
//                   REVOKED, so a replay that ignores revokes produces a wrong
//                   file list (poison.txt appears, keep.txt disappears).
//   ext4-journal-torn  synthetic power loss: txn 101 has descriptor + data but
//                   no commit.  Only the earlier committed txn 100 may replay;
//                   a pending transaction must never become visible.
//   ext4-journal-v1    original JBD (ext3-era) superblock type 3 and 8-byte
//                   tags with a trailing 16-byte UUID.  Parsing them with the
//                   JBD2 tag rule desyncs the log; the same bytes must replay.
//   ext4-journal-clean  identical journal bytes with s_start = 0: a
//                   checkpointed log must replay nothing at all.
//   ext4-journal-csum2 / -csum3  JBD2 checksum v2 (16-bit tag CRC) and v3
//                   (32-bit tag CRC): descriptor tail, commit h_chksum[0],
//                   superblock s_checksum and each data-block tag checksum
//                   are CRC32C.  The same bytes must replay as the legacy log.
//   ext4-journal-csum-bad  v3 journal with the first commit checksum flipped:
//                   the engine must refuse the transaction (0 replays).  A
//                   corrupt-log overlay is silent wrong data, worse than none.
//   ext4-flex-bg-gdt  two groups with FLEX_BG relocation: group 1's inode
//                   table lives in group 0 and the correct group descriptor
//                   only exists in the committed log.  Without reloading the
//                   GDT after replay, inode 65 / far.txt is silently lost.
//   ext4-journal-external  s_journal_dev: the log is a separate image.  The
//                   engine must verify its UUID against the volume and refuse
//                   replay when no external source was supplied.
//
// Also asserts the scan-source read-only invariant per image (whole-image
// FNV-1a unchanged after scan + recovery).
//
// No <filesystem>, no Qt: builds under MinGW g++ 7.3 and GCC 14.2.

#include "common/Platform.h"

#if !PHOENIX_OS_WINDOWS
int main()
{
    std::printf("SKIP: Ext4DepthFeatures requires the Windows FS code path\n");
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
#include "fs/ext4/Ext4Parser.h"
#include "recover/FileNode.h"
#include "recover/RecoverEngine.h"
#include "scan/FileSystemModuleService.h"
#include "scan/ScanEngine.h"
#include "scan/SignatureJson.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
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

struct ExpectedFile
{
    std::string path;      // e.g. "\\hello.txt"
    std::string name;      // e.g. "hello.txt"
    std::string sha256;    // empty for directories
    std::string status;    // "normal" | "deleted"
    std::string kind;      // "data" | "zeros" | "dir"
    long long   size = 0;
};

struct Fixture
{
    std::string               variant;
    long long                 imageBytes = 0;
    long long                 startSector = 0;
    std::vector<ExpectedFile> files;

    // JBD2 variants: what replay must have done, and which names must stay
    // absent (revoked, or never logged at all).
    bool                     hasJournal = false;
    long long                jnlApplied = -1;   // transactions contributing blocks
    long long                jnlBlocks  = -1;   // overlaid blocks
    bool                     v1         = false;
    std::wstring             externalJournalPath;
    std::vector<std::string> absent;
};

std::wstring FixtureDir()
{
#ifdef PHOENIX_EXT4_FIXTURE_DIR
    return std::wstring(PHOENIX_EXT4_FIXTURE_DIR);
#else
    return std::wstring(PHOENIX_SOURCE_DIR) + L"/testdata/ext4-synthetic";
#endif
}

bool ReadWholeFile(const std::wstring& path, std::string& out)
{
    out.clear();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char buf[8192];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

bool LoadManifest(const std::wstring& jsonPath, Fixture& fx)
{
    std::string text;
    if (!ReadWholeFile(jsonPath, text)) return false;
    phoenix::JsonValue root;
    phoenix::JsonParser parser(text);
    if (!parser.Parse(root) || !root.IsObject()) return false;

    if (const phoenix::JsonValue* v = root.Find("variant")) fx.variant = v->AsString();
    if (const phoenix::JsonValue* v = root.Find("imageBytes"))
        fx.imageBytes = static_cast<long long>(v->AsNumber());
    if (const phoenix::JsonValue* v = root.Find("externalJournalPath"))
        fx.externalJournalPath = Utils::Utf8ToWide(v->AsString());
    if (const phoenix::JsonValue* v = root.Find("partitionStartSector"))
        fx.startSector = static_cast<long long>(v->AsNumber());

    if (const phoenix::JsonValue* j = root.Find("journal")) {
        fx.hasJournal = true;
        if (const phoenix::JsonValue* v = j->Find("appliedTransactions"))
            fx.jnlApplied = static_cast<long long>(v->AsNumber());
        if (const phoenix::JsonValue* v = j->Find("overlayBlocks"))
            fx.jnlBlocks = static_cast<long long>(v->AsNumber());
        // JsonValue has no AsBool(); the fixture name is the stable marker.
        fx.v1 = (fx.variant == "ext4-journal-v1");
    }
    if (const phoenix::JsonValue* a = root.Find("absent")) {
        if (a->IsArray())
            for (const phoenix::JsonValue& item : a->array)
                fx.absent.push_back(item.AsString());
    }

    const phoenix::JsonValue* files = root.Find("files");
    if (!files || !files->IsArray()) return false;
    for (const phoenix::JsonValue& item : files->array) {
        ExpectedFile e;
        if (const phoenix::JsonValue* v = item.Find("path"))   e.path   = v->AsString();
        if (const phoenix::JsonValue* v = item.Find("name"))   e.name   = v->AsString();
        if (const phoenix::JsonValue* v = item.Find("sha256")) e.sha256 = v->AsString();
        if (const phoenix::JsonValue* v = item.Find("status")) e.status = v->AsString();
        if (const phoenix::JsonValue* v = item.Find("kind"))   e.kind   = v->AsString();
        if (const phoenix::JsonValue* v = item.Find("size"))
            e.size = static_cast<long long>(v->AsNumber());
        fx.files.push_back(e);
    }
    return true;
}

bool Sha256Hex(const uint8_t* data, size_t len, char out65[65])
{
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool ok = false;
    if (CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
            size_t off = 0;
            ok = true;
            while (off < len) {
                DWORD chunk = static_cast<DWORD>((std::min)(size_t(1u << 30), len - off));
                if (!CryptHashData(hash, data + off, chunk, 0)) { ok = false; break; }
                off += chunk;
            }
            if (ok) {
                BYTE digest[32]{};
                DWORD cb = sizeof(digest);
                if (CryptGetHashParam(hash, HP_HASHVAL, digest, &cb, 0) && cb == 32) {
                    static const char hexd[] = "0123456789abcdef";
                    for (int i = 0; i < 32; ++i) {
                        out65[i * 2]     = hexd[digest[i] >> 4];
                        out65[i * 2 + 1] = hexd[digest[i] & 15];
                    }
                    out65[64] = '\0';
                } else {
                    ok = false;
                }
            }
        }
        if (hash) CryptDestroyHash(hash);
        CryptReleaseContext(prov, 0);
    }
    return ok;
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

std::wstring MakeOutDir(const wchar_t* tag)
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"phx_ext4_" + tag + L"_" +
                       std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

const FileNode* FindNode(const std::vector<FileNode>& nodes,
                         const std::string& path, const std::string& name)
{
    const std::wstring wpath = Utils::Utf8ToWide(path);
    const std::wstring wname = Utils::Utf8ToWide(name);
    for (const FileNode& n : nodes) {
        if (n.name == wname && n.originalPath == wpath) return &n;
    }
    // Path spelling can differ for synthetic orphans; fall back to name only
    // when the name is otherwise unique.
    const FileNode* only = nullptr;
    int matches = 0;
    for (const FileNode& n : nodes) {
        if (n.name == wname) { only = &n; ++matches; }
    }
    return matches == 1 ? only : nullptr;
}

void RunVariant(const Fixture& fx, bool autoProbe)
{
    std::printf("-- %s (%s)\n", fx.variant.c_str(), autoProbe ? "auto-probe" : "direct");
    const std::wstring image = FixtureDir() + L"/" +
                               std::wstring(fx.variant.begin(), fx.variant.end()) + L".img";

    const uint64_t before = Fnv1aFile(image);
    CHECK(before != 0, "fixture image is readable");

    SectorReader reader;
    CHECK(reader.OpenImage(image), "SectorReader opens the synthetic ext4 image");
    if (!reader.IsOpen()) return;

    const uint32_t ss = reader.GetSectorSize();
    Partition part;
    part.startSector  = static_cast<uint64_t>(fx.startSector);
    part.totalSectors = static_cast<uint64_t>(fx.imageBytes / (ss ? ss : 512));
    part.sizeBytes    = part.totalSectors * (ss ? ss : 512);
    part.fsType       = L"";             // always exercise the production chain

    ScanResult scanResult;
    std::unique_ptr<FileSystem> parser =
        FileSystemModuleService::CreateParserForPartition(reader, part, scanResult);
    CHECK(parser != nullptr, "production probe chain resolves the ext4 volume");
    if (!parser) {
        std::printf("   probe detail: ss=%u start=%llu total=%llu bytes=%lld err=%ls\n",
                    ss, static_cast<unsigned long long>(part.startSector),
                    static_cast<unsigned long long>(part.totalSectors),
                    fx.imageBytes, scanResult.errorMsg.c_str());
        reader.Close();
        return;
    }
    if (autoProbe) {
        CHECK(dynamic_cast<Ext4Parser*>(parser.get()) != nullptr,
              "empty fsType resolves to Ext4Parser");
    }
    if (!fx.externalJournalPath.empty()) {
        const std::wstring jpath = FixtureDir() + L"/" + fx.externalJournalPath;
        Ext4Parser* ep = dynamic_cast<Ext4Parser*>(parser.get());
        CHECK(ep != nullptr, "external-journal variant resolves to Ext4Parser");
        CHECK(ep && ep->SetExternalJournalImage(jpath),
              "external journal device is opened before volume Open");
    }
    CHECK(parser->Open(reader, part), "Ext4Parser::Open accepts the fixture");
    CHECK(parser->ScanMetadata(), "Ext4Parser::ScanMetadata completes");

    const std::vector<FileNode> nodes = parser->GetAllFiles();
    const std::wstring outDir = MakeOutDir(std::wstring(fx.variant.begin(),
                                                         fx.variant.end()).c_str());

    // ── JBD2: prove replay actually ran, and that it did not over-reach ───
    if (fx.hasJournal) {
        Ext4Parser* ep = dynamic_cast<Ext4Parser*>(parser.get());
        CHECK(ep != nullptr, "journal variant resolves to Ext4Parser");
        if (ep) {
            std::printf("   journal: %u transaction(s), %u block(s) overlaid\n",
                        ep->GetJournalCommitCount(), ep->GetJournalOverlayBlocks());
            CHECK(static_cast<long long>(ep->GetJournalCommitCount()) == fx.jnlApplied,
                  "journal replay applied the expected transaction count");
            CHECK(static_cast<long long>(ep->GetJournalOverlayBlocks()) == fx.jnlBlocks,
                  "journal overlay holds the expected block count");
            if (fx.v1)
                CHECK(ep->GetJournalCommitCount() > 0,
                      "JBD v1 inline-UUID tags were parsed (not silently refused)");
        }
    }

    // Names that must NOT be listed: a revoke that was ignored, or a clean
    // journal that got replayed anyway, both show up here first.
    for (const std::string& nm : fx.absent) {
        const std::wstring w = Utils::Utf8ToWide(nm);
        bool found = false;
        for (const FileNode& n : nodes)
            if (n.name == w) { found = true; break; }
        CHECK(!found, "node must not be listed (revoked or never logged)");
        std::printf("   %-28s %s (must be absent)\n", nm.c_str(),
                    found ? "PRESENT  <-- WRONG" : "absent ok");
    }

    for (const ExpectedFile& e : fx.files) {
        const FileNode* node = FindNode(nodes, e.path, e.name);
        std::printf("   %-28s ", e.name.c_str());
        if (!node) {
            std::printf("MISSING\n");
            ++g_checks; ++g_fails;
            std::fprintf(stderr, "FAIL: node not found: %s\n", e.path.c_str());
            continue;
        }
        const bool wantDeleted = (e.status == "deleted");
        CHECK(node->status == (wantDeleted ? FileStatus::Deleted : FileStatus::Normal),
              "node deletion state matches the manifest");
        if (e.kind == "dir") {
            CHECK(node->isDirectory, "inline directory is reported as a directory");
            std::printf("dir ok\n");
            continue;
        }
        CHECK(!node->isDirectory, "regular file is not reported as a directory");
        CHECK(static_cast<long long>(node->size) == e.size, "node size matches the manifest");

        RecoverEngine engine;
        const FileRecoverResult rr = engine.RecoverFile(*node, outDir, false, reader, part);
        if (rr.status != RecoverStatus::Success || rr.targetPath.empty()) {
            std::printf("RECOVER FAILED\n");
            ++g_checks; ++g_fails;
            std::fprintf(stderr, "FAIL: RecoverEngine failed for %s\n", e.path.c_str());
            continue;
        }
        char      hex[65]{};
        long long got = 0;
        const bool hashed = Sha256OfFile(rr.targetPath, hex, got);
        CHECK(hashed, "recovered output is readable");
        CHECK(got == e.size, "recovered byte count equals the logical size");
        if (hashed && !e.sha256.empty()) {
            const bool same = (std::strcmp(hex, e.sha256.c_str()) == 0);
            if (!same) {
                std::fprintf(stderr, "FAIL: sha256 mismatch for %s\n  got  %s\n  want %s\n",
                             e.path.c_str(), hex, e.sha256.c_str());
            }
            ++g_checks;
            if (!same) ++g_fails;
            std::printf("%s %s\n", same ? "bytes ok" : "BYTES WRONG", e.kind.c_str());
        } else {
            std::printf("size ok (no sha)\n");
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
    struct VariantSpec
    {
        const char* name;
        bool        autoProbe;
    };
    const VariantSpec variants[] = {
        {"ext4-base",          true},   // also proves the production probe chain
        {"ext4-uninit",        false},
        {"ext4-bigalloc",      false},
        {"ext4-inline",        false},
        {"ext4-sparse",        false},  // depth-1 extent tree + holes, legacy indirect hole
        {"ext4-journal",       false},  // JBD2 replay: recovered from the log only
        {"ext4-journal-torn",  false},  // torn commit: discard the pending txn
        {"ext4-journal-v1",    false},  // JBD v1: be32 flags + inline 16-byte UUID
        {"ext4-journal-clean", false},  // s_start == 0 must replay nothing
        // JBD2 checksum v2/v3: tag/commit/descriptor CRC32C must verify and
        // replay the same bytes; a corrupted commit checksum must refuse ALL
        // replay (a wrong overlay is worse than no replay).
        {"ext4-journal-csum2", false},
        {"ext4-journal-csum3", false},
        {"ext4-journal-csum-bad", false},
        {"ext4-flex-bg-gdt",   false},  // FLEX_BG + replayed cross-group GDT
        {"ext4-journal-external", false},  // s_journal_dev + UUID-matched log
    };

    for (const VariantSpec& v : variants) {
        Fixture fx;
        const std::wstring json = FixtureDir() + L"/" +
                                  std::wstring(v.name, v.name + std::strlen(v.name)) + L".json";
        if (!LoadManifest(json, fx)) {
            std::printf("SKIP %s: manifest missing (%ls)\n"
                        "     run: py -3 tools/make_ext4_fixture.py\n",
                        v.name, json.c_str());
            ++g_checks;
            ++g_fails;
            continue;
        }
        RunVariant(fx, v.autoProbe);
    }

    std::printf("%s: Ext4DepthFeatures %d checks, %d failure(s)\n",
                g_fails ? "FAIL" : "OK", g_checks, g_fails);
    return g_fails ? 1 : 0;
}

#endif
