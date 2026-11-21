// W8 / CAR-002 — quantify the effect of an OPT-IN strict header-structure
// gate on RAW-carving false positives.
//
// RawScanFalsePositive (W5) measured 115 FP on 8 MiB of noise (14.38 FP/MiB,
// precision 12.80%) using HEADER-ONLY planted magics.  That corpus cannot be
// used to evaluate a structural gate: the planted "true positives" themselves
// have no structure, so a strict check would (correctly) reject them too.
//
// This test therefore builds a corpus with two planted groups:
//   * STRUCTURED (16): a magic PLUS the minimum legal header structure
//     (PNG IHDR, GIF logical screen descriptor, JPEG APP0/JFIF segment).
//     These are the true positives and must survive the strict gate.
//   * BARE (16): the same magics with random bytes after them.  These are
//     deliberate weak matches that the strict gate is expected to reject.
// Background is the same seeded random noise as W5 (seed 0xC0FFEE), so the
// baseline FP/MiB stays comparable.
//
// Strict validation is OFF by default; enabling it is an explicit caller
// decision (RawScan::SetStrictHeaderValidation(true)).  No production caller
// enables it yet — see PERFORMANCE_BASELINE.md §10.

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

constexpr size_t   kImageSize = 8 * 1024 * 1024;   // 16384 sectors
constexpr uint32_t kSeed      = 0xC0FFEEu;          // same noise as W5

struct Plant
{
    size_t            sector;
    std::vector<uint8_t> bytes;
};

const uint8_t kPngStructured[] = {
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,   // PNG magic
    0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,   // IHDR length + type
    0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,   // width=1 height=1
    0x08,0x02,0x00,0x00,0x00,                  // bit depth / colour / ...
    0x00,0x00,0x00,0x00                        // CRC (not validated)
};
const uint8_t kPngBare[] = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A};

const uint8_t kGifStructured[] = {
    0x47,0x49,0x46,0x38,0x39,0x61,             // "GIF89a"
    0x01,0x00,0x01,0x00,                       // width=1, height=1
    0x00,0x00,0x00                             // packed, bg, aspect
};
const uint8_t kGifBare[] = {0x47,0x49,0x46,0x38,0x39,0x61};

const uint8_t kJpgStructured[] = {
    0xFF,0xD8,0xFF,0xE0,                       // SOI + APP0
    0x00,0x10,                                 // segment length 16
    0x4A,0x46,0x49,0x46,0x00,                 // "JFIF\0"
    0x01,0x01,0x00,0x00,0x01,0x00,0x01,0x00,0x00
};
const uint8_t kJpgBare[] = {0xFF,0xD8,0xFF,0xE0};

template <size_t N>
std::vector<uint8_t> Arr(const uint8_t (&a)[N])
{
    return std::vector<uint8_t>(a, a + N);
}

const size_t kStructuredSectors[] = {
      100,  300,  500,  700,  900, 1100, 2048, 4096,
     8192,12288,13000,14000,15000,15500,16000,16300
};
const size_t kBareSectors[] = {
      150,  350,  550,  750,  950, 1150, 2100, 4200,
     8300,12300,13100,14100,15100,15600,16100,16350
};

std::vector<Plant> BuildPlants(bool structured)
{
    const std::vector<uint8_t> png = structured ? Arr(kPngStructured) : Arr(kPngBare);
    const std::vector<uint8_t> gif = structured ? Arr(kGifStructured) : Arr(kGifBare);
    const std::vector<uint8_t> jpg = structured ? Arr(kJpgStructured) : Arr(kJpgBare);
    const size_t* sectors = structured ? kStructuredSectors : kBareSectors;
    const size_t count = structured
        ? sizeof(kStructuredSectors) / sizeof(kStructuredSectors[0])
        : sizeof(kBareSectors) / sizeof(kBareSectors[0]);
    std::vector<Plant> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const std::vector<uint8_t>& bytes =
            (i % 3 == 0) ? jpg : (i % 3 == 1 ? png : gif);
        out.push_back({sectors[i], bytes});
    }
    return out;
}

std::vector<uint8_t> MakeImage()
{
    std::vector<uint8_t> img(kImageSize);
    std::mt19937 rng(kSeed);
    for (auto& b : img) b = static_cast<uint8_t>(rng() & 0xFF);
    for (const auto& p : BuildPlants(true))
        std::memcpy(img.data() + p.sector * 512, p.bytes.data(), p.bytes.size());
    for (const auto& p : BuildPlants(false))
        std::memcpy(img.data() + p.sector * 512, p.bytes.data(), p.bytes.size());
    return img;
}

std::wstring WriteImage(const std::vector<uint8_t>& img)
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"PhoenixRecover_strict_fp.dd";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    DWORD written = 0;
    const BOOL ok = WriteFile(h, img.data(), static_cast<DWORD>(img.size()),
                              &written, nullptr) && written == img.size();
    CloseHandle(h);
    if (!ok) { DeleteFileW(path.c_str()); return L""; }
    return path;
}

std::set<uint64_t> ScanOffsets(const std::wstring& path, bool strict)
{
    std::set<uint64_t> offsets;
    SectorReader reader;
    if (!reader.OpenImage(path)) return offsets;
    Partition part;
    part.startSector  = 0;
    part.totalSectors = static_cast<uint32_t>(kImageSize / 512);
    part.sizeBytes    = kImageSize;
    RawScan rs;
    if (strict) rs.SetStrictHeaderValidation(true);
    const ScanResult r = rs.Run(reader, part);
    for (const auto& fn : r.files)
        if (fn.rawOffset != 0) offsets.insert(fn.rawOffset);
    reader.Close();
    return offsets;
}

