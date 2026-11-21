// P0-6 regression gate: the real-disk pipeline must be ENGINE-ONLY.
//
//   DISCOVERY       NtfsParser::ScanMetadata() + GetAllFiles()   (scan cmd)
//   RECOVERY        RecoverEngine::RecoverBatch()                (recover cmd)
//   FORMAT MATCH    SignatureDB::Match() + FormatPayloadScoreUtil
//
// The independent Python MFT oracle is a byte-level VERIFICATION TRUTH, never
// a source of "which files exist". This gate makes that contract falsifiable:
//
//  1. Engine signature DB classifies real magic bytes (not filename guessing).
//  2. A magic/extension CONFLICT case proves the classifier reads payload.
//  3. AC and legacy Linear matching agree (production matcher behaviour).
//  4. ScanBatch (streaming format scan) finds a header at its disk offset.
//  5. Source-level guards pin the exact engine APIs in the headless driver
//     and the "engine" candidate source in the batch orchestrator.
//
// Negative guard: PHOENIX_NEGATIVE_ENGINE_FORMAT_GUARD=1 simulates the
// forbidden regression (format decided by declared extension / candidates
// taken from the oracle). The gate MUST then fail.
//
// Zero production changes; only the guard code and synthetic byte samples live
// here. Zero GUI changes, zero writes to any disk.

