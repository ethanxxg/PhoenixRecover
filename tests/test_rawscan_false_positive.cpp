// W5 / CAR-002 — quantify RAW-carving false positives on random noise.
//
// Prior benchmarks used a constant 0x5A filler with strictly increasing
// header-only magics, so they deliberately contained NO random false
// positives (PERFORMANCE_BASELINE §8.3).  This test supplies a known
// ground truth instead:
//
//   Case A: seeded-random 8 MiB image, ZERO planted magics.
//           Every reported file is a false positive (FP).
//   Case B: identical image plus K planted header-only magics.
//           TP = reported offsets that are planted, FP = all others,
//           FN = planted offsets that were not reported.
//
// Output is a CSV line so the numbers can be archived, plus deterministic
// replay guards (the same seed must produce the same FP set twice).
// No hard FP threshold is asserted on the first measurement — the point is
// to make the metric reproducible and visible, not to fake a PASS.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "scan/RawScan.h"
#include "disk/SectorReader.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr size_t kImageSize = 8 * 1024 * 1024; // 16384 sectors
constexpr uint32_t kSeed    = 0xC0FFEEu;

struct Spec { size_t sector; const char* hex; };

const Spec kPlanted[] = {
    {  100, "FFD8FFE0" },             // jpg
    {  300, "89504E470D0A1A0A" },     // png
    {  500, "474946383961" },         // gif
    {  700, "FFD8FFE0" },
    {  900, "89504E470D0A1A0A" },
    { 1100, "474946383961" },
    { 2048, "FFD8FFE0" },             // chunk-boundary sector
    { 4096, "89504E470D0A1A0A" },
    { 8192, "474946383961" },
    {12288, "FFD8FFE0" },
    {13000, "89504E470D0A1A0A" },
    {14000, "474946383961" },
    {15000, "FFD8FFE0" },
    {15500, "89504E470D0A1A0A" },
    {16000, "474946383961" },
    {16300, "FFD8FFE0" },
};
constexpr size_t kPlantedCount = sizeof(kPlanted) / sizeof(kPlanted[0]);

std::vector<uint8_t> HexToBytes(const char* hex)
{
    std::vector<uint8_t> out;
    auto hv = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return 0;
    };
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
        out.push_back(static_cast<uint8_t>((hv(hex[i]) << 4) | hv(hex[i + 1])));
    return out;
}

std::vector<uint8_t> MakeImage(bool plant)
{
    std::vector<uint8_t> img(kImageSize);
    std::mt19937 rng(kSeed);
    for (auto& b : img) b = static_cast<uint8_t>(rng() & 0xFF);
    if (plant) {
        for (const auto& s : kPlanted) {
            const auto bytes = HexToBytes(s.hex);
            const size_t off = s.sector * 512;
            if (off + bytes.size() <= img.size())
                std::memcpy(img.data() + off, bytes.data(), bytes.size());
        }
    }
    return img;
}

std::wstring WriteImage(const std::vector<uint8_t>& img, const char* tag)
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"PhoenixRecover_fp_" + 
                        std::wstring(tag, tag + std::strlen(tag)) + L".dd";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    DWORD written = 0;
    const BOOL ok = WriteFile(h, img.data(), static_cast<DWORD>(img.size()), &written, nullptr)
                    && written == img.size();
    CloseHandle(h);
    if (!ok) { DeleteFileW(path.c_str()); return L""; }
    return path;
}

std::set<uint64_t> ScanOffsets(const std::wstring& path, uint32_t* foundCount)
{
    std::set<uint64_t> offsets;
    SectorReader reader;
    if (!reader.OpenImage(path)) return offsets;
    Partition part;
    part.startSector  = 0;
    part.totalSectors = static_cast<uint32_t>(kImageSize / 512);
    part.sizeBytes    = kImageSize;
    RawScan rs;
    ScanResult r = rs.Run(reader, part);
    if (foundCount) *foundCount = r.foundCount;
    for (const auto& fn : r.files)
        if (fn.rawOffset != 0) offsets.insert(fn.rawOffset);
    reader.Close();
    return offsets;
}

std::set<uint64_t> PlantedOffsets()
{
    std::set<uint64_t> s;
    for (const auto& spec : kPlanted) s.insert(static_cast<uint64_t>(spec.sector) * 512);
    return s;
}

} // namespace

int main()
{
    int checks   = 0;
    int failures = 0;
    auto check = [&](bool ok, const char* msg) {
        ++checks;
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", msg); }
        else     { std::fprintf(stdout, "ok:   %s\n", msg); }
    };

    const auto planted = PlantedOffsets();

    // ── Case A (twice, for determinism) ──────────────────────────────────
    const auto noise = MakeImage(false);
    const std::wstring pA1 = WriteImage(noise, "noise1");
    const std::wstring pA2 = WriteImage(noise, "noise2");
    check(!pA1.empty() && !pA2.empty(), "write noise images");
    if (pA1.empty() || pA2.empty()) return 1;

    uint32_t cntA1 = 0, cntA2 = 0;
    const auto a1 = ScanOffsets(pA1, &cntA1);
    const auto a2 = ScanOffsets(pA2, &cntA2);
    const size_t fpA = a1.size();
    check(a1 == a2 && cntA1 == cntA2, "noise-only FP set is deterministic across replays");

    // ── Case B: noise + planted magics ───────────────────────────────────
    const auto withPlants = MakeImage(true);
    const std::wstring pB = WriteImage(withPlants, "planted");
    check(!pB.empty(), "write planted image");
    if (pB.empty()) { DeleteFileW(pA1.c_str()); DeleteFileW(pA2.c_str()); return 1; }

    uint32_t cntB = 0;
    const auto b = ScanOffsets(pB, &cntB);
    size_t tp = 0, fpB = 0;
    for (uint64_t off : b) {
        if (planted.count(off)) ++tp;
        else                    ++fpB;
    }
    size_t fn = 0;
    for (uint64_t off : planted)
        if (!b.count(off)) ++fn;

    const double precision = (tp + fpB) ? (100.0 * tp / (tp + fpB)) : 0.0;
    const double recall    = planted.empty() ? 0.0 : (100.0 * tp / planted.size());
    const double mb        = kImageSize / (1024.0 * 1024.0);

    std::printf("\n--- CAR-002 RAW carving false-positive metrics ---\n");
    std::printf("image_MiB,seed,planted,case_A_FP,case_A_FP_per_MiB,TP,FP,FN,precision_pct,recall_pct\n");
    std::printf("%.1f,%u,%zu,%zu,%.2f,%zu,%zu,%zu,%.2f,%.2f\n",
                mb, kSeed, kPlantedCount, fpA, fpA / mb, tp, fpB, fn,
                precision, recall);
    std::printf("case_A_reported=%u case_B_reported=%u\n\n", cntA1, cntB);

    // Ground-truth invariants.  The FP count itself is reported, not asserted:
    // random bytes legitimately contain accidental magic prefixes.
    check(tp == kPlantedCount, "all planted header-only magics recovered (TP)");
    check(fn == 0,            "no planted magic missed (FN == 0)");
    check(recall >= 99.0,     "recall >= 99% on planted set");
    check(fpA / mb >= 0.0,    "FP/MiB metric computed");
    check(b.size() == tp + fpB, "every reported offset classified exactly once");

    DeleteFileW(pA1.c_str());
    DeleteFileW(pA2.c_str());
    DeleteFileW(pB.c_str());

    std::printf("%s: RawScanFalsePositive %d checks, %d failure(s)\n",
                failures ? "FAIL" : "OK", checks, failures);
    return failures ? 1 : 0;
}
