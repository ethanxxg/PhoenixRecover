// test_signature_db_ac.cpp
//
// Proves that the AC-backed SignatureDB::Match (now the default, replacing the
// O(patterns × bytes) linear memcmp scan) is behavior-identical to the legacy
// linear scan over the *real* built-in signature set, and quantifies the
// algorithmic speedup on a large synthetic buffer.
//
// This is the integration point RawScan / CarveModuleService use for header
// magic discovery, so an identical hit set here means the raw-carving path now
// runs on the Aho-Corasick automaton with no behavioral change.
//
// Zero GUI, no Qt, no external images — fully offline.

#include "scan/SignatureDB.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <chrono>

namespace {

// Independent naive linear reference — mirrors the pre-AC Match loop exactly
// (including empty-header signatures, which always match). Using an independent
// oracle (not the test-only impl switch) keeps the regression non-circular.
const FileSignature* NaiveLinear(const SignatureDB& db,
                                const uint8_t* buf, size_t len)
{
    for (const auto& sig : db.GetAll()) {
        if (sig.headerOffset < 0) continue;
        size_t off = static_cast<size_t>(sig.headerOffset);
        if (off + sig.header.size() > len) continue;
        if (std::memcmp(buf + off, sig.header.data(), sig.header.size()) != 0)
            continue;
        if (sig.secondaryOffset >= 0 && !sig.secondary.empty()) {
            size_t so = static_cast<size_t>(sig.secondaryOffset);
            if (so + sig.secondary.size() > len) continue;
            if (std::memcmp(buf + so, sig.secondary.data(), sig.secondary.size()) != 0)
                continue;
        }
        return &sig;
    }
    return nullptr;
}

int g_failures = 0;
void Check(bool cond, const char* msg)
{
    if (!cond) { fprintf(stderr, "FAIL: %s\n", msg); ++g_failures; }
}

// Compare AC (default) vs naive linear reference at every window of buf, the way
// RawScan slides a kMatchStride-stepped window across a sector.
void CompareOverWindows(const SignatureDB& db, const std::vector<uint8_t>& buf,
                        const char* label)
{
    const size_t N = buf.size();
    for (size_t i = 0; i + 8 <= N; ++i) {
        size_t avail = N - i;
        const auto* ac  = db.Match(buf.data() + i, avail);
        const auto* ref = NaiveLinear(db, buf.data() + i, avail);
        if (ac != ref) {
            fprintf(stderr, "FAIL: %s mismatch @%zu (ac=%ls ref=%ls)\n",
                    label, i,
                    ac ? ac->extension.c_str() : L"<null>",
                    ref ? ref->extension.c_str() : L"<null>");
            ++g_failures; return;
        }
    }
}

// Cross-check against the test-only Linear impl switch (must agree with AC).
void CompareImplSwitch(const SignatureDB& db, const std::vector<uint8_t>& buf,
                       const char* label)
{
    const size_t N = buf.size();
    for (size_t i = 0; i + 8 <= N; ++i) {
        size_t avail = N - i;
        SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::Linear);
        const auto* lin = db.Match(buf.data() + i, avail);
        SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::AC);
        const auto* ac = db.Match(buf.data() + i, avail);
        if (lin != ac) {
            fprintf(stderr, "FAIL: %s impl-switch mismatch @%zu (lin=%ls ac=%ls)\n",
                    label, i,
                    lin ? lin->extension.c_str() : L"<null>",
                    ac ? ac->extension.c_str() : L"<null>");
            ++g_failures; return;
        }
    }
}

} // namespace

