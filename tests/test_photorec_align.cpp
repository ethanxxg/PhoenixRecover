// PhotoRec carving-alignment gate.
//
// TestDisk's carving half (PhotoRec) ships 350 src/file_*.c format handlers.
// Almost all of them begin with a purely declarative registration:
//
//     register_header_check(<offset>, <array>, sizeof(<array>), &cb, file_stat)
//
// tools/gen_photorec_signatures.py resolves those array literals into
// scan/PhotoRecSignatures.h, which SignatureDB's constructor registers after
// the built-in table.  This gate pins three properties:
//
//   1. Every extracted magic is actually present in the production database
//      (extension + magic bytes + offset), i.e. the header is wired in.
//   2. Match() resolves each magic to that very signature, so the AC backend
//      and the linear reference agree on the new entries.
//   3. No signature shorter than 3 bytes is in the default set.  A 1-2 byte
//      magic matches random data every few hundred bytes; PhotoRec can afford
//      that because each magic is paired with a structural callback, which we
//      deliberately do not reproduce here.  The invariant is what keeps the
//      carving false-positive rate from collapsing.
//
// The expected JSON is produced by the generator and lives under testdata/
// (not committed, per project convention).  When it is absent the gate still
// checks the compiled-in database against the counts baked in below, so a
// clean checkout keeps a meaningful signal.
//
// Negative guard: comment out the RegisterPhotoRecAlignSignatures() call in
// scan/SignatureDB.cpp and this test must FAIL.

#include "scan/SignatureDB.h"
#include "scan/SignatureJson.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

static int g_checks = 0;
static int g_fails  = 0;

#define CHECK(expr, msg)                                                        \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(expr)) {                                                          \
            ++g_fails;                                                          \
            std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__); \
        }                                                                       \
    } while (0)

