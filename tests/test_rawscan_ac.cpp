// A1-6 / parallel-track: streaming Aho-Corasick batch scan wired into RawScan.
//
// Proves the streaming batch discovery (SignatureDB::ScanBatch) is behavior-
// identical to the legacy per-stride linear match, both at the unit level
// (ScanBatch AC == ScanBatch Linear) and end-to-end (RawScan::Run Linear vs AC
// on a synthetic disk image). Also reports an honest Linear-vs-AC timing.
//
// Build: no Qt, no device I/O. PHOENIX_BUILD_GUI=0.

#include "scan/SignatureDB.h"
#include "scan/RawScan.h"
#include "disk/SectorReader.h"

#include <algorithm>
#include <chrono>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> HexToBytes(const std::string& hex)
{
    std::vector<uint8_t> out;
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<uint8_t>((hv(hex[i]) << 4) | hv(hex[i + 1])));
    return out;
}

void Plant(std::vector<uint8_t>& img, size_t off, const std::string& headerHex)
{
    std::vector<uint8_t> b = HexToBytes(headerHex);
    if (off + b.size() > img.size()) return;
    std::memcpy(img.data() + off, b.data(), b.size());
}

} // namespace

int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* msg) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", msg); ++failures; }
        else     { std::fprintf(stdout, "ok:   %s\n", msg); }
    };

    // ── Build a synthetic 4 MiB partition image ──
    const size_t kImgSize = 4 * 1024 * 1024;
    std::vector<uint8_t> img(kImgSize, 0);
    std::mt19937_64 rng(0x1234abcdULL);
    for (size_t i = 0; i < img.size(); ++i) img[i] = static_cast<uint8_t>(rng() & 0xFF);

    // Planted magic specs: {fileOffset (512-aligned), headerHex, label}.
    // Every entry is a discoverable PRIMARY magic (its header matches at
    // headerOffset). Secondary companion bytes (e.g. the 'WAVE' 4cc) are planted
    // separately and must NOT be in this list, since they are never reported as a
    // discovery fileStart on their own — they only disambiguate the primary.
    struct Spec { size_t off; std::string hex; std::string label; };
    std::vector<Spec> specs = {
        {0*512,      "FFD8FFE0",         "jpg@0"},
        {200*512,    "89504E470D0A1A0A", "png@200"},
        {401*512,    "504B0304",         "zip@401"},
        {800*512,    "25504446",         "pdf@800"},
        {1200*512,   "52494646",         "riff@1200"},       // RIFF primary; WAVE 4cc just after makes it resolve to .wav
        {2000*512,   "474946383961",     "gif@2000"},
        {2400*512,   "D0CF11E0A1B11AE1", "cfb/doc@2400"},
        {2800*512,   "49492A00",         "tif@2800"},
        {3200*512,   "FFD8FFE1",         "jpg-exif@3200"},
    };
    // mp4: ISO-BMFF 'ftyp' box — 4-byte size prefix, 'ftyp' at +4, brand 'isom' at
    // +8. The ftyp sig uses headerOffset=4 with a secondary brand check at +8, so a
    // real brand 4cc must be present for discovery (mirrors actual MP4 layout).
    {
        size_t off = 1600 * 512;
        std::vector<uint8_t> head = HexToBytes("000000186674797069736F6D"); // size + 'ftyp' + 'isom'
        std::memcpy(img.data() + off, head.data(), head.size());
        // ftyp is a discoverable primary (headerOffset=4 + secondary brand) — verify it.
        specs.push_back({off, "000000186674797069736F6D", "mp4-ftyp@1600"});
    }
    // Companion secondary bytes: 'WAVE' at RIFF+8 so the riff@1200 magic resolves
    // to .wav (sub-type disambiguation). NOT a discoverable primary itself.
    Plant(img, 1200*512 + 8, "57415645");
    // A magic at a NON-512-aligned offset must be ignored by the stride scan.
    Plant(img, 1234, "FFD8FFE0");

    for (auto& s : specs) Plant(img, s.off, s.hex);

    // ── Test 1: ScanBatch AC == ScanBatch Linear (per-position equivalence) ──
    SignatureDB db;
    std::vector<SignatureDB::ScanCandidate> linearC, acC;

    SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::Linear);
    db.ScanBatch(img.data(), img.size(), 0, 512, linearC);
    SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::AC);
    db.ScanBatch(img.data(), img.size(), 0, 512, acC);

    std::set<uint64_t> linOff, acOff;
    for (auto& c : linearC) linOff.insert(c.fileStart);
    for (auto& c : acC)     acOff.insert(c.fileStart);
    check(linOff == acOff, "ScanBatch AC vs Linear: identical discovered offsets");
    if (linOff != acOff) {
        std::fprintf(stderr, "  linear count=%zu ac count=%zu\n", linOff.size(), acOff.size());
        for (uint64_t o : linOff) if (acOff.find(o) == acOff.end())
            std::fprintf(stderr, "  only-linear: %llu\n", (unsigned long long)o);
        for (uint64_t o : acOff) if (linOff.find(o) == linOff.end())
            std::fprintf(stderr, "  only-ac:     %llu\n", (unsigned long long)o);
    }
    bool allFound = true;
    for (auto& s : specs) {
        if (linOff.find(s.off) == linOff.end()) {
            std::fprintf(stderr, "  MISSING planted %s at off=%zu (0x%zx)\n",
                         s.label.c_str(), s.off, s.off);
            allFound = false;
        }
    }
    check(allFound, "all planted 512-aligned magics discovered");
    check(linOff.find(1234) == linOff.end(), "non-aligned magic at off=1234 ignored");

    // ── Timing: 16 MiB buffer, measure Linear vs AC ScanBatch ──
    {
        const size_t kBig = 16 * 1024 * 1024;
        std::vector<uint8_t> big(kBig, 0);
        for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(rng() & 0xFF);
        const char* pick[] = {"FFD8FFE0","89504E47","504B0304","25504446",
                              "D0CF11E0A1B11AE1","49492A00","6674797069736F6D","474946383961"};
        for (size_t k = 0; k < 500; ++k) {
            size_t off = (static_cast<size_t>(rng()) % (kBig / 512)) * 512;
            Plant(big, off, pick[k % 8]);
        }
        std::vector<SignatureDB::ScanCandidate> tmp;
        auto t0 = std::chrono::steady_clock::now();
        SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::Linear);
        db.ScanBatch(big.data(), big.size(), 0, 512, tmp);
        auto tL = std::chrono::steady_clock::now();
        SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::AC);
        db.ScanBatch(big.data(), big.size(), 0, 512, tmp);
        auto tA = std::chrono::steady_clock::now();
        double mL = std::chrono::duration<double, std::milli>(tL - t0).count();
        double mA = std::chrono::duration<double, std::milli>(tA - tL).count();
        // speedup = linear/time ÷ ac/time. 1.0 == equal; >1 means AC is faster.
        double speedup = (mA > 0) ? mL / mA : 0.0;
        const char* verdict = (speedup > 1.05) ? "AC faster"
                              : (speedup < 0.95) ? "AC slower" : "parity";
        std::fprintf(stdout,
                     "timing: linear=%.1fms ac=%.1fms speedup=%.2fx (%s) hits=%zu\n",
                     mL, mA, speedup, verdict, tmp.size());
    }

    // ── Timing (production pattern): many small 64 KiB batches ──
    // RawScan calls ScanBatch once per 64 KiB batch. The batched AC pass replaces
    // the legacy per-stride point-lookup, so its advantage grows with the number
    // of batches (call overhead is amortized). This mirrors the real workload far
    // better than a single 16 MiB buffer.
    {
        const size_t kBatch = 64 * 1024;
        const size_t kNBatches = 512;
        std::vector<uint8_t> big(kBatch * kNBatches, 0);
        for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(rng() & 0xFF);
        const char* pick[] = {"FFD8FFE0","89504E47","504B0304","25504446",
                              "D0CF11E0A1B11AE1","49492A00","6674797069736F6D","474946383961"};
        for (size_t k = 0; k < 2000; ++k) {
            size_t off = (static_cast<size_t>(rng()) % (big.size() / 512)) * 512;
            Plant(big, off, pick[k % 8]);
        }
        std::vector<SignatureDB::ScanCandidate> tmp;
        size_t totalHits = 0;
        auto t0 = std::chrono::steady_clock::now();
        SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::Linear);
        for (size_t b = 0; b < kNBatches; ++b) {
            db.ScanBatch(big.data() + b * kBatch, kBatch, b * kBatch, 512, tmp);
            totalHits += tmp.size();
        }
        auto tL = std::chrono::steady_clock::now();
        SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::AC);
        for (size_t b = 0; b < kNBatches; ++b) {
            db.ScanBatch(big.data() + b * kBatch, kBatch, b * kBatch, 512, tmp);
            totalHits += tmp.size();
        }
        auto tA = std::chrono::steady_clock::now();
        double mL = std::chrono::duration<double, std::milli>(tL - t0).count();
        double mA = std::chrono::duration<double, std::milli>(tA - tL).count();
        double speedup = (mA > 0) ? mL / mA : 0.0;
        const char* verdict = (speedup > 1.05) ? "AC faster"
                              : (speedup < 0.95) ? "AC slower" : "parity";
        std::fprintf(stdout,
                     "timing(64k x%zu): linear=%.1fms ac=%.1fms speedup=%.2fx (%s) hits=%zu\n",
                     kNBatches, mL, mA, speedup, verdict, totalHits);

        // ── Timing: legacy per-stride point-lookup (pre-ScanBatch RawScan) ──
        // This is the pattern ScanBatch replaces in RawScan: one m_db->Match()
        // call per 512-byte probe. Shows the real-world speedup of the batched
        // design (amortized call overhead dominates the old per-stride path).
        {
            size_t legacyHits = 0;
            auto t1 = std::chrono::steady_clock::now();
            SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::AC);
            for (size_t b = 0; b < kNBatches; ++b) {
                const uint8_t* base = big.data() + b * kBatch;
                for (size_t i = 0; i + 8 <= kBatch; i += 512) {
                    const FileSignature* sig = db.Match(base + i, kBatch - i);
                    if (sig) ++legacyHits;
                }
            }
            auto tG = std::chrono::steady_clock::now();
            double mLegacy = std::chrono::duration<double, std::milli>(tG - t1).count();
            double realSpeedup = (mLegacy > 0) ? mLegacy / mA : 0.0;
            std::fprintf(stdout,
                         "timing(legacy stride, 64k x%zu): legacy=%.1fms batched-ac=%.1fms speedup=%.2fx hits=%zu\n",
                         kNBatches, mLegacy, mA, realSpeedup, legacyHits);
        }
    }

    // ── Test 2: end-to-end RawScan::Run identical under Linear vs AC ──
    const char* tmpPath = "rawscan_ac_test_tmp.dd";
    {
        FILE* f = std::fopen(tmpPath, "wb");
        if (f) { std::fwrite(img.data(), 1, img.size(), f); std::fclose(f); }
    }
    if (std::FILE* f = std::fopen(tmpPath, "rb")) {
        std::fclose(f);
        SectorReader reader;
        std::wstring wpath(tmpPath, tmpPath + std::strlen(tmpPath));
        if (reader.OpenImage(wpath)) {
            Partition part;
            part.startSector  = 0;
            part.totalSectors = kImgSize / 512;
            part.sizeBytes    = kImgSize;

            SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::Linear);
            RawScan rsLinear(&db);
            ScanResult rLin = rsLinear.Run(reader, part);
            std::set<uint64_t> linOff2;
            for (auto& fn : rLin.files) linOff2.insert(fn.rawOffset);

            SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::AC);
            RawScan rsAC(&db);
            ScanResult rAC = rsAC.Run(reader, part);
            std::set<uint64_t> acOff2;
            for (auto& fn : rAC.files) acOff2.insert(fn.rawOffset);

            check(linOff2 == acOff2,
                  "RawScan::Run discovered offsets identical (Linear vs AC)");
            if (linOff2 != acOff2) {
                std::fprintf(stderr, "  lin=%zu ac=%zu\n", linOff2.size(), acOff2.size());
                // dump symmetric difference for debugging
                for (uint64_t o : linOff2) if (acOff2.find(o) == acOff2.end())
                    std::fprintf(stderr, "  only-linear: %llu\n", (unsigned long long)o);
                for (uint64_t o : acOff2) if (linOff2.find(o) == linOff2.end())
                    std::fprintf(stderr, "  only-ac:     %llu\n", (unsigned long long)o);
            }
        } else {
            std::fprintf(stdout, "note: RawScan integration SKIP (OpenImage unavailable)\n");
        }
        std::remove(tmpPath);
    }

    SignatureDB::SetMatchImplForTesting(SignatureDB::MatchImpl::AC); // restore default
    if (failures == 0) std::fprintf(stdout, "\nALL RAWSCAN-AC CHECKS PASSED\n");
    else               std::fprintf(stderr, "\n%d RAWSCAN-AC CHECK(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