#include "scan/SignatureDB.h"
#include "recover/FormatPayloadScore.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef PHOENIX_SOURCE_DIR
#define PHOENIX_SOURCE_DIR L"."
#endif

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(expr)                                                         \
    do {                                                                    \
        ++g_checks;                                                         \
        if (!(expr)) {                                                      \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__,     \
                         __LINE__);                                         \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

static bool NegativeGuard()
{
    const char* v = std::getenv("PHOENIX_NEGATIVE_ENGINE_FORMAT_GUARD");
    return v && std::string(v) == "1";
}

static std::wstring LowerW(std::wstring s)
{
    for (auto& c : s)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return s;
}

static std::string Narrow(const std::wstring& w)
{
    std::string s;
    for (wchar_t c : w) s.push_back(static_cast<char>(c & 0xFF));
    return s;
}

static std::string ReadText(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

static bool Contains(const std::string& hay, const std::string& needle)
{
    return hay.find(needle) != std::string::npos;
}

static void ReplaceAll(std::string& s, const std::string& from,
                       const std::string& to)
{
    size_t p = 0;
    while ((p = s.find(from, p)) != std::string::npos) {
        s.replace(p, from.size(), to);
        p += to.size();
    }
}

static std::vector<uint8_t> Bytes(const char* p, size_t n)
{
    std::vector<uint8_t> v(p, p + n);
    return v;
}

static void Pad(std::vector<uint8_t>& v, size_t to, uint8_t fill)
{
    while (v.size() < to) v.push_back(fill);
}

struct Sample {
    const char*       label;
    std::vector<uint8_t> bytes;
    std::wstring      expect;
};

static std::vector<Sample> MakeSamples()
{
    std::vector<Sample> s;

    Sample jpg;
    jpg.label = "jpeg";
    const char jpgRaw[] = "\xFF\xD8\xFF\xE0\x00\x10JFIF\x00\x01\x01";
    jpg.bytes = Bytes(jpgRaw, sizeof(jpgRaw) - 1);
    Pad(jpg.bytes, 64, 0x5A);
    jpg.expect = L"jpg";
    s.push_back(jpg);

    Sample png;
    png.label = "png";
    const char pngRaw[] = "\x89PNG\x0D\x0A\x1A\x0A\x00\x00\x00\x0DIHDR"
                          "\x00\x00\x00\x01\x00\x00\x00\x01";
    png.bytes = Bytes(pngRaw, sizeof(pngRaw) - 1);
    Pad(png.bytes, 64, 0x5A);
    png.expect = L"png";
    s.push_back(png);

    Sample pdf;
    pdf.label = "pdf";
    const char pdfRaw[] = "%PDF-1.7\n1 0 obj\n";
    pdf.bytes = Bytes(pdfRaw, sizeof(pdfRaw) - 1);
    Pad(pdf.bytes, 64, 0x5A);
    pdf.expect = L"pdf";
    s.push_back(pdf);

    Sample zip;
    zip.label = "zip";
    const char zipRaw[] = "PK\x03\x04\x14\x00\x00\x00\x00\x00";
    zip.bytes = Bytes(zipRaw, sizeof(zipRaw) - 1);
    Pad(zip.bytes, 64, 0x5A);
    zip.expect = L"zip";
    s.push_back(zip);

    Sample gif;
    gif.label = "gif";
    const char gifRaw[] = "GIF89a\x01\x00\x01\x00";
    gif.bytes = Bytes(gifRaw, sizeof(gifRaw) - 1);
    Pad(gif.bytes, 64, 0x5A);
    gif.expect = L"gif";
    s.push_back(gif);

    // RIFF/WAV exercises the engine's SECONDARY sub-type disambiguation
    // (primary "RIFF" at 0 + "WAVE" at offset 8), not just a plain magic.
    // Side observation, NOT fixed here: the built-in sqlite entry is the
    // 9-byte "SQLite 3 " instead of "SQLite format 3\0". Production engine
    // code is intentionally left untouched for this verification task.
    Sample wav;
    wav.label = "wav";
    const char wavRaw[] = "RIFF\x24\x00\x00\x00WAVEfmt \x10\x00\x00\x00";
    wav.bytes = Bytes(wavRaw, sizeof(wavRaw) - 1);
    Pad(wav.bytes, 64, 0x5A);
    wav.expect = L"wav";
    s.push_back(wav);

    return s;
}

int main()
{
    const bool neg = NegativeGuard();
    const std::wstring root = PHOENIX_SOURCE_DIR;

    // ── Gate 1: production signature database is the classifier ──────────
    SignatureDB db;
    CHECK(db.GetAll().size() >= 500);   // 513 built-in signatures at P0-6
    std::vector<Sample> samples = MakeSamples();
    for (const auto& s : samples) {
        const FileSignature* sig = db.Match(s.bytes.data(), s.bytes.size());
        const std::wstring got = sig ? LowerW(sig->extension) : L"";
        if (got != s.expect)
            std::fprintf(stderr, "  sample %-8s expected=%ls got=%ls\n",
                         s.label, s.expect.c_str(), got.c_str());
        CHECK(got == s.expect);
    }

    // ── Gate 2: magic wins over a WRONG declared extension ────────────────
    // If format matching ever degrades to "look at the filename", this fails.
    const std::wstring declared = L"txt";
    const std::vector<uint8_t>& png = samples[1].bytes;
    const FileSignature* pngSig = db.Match(png.data(), png.size());
    const std::wstring engineExt = pngSig ? LowerW(pngSig->extension) : L"";
    CHECK(engineExt == L"png");
    CHECK(engineExt != declared);
    CHECK(FormatPayloadScoreUtil::ProbeExtension(png.data(), png.size()) == L"png");
    CHECK(static_cast<int>(FormatPayloadScoreUtil::Score(declared, png.data(),
                                                         png.size())) !=
          static_cast<int>(FormatPayloadScore::Complete));
    // Complete is what a correct PNG declaration would score at least Partial.
    CHECK(static_cast<int>(FormatPayloadScoreUtil::Score(L"png", png.data(),
                                                         png.size())) >=
          static_cast<int>(FormatPayloadScore::Partial));

    if (neg) {
        // Forbidden regression: classify by the declared extension.
        CHECK(engineExt == declared);                  // MUST FAIL
        CHECK(db.GetAll().empty());                    // MUST FAIL
    }

    // ── Gate 3: AC matcher == legacy linear matcher ───────────────────────
    SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::Linear);
    for (const auto& s : samples) {
        const FileSignature* sig = db.Match(s.bytes.data(), s.bytes.size());
        CHECK(sig && LowerW(sig->extension) == s.expect);
    }
    SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::AC);

    // ── Gate 4: streaming format scan (RawScan path) ──────────────────────
    std::vector<uint8_t> sector(512, 0x5A);
    std::memcpy(sector.data(), png.data(), 8);
    std::vector<SignatureDB::ScanCandidate> hits;
    db.ScanBatch(sector.data(), sector.size(), 4096, 512, hits);
    bool foundPng = false;
    for (const auto& h : hits)
        if (h.sig && LowerW(h.sig->extension) == L"png" && h.fileStart == 4096)
            foundPng = true;
    CHECK(foundPng);

    // ── Gate 5: source-level pipeline guards ──────────────────────────────
    std::string headless = ReadText(Narrow(root) + "/tools/phoenix_headless.cpp");
    std::string orch = ReadText(Narrow(root) + "/tools/batch_recover.py");
    if (neg) {
        // Simulate the two regressions the user forbade.
        ReplaceAll(headless, "m_db.Match", "GuessByExtension");
        ReplaceAll(headless, "SignatureDB::Match", "GuessByExtension");
        ReplaceAll(orch, "engine_nodes_vol", "candidates_vol");
        ReplaceAll(orch, "CANDIDATE_SOURCE = \"engine\"",
                   "CANDIDATE_SOURCE = \"oracle\"");
    }
    CHECK(!headless.empty());
    CHECK(!orch.empty());

    // Engine discovery / recovery / format matching APIs are really called.
    CHECK(Contains(headless, "ScanMetadata"));
    CHECK(Contains(headless, "GetAllFiles"));
    CHECK(Contains(headless, "RecoverBatch"));
    CHECK(Contains(headless, "ReadFileDataPrefix"));
    CHECK(Contains(headless, "SignatureDB"));
    CHECK(Contains(headless, "m_db.Match"));
    CHECK(Contains(headless, "FormatPayloadScoreUtil::Score"));
    CHECK(Contains(headless, "ProbeExtension"));
    CHECK(Contains(headless, "CmdScan"));

    // The orchestrator may not take candidates from the oracle.
    CHECK(Contains(orch, "CANDIDATE_SOURCE = \"engine\""));
    CHECK(Contains(orch, "engine_nodes_vol"));
    CHECK(Contains(orch, "assert_engine_discovery"));
    CHECK(Contains(orch, "summarize_engine_format"));
    CHECK(Contains(orch, "\"--engine-format\", \"1\""));
    CHECK(!Contains(orch, "candidates_vol%d.csv"));

    std::fprintf(stdout, "EngineScanFormatMatch: %d checks, %d failures%s\n",
                 g_checks, g_failures, neg ? " (NEGATIVE GUARD)" : "");
    if (neg) {
        if (g_failures == 0) {
            std::fprintf(stderr,
                         "FAIL: negative guard did not fail any check — "
                         "the gate is not falsifiable\n");
            return 1;
        }
        std::fprintf(stdout,
                     "NEGATIVE GUARD: %d checks failed as expected -> exit 1\n",
                     g_failures);
        return 1;   // a negative run is EXPECTED to fail CTest
    }
    return g_failures == 0 ? 0 : 1;
}