namespace {

// Formats that only exist because of the PhotoRec alignment.  Each one is a
// real-world container we previously could not name; a sampling keeps the
// assertion honest if the generator ever regresses.
const wchar_t* const kKeyFormats[] = {
    L"e01",    // EnCase forensic image
    L"luks",   // Linux LUKS header
    L"accdb",  // Microsoft Access 2007+
    L"one",    // OneNote section
    L"cwk",    // ClarisWorks / AppleWorks
    L"dxf",    // AutoCAD drawing
    L"stl",    // 3D printing mesh
    L"xfs",    // XFS filesystem blocks
    L"wtv",    // Windows Recorded TV
    L"m2ts",   // Blu-ray transport stream
    L"vib",    // Veeam backup
};

struct GoldenSig
{
    std::string           ext;
    std::vector<uint8_t>  magic;
    int                   off = 0;
};

std::vector<uint8_t> HexToBytes(const std::string& hex)
{
    std::vector<uint8_t> out;
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = hv(hex[i]), lo = hv(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

std::string HexOf(const std::vector<uint8_t>& bytes)
{
    static const char* k = "0123456789ABCDEF";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) { s.push_back(k[b >> 4]); s.push_back(k[b & 0xF]); }
    return s;
}

bool LoadGolden(const std::string& path, std::vector<GoldenSig>& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    phoenix::JsonValue root;
    phoenix::JsonParser parser(text);
    if (!parser.Parse(root)) return false;
    const phoenix::JsonValue* arr = root.Find("signatures");
    if (!arr || !arr->IsArray()) return false;
    for (const auto& item : arr->array) {
        GoldenSig g;
        if (const phoenix::JsonValue* v = item.Find("extension")) g.ext = v->AsString();
        if (const phoenix::JsonValue* v = item.Find("magic"))     g.magic = HexToBytes(v->AsString());
        if (const phoenix::JsonValue* v = item.Find("magic_offset"))
            g.off = static_cast<int>(v->AsNumber());
        if (g.ext.empty() || g.magic.empty()) continue;
        out.push_back(std::move(g));
    }
    return !out.empty();
}

} // namespace

int main()
{
    SignatureDB db;
    const auto& all = db.GetAll();

    std::printf("built-in + PhotoRec signatures: %zu\n", all.size());

    // The pre-alignment database shipped 513 signatures over 309 extensions.
    // The generator contributes ~322 more (>= 3-byte magics, duplicates of
    // built-in entries already excluded).
    CHECK(all.size() >= 800, "database holds the built-in set plus the PhotoRec alignment");

    std::set<std::wstring> exts;
    for (const auto& s : all) exts.insert(s.extension);
    std::printf("distinct extensions: %zu\n", exts.size());
    CHECK(exts.size() >= 450, "extension coverage grew past the pre-alignment 309");

    // Invariant 3: the alignment must not add degenerate magics.
    //
    // The pre-alignment database already carried 43 signatures shorter than
    // 3 bytes, all deliberate:
    //   30 in the hand-written table  (dbf "03", bmp "424D", gz "1F8B", ...)
    //   13 in the UI type-coverage aliases (efi/ocx/ax/com/scr/vxd/drv "4D5A",
    //      tgz "1F8B", ini "5B", dib "424D", acc "FFF1", pam "5031", pm "5036")
    // They are a precision/recall trade-off made earlier and out of scope here,
    // so this gate freezes the count: the PhotoRec layer (which only ships
    // magics >= 3 bytes) is not allowed to make it worse.
    constexpr size_t kShortMagicBaseline = 43;
    size_t shortMagics = 0;
    std::string shortestDesc, shortList;
    size_t shortest = 999;
    for (const auto& s : all) {
        if (s.header.size() < 3) {
            ++shortMagics;
            shortList += " " + std::string(s.extension.begin(), s.extension.end()) +
                         "=" + HexOf(s.header);
        }
        if (s.header.size() < shortest) {
            shortest = s.header.size();
            shortestDesc = std::string(s.extension.begin(), s.extension.end()) +
                           " (" + HexOf(s.header) + ")";
        }
    }
    std::printf("short magics (<3B): %zu (baseline %zu), shortest %zu byte(s): %s\n",
                shortMagics, kShortMagicBaseline, shortest, shortestDesc.c_str());
    if (shortMagics > kShortMagicBaseline)
        std::printf("  short set:%s\n", shortList.c_str());
    CHECK(shortMagics <= kShortMagicBaseline,
          "PhotoRec alignment introduces no new sub-3-byte magic");

    // Invariant 1: the named formats are present.
    for (const wchar_t* want : kKeyFormats)
        CHECK(exts.count(want) == 1, "PhotoRec format present in the database");

    // Invariant 2: every extracted magic resolves through Match().
    std::vector<GoldenSig> golden;
    const std::wstring wroot(PHOENIX_SOURCE_DIR);
    const std::string path = std::string(wroot.begin(), wroot.end()) +
                             "/testdata/signatures/photorec_aligned.json";
    const bool haveGolden = LoadGolden(path, golden);
    std::printf("golden signature file: %s (%zu entries)\n",
                haveGolden ? "loaded" : "absent - count checks only", golden.size());

    if (haveGolden) {
        size_t missing = 0, shadowed = 0, badShadow = 0, probeFail = 0;
        for (const auto& g : golden) {
            const std::string want = HexOf(g.magic);
            bool present = false;
            for (const auto& s : all) {
                if (s.extension != std::wstring(g.ext.begin(), g.ext.end())) continue;
                if (s.headerOffset != g.off) continue;
                if (HexOf(s.header) != want) continue;
                present = true;
                break;
            }
            if (!present) {
                ++missing;
                if (missing <= 5)
                    std::fprintf(stderr, "  missing: %s off=%d magic=%s\n",
                                 g.ext.c_str(), g.off, want.c_str());
                continue;
            }

            // Minimal sample: 0xFF filler so a magic can never be silently
            // satisfied by zero bytes past its end (dv "1F0700" must not be
            // shadowed by dif "1F070000" merely because byte 4 is 0).
            std::vector<uint8_t> buf(static_cast<size_t>(g.off) + g.magic.size() + 64, 0xFF);
            std::memcpy(buf.data() + g.off, g.magic.data(), g.magic.size());
            const FileSignature* hit = db.Match(buf.data(), buf.size());
            if (!hit) {
                ++probeFail;
                if (probeFail <= 5)
                    std::fprintf(stderr, "  no match: %s off=%d magic=%s len=%zu\n",
                                 g.ext.c_str(), g.off, want.c_str(), buf.size());
                continue;
            }

            // Same format: a different variant / offset of the same type won
            // the tie-break (e.g. built-in iso "CD001"@32769 vs PhotoRec
            // "\x01CD001"@32768 are both right).  That is the intended
            // behaviour, so it counts as a pass.
            const std::wstring w(g.ext.begin(), g.ext.end());
            if (hit->extension == w) continue;

            // A different format won the offset.  That is only acceptable when
            // one magic is a prefix of the other: Match()/ScanBatch keep the
            // lowest pattern id on a tie, so a shorter built-in magic
            // (qcow "514649FB") shadows a longer PhotoRec one
            // (cow "514649FB00000001"), and vice versa.
            const std::string got = HexOf(hit->header);
            const bool prefix = !got.empty() &&
                (want.compare(0, got.size(), got) == 0 ||
                 got.compare(0, want.size(), want) == 0);
            if (prefix) {
                ++shadowed;
                continue;
            }
            ++badShadow;
            if (badShadow <= 5)
                std::fprintf(stderr, "  wrong hit: %s -> %ls off=%d/%d magic=%s vs %s\n",
                             g.ext.c_str(), hit->extension.c_str(), g.off,
                             hit->headerOffset, want.c_str(), got.c_str());
        }
        std::printf("golden probe: missing=%zu shadowed=%zu badShadow=%zu noMatch=%zu\n",
                    missing, shadowed, badShadow, probeFail);
        CHECK(missing == 0,     "every extracted magic is registered in the database");
        CHECK(badShadow == 0,   "Match() never resolves a magic to an unrelated format");
        CHECK(probeFail == 0,   "Match() finds a signature at the expected offset");
        // Prefix shadowing is bounded by the number of deliberately shorter
        // built-in magics; a jump means a new tie-break problem appeared.
        CHECK(shadowed <= 100,  "prefix shadowing stays within the known bound");
    }

    // Invariant 4: a blank volume must stay blank.
    //
    // RAW carving falls back to signature scanning whenever structured parsing
    // finds nothing, and a freshly formatted volume is exactly that case.  If
    // any enabled magic can be satisfied by zero bytes, every 512-byte slot
    // becomes a "file" and the fallback floods the result list.  Scanning an
    // all-zero buffer at the real stride pins that failure mode directly, with
    // no disk I/O involved.
    {
        constexpr size_t kBlank = 4 * 1024 * 1024;
        std::vector<uint8_t> blank(kBlank, 0);
        std::vector<SignatureDB::ScanCandidate> cands;
        db.ScanBatch(blank.data(), blank.size(), 0, 512, cands);
        std::printf("blank 4 MiB volume candidates at stride 512: %zu\n", cands.size());
        for (size_t i = 0; i < cands.size() && i < 10; ++i)
            std::fprintf(stderr, "  blank hit @%llu: %ls\n",
                         static_cast<unsigned long long>(cands[i].fileStart),
                         cands[i].sig ? cands[i].sig->extension.c_str() : L"(null)");
        CHECK(cands.empty(), "an all-zero volume yields no carving candidates");
    }

    std::printf("%s: PhotoRecAlign %d checks, %d failure(s)\n",
                g_fails ? "FAIL" : "OK", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