std::set<uint64_t> PlantedOffsets(bool structured)
{
    const size_t* sectors = structured ? kStructuredSectors : kBareSectors;
    const size_t count = structured
        ? sizeof(kStructuredSectors) / sizeof(kStructuredSectors[0])
        : sizeof(kBareSectors) / sizeof(kBareSectors[0]);
    std::set<uint64_t> s;
    for (size_t i = 0; i < count; ++i)
        s.insert(static_cast<uint64_t>(sectors[i]) * 512);
    return s;
}

struct Metrics
{
    size_t tp = 0;
    size_t bareHits = 0;
    size_t fp = 0;
    size_t fn = 0;
    size_t reported = 0;
    double precision = 0.0;
    double recall = 0.0;
};

Metrics Measure(const std::set<uint64_t>& reported,
                const std::set<uint64_t>& structured,
                const std::set<uint64_t>& bare)
{
    Metrics m;
    m.reported = reported.size();
    for (uint64_t off : reported) {
        if (structured.count(off))      ++m.tp;
        else if (bare.count(off))       ++m.bareHits;
        else                            ++m.fp;
    }
    for (uint64_t off : structured)
        if (!reported.count(off)) ++m.fn;
    m.precision = m.reported ? 100.0 * m.tp / m.reported : 0.0;
    m.recall    = structured.empty() ? 0.0
                  : 100.0 * m.tp / structured.size();
    return m;
}

} // namespace

int main()
{
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* msg) {
        ++checks;
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", msg); }
        else     { std::printf("ok:   %s\n", msg); }
    };

    // Default must remain OFF — this is the backwards-compatibility contract.
    RawScan probe;
    check(!probe.GetStrictHeaderValidation(),
          "strict header validation is OFF by default (zero behaviour change)");

    const std::wstring image = WriteImage(MakeImage());
    check(!image.empty(), "write seeded-noise image with structured + bare plants");
    if (image.empty()) return 1;

    const auto structured = PlantedOffsets(true);
    const auto bare       = PlantedOffsets(false);

    const auto normal = ScanOffsets(image, false);
    const auto strict = ScanOffsets(image, true);
    const auto replay = ScanOffsets(image, true);

    const Metrics mn = Measure(normal, structured, bare);
    const Metrics ms = Measure(strict,  structured, bare);

    const double mb = kImageSize / (1024.0 * 1024.0);
    std::printf("\n--- W8 / CAR-002 strict-header false-positive comparison ---\n");
    std::printf("mode,image_MiB,seed,structured,bare,reported,TP,bare_hits,FP,FN,precision_pct,recall_pct,FP_per_MiB\n");
    std::printf("default,%.1f,%u,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%.2f,%.2f,%.2f\n",
                mb, kSeed, structured.size(), bare.size(), mn.reported,
                mn.tp, mn.bareHits, mn.fp, mn.fn, mn.precision, mn.recall,
                (mn.fp + mn.bareHits) / mb);
    std::printf("strict,%.1f,%u,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%.2f,%.2f,%.2f\n",
                mb, kSeed, structured.size(), bare.size(), ms.reported,
                ms.tp, ms.bareHits, ms.fp, ms.fn, ms.precision, ms.recall,
                (ms.fp + ms.bareHits) / mb);

    check(strict == replay, "strict-mode result is deterministic across replays");
    check(mn.tp == structured.size(),
          "DEFAULT mode recovers every structured true positive");
    check(ms.tp == structured.size(),
          "STRICT mode keeps every structured true positive (recall preserved)");
    check(ms.fn == 0, "STRICT mode introduces no false negatives");
    check(mn.bareHits >= bare.size() / 2,
          "DEFAULT mode reports most bare-magic weak matches");
    // Strict mode cannot promise to reject EVERY bare plant: (a) format rules
    // are intentionally conservative (the GIF rule only sees "GIF89a" + dims,
    // which noise satisfies ~100% of the time; JPEG's segment length passes
    // ~6% by coincidence) and (b) the 513-entry DB may emit a DIFFERENT
    // signature at the same offset under an uncovered extension that falls
    // through to "legacy behaviour".  The defensible guarantee is that the
    // strict gate NEVER increases weak matches and strictly REDUCES them.
    check(ms.bareHits < mn.bareHits,
          "STRICT mode strictly reduces bare-magic weak matches");
    check(ms.fp + ms.bareHits < mn.fp + mn.bareHits,
          "STRICT mode strictly reduces total false positives");
    check(ms.precision > mn.precision,
          "STRICT mode improves precision");
    check(normal.size() >= ms.reported,
          "STRICT mode never emits more nodes than default");

    std::printf("delta: FP+bare %zu -> %zu (-%.1f%%), precision %.2f%% -> %.2f%%\n\n",
                mn.fp + mn.bareHits, ms.fp + ms.bareHits,
                100.0 * (static_cast<double>(mn.fp + mn.bareHits) -
                         static_cast<double>(ms.fp + ms.bareHits)) /
                    (mn.fp + mn.bareHits ? mn.fp + mn.bareHits : 1),
                mn.precision, ms.precision);

    DeleteFileW(image.c_str());
    std::printf("%s: RawScanStrictHeaders %d checks, %d failure(s)\n",
                failures ? "FAIL" : "OK", checks, failures);
    return failures ? 1 : 0;
}