int main()
{
    SignatureDB db;
    fprintf(stdout, "built-in signatures: %zu\n", db.GetAll().size());
    fflush(stdout);
    Check(!db.GetAll().empty(), "signature DB non-empty");

    // ── 1. Deterministic headerOffset / secondary / negative cases ──────────
    {
        std::vector<uint8_t> b = {'R','I','F','F',0,0,0,0,'W','A','V','E'};
        const auto* ac  = db.Match(b.data(), b.size());
        const auto* ref = NaiveLinear(db, b.data(), b.size());
        Check(ac && ref && ac == ref, "RIFF/WAVE secondary");
    }
    {
        // mp4 ftyp at headerOffset=4: 4 junk bytes then "ftyp"
        std::vector<uint8_t> b = {'X','X','X','X','f','t','y','p','m','p','4','2'};
        const auto* ac  = db.Match(b.data(), b.size());
        const auto* ref = NaiveLinear(db, b.data(), b.size());
        Check(ac && ref && ac == ref, "mp4 headerOffset=4");
    }
    {
        // Negative: a magic-free buffer must yield the same (null) result in
        // both implementations. Arithmetic noise is intentionally unlikely to
        // contain a real magic, but we assert equivalence rather than "null".
        std::vector<uint8_t> b(64, 0x00);
        for (size_t k = 0; k < b.size(); ++k)
            b[k] = static_cast<uint8_t>((k * 31 + 7) & 0xFF);
        const auto* ac  = db.Match(b.data(), b.size());
        const auto* ref = NaiveLinear(db, b.data(), b.size());
        Check(ac == ref, "noise equivalence (AC == linear)");
    }
    fprintf(stdout, "phase1 done\n"); fflush(stdout);

    // ── 2. Random buffers: AC vs naive-linear at every window ────────────────
    std::mt19937 rng(0xC0FFEE);
    static const std::vector<std::vector<uint8_t>> magics = {
        {0xFF,0xD8,0xFF}, {0x89,0x50,0x4E,0x47}, {'P','K',0x03,0x04},
        {'%','P','D','F'}, {'R','I','F','F'}, {0x25,0x50,0x44,0x46,0x2D,0x31,0x2E,0x34}
    };
    for (int t = 0; t < 60; ++t) {
        std::vector<uint8_t> buf(64 + (rng() % 448));   // 64..512 bytes
        for (auto& x : buf) x = static_cast<uint8_t>(rng() & 0xFF);
        for (int p = 0; p < 2; ++p) {
            const auto& m = magics[rng() % magics.size()];
            if (buf.size() < m.size() + 8) continue;
            size_t off = rng() % (buf.size() - m.size() - 8);
            std::memcpy(buf.data() + off, m.data(), m.size());
        }
        CompareOverWindows(db, buf, "random-windows");
        CompareImplSwitch(db, buf, "random-switch");
    }
    fprintf(stdout, "phase2 done\n"); fflush(stdout);

    // ── 3. Large synthetic buffer: hit-set equality + speedup ────────────────
    {
        const size_t MiB = 8;
        const size_t total = MiB * 1024 * 1024;
        std::vector<uint8_t> big(total, 0);
        std::mt19937 rng2(0xABCDEF);
        for (auto& x : big) x = static_cast<uint8_t>(rng2() & 0xFF);
        for (int p = 0; p < 200; ++p) {            // plant 200 JPEG SOI markers
            size_t off = 16 + (rng2() % (total - 32));
            big[off] = 0xFF; big[off+1] = 0xD8; big[off+2] = 0xFF;
        }

        const size_t stride = 512;                // simulate per-sector scanning
        SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::Linear);
        auto t0 = std::chrono::steady_clock::now();
        long linHits = 0;
        for (size_t i = 0; i + 8 <= total; i += stride)
            if (db.Match(big.data() + i, total - i)) ++linHits;
        auto t1 = std::chrono::steady_clock::now();
        double linMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::AC);
        auto t2 = std::chrono::steady_clock::now();
        long acHits = 0;
        for (size_t i = 0; i + 8 <= total; i += stride)
            if (db.Match(big.data() + i, total - i)) ++acHits;
        auto t3 = std::chrono::steady_clock::now();
        double acMs = std::chrono::duration<double, std::milli>(t3 - t2).count();

        Check(linHits == acHits, "large buffer hit count equal (linear vs AC)");
        fprintf(stdout,
                "large buffer: linear=%.1f ms (%ld hits)  AC=%.1f ms (%ld hits)  "
                "speedup=%.2fx\n",
                linMs, linHits, acMs, acHits,
                linMs / (acMs > 1e-6 ? acMs : 1e-6));
        fflush(stdout);
    }

    SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::AC);
    if (g_failures == 0) {
        fprintf(stdout, "OK: SignatureDB AC Match equivalent to legacy linear scan\n");
        return 0;
    }
    fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
    return 1;
}
