// A1-7 / task #51 — DeepScan damaged-NTFS fallback → SignatureMatcher RAW carving.
//
// Proves that when DeepScan recognises a partition as a known file system
// (NTFS boot sector present) but its *structured* parse recovers almost nothing
// (the classic "damaged / overwritten MFT or directory" case), it transparently
// falls back to content-based RAW carving via the verified SignatureMatcher path
// (RawScan::Run) and recovers the embedded file magics.
//
// Contrast case: the SAME embedded magics WITHOUT a recognised FS boot sector are
// NOT recovered by DeepScan on its own (no fallback fired) — this isolates the
// fallback trigger precisely and proves it is gated on FS recognition.
//
// Build: no Qt, no device I/O. PHOENIX_BUILD_GUI=0.

#include "scan/DeepScan.h"
#include "scan/RawScan.h"
#include "disk/SectorReader.h"

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

// Overwrite the first 512 bytes with a *minimal but valid* NTFS boot sector so
// NtfsParser::Open() succeeds (oemId="NTFS    ", bytesPerSector, sectorsPerCluster).
// The MFT location is intentionally left pointing at garbage — Open() does NOT
// read the MFT, so the volume is "recognised" yet its structure is unreadable,
// which is exactly the condition that should trigger the RAW fallback.
void PlantNtfsBootSector(std::vector<uint8_t>& img, uint64_t totalSectors)
{
    std::memset(img.data(), 0, 512);
    static const char oem[] = "NTFS    ";             // offset 3..10
    std::memcpy(img.data() + 3, oem, 8);
    img[11] = 0x00; img[12] = 0x02;                   // bytesPerSector = 512 (LE)
    img[13] = 1;                                      // sectorsPerCluster = 1
    img[21] = 0xF8;                                   // mediaDescriptor
    for (int i = 0; i < 8; ++i)                       // totalSectors (LE uint64) @40
        img[40 + i] = static_cast<uint8_t>((totalSectors >> (8 * i)) & 0xFF);
    img[64] = static_cast<uint8_t>(-10);              // clustersPerRecord=-10 → recordSize 1024
    img[68] = static_cast<uint8_t>(-9);               // clustersPerIndex=-9
    img[510] = 0x55; img[511] = 0xAA;                // boot signature
}

ScanResult RunDeepScanOnImage(const std::string& path, uint64_t imgSize)
{
    SectorReader reader;
    std::wstring wpath(path.begin(), path.end());
    ScanResult empty;
    if (!reader.OpenImage(wpath)) {
        std::fprintf(stderr, "FAIL: OpenImage(%s)\n", path.c_str());
        return empty;
    }
    Partition part;
    part.startSector  = 0;
    part.totalSectors = static_cast<uint32_t>(imgSize / 512);
    part.sizeBytes    = imgSize;
    DeepScan ds;
    return ds.Run(reader, part);
}

} // namespace

int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* msg) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", msg); ++failures; }
        else     { std::fprintf(stdout, "ok:   %s\n", msg); }
    };

    const size_t kImgSize = 4 * 1024 * 1024;
    const uint64_t totalSectors = kImgSize / 512;

    // Planted magics: {sector, headerHex, label}. All 512-aligned, all header-only
    // signatures whose carvers set rawOffset on the header alone (JPEG/PNG/GIF) so
    // RawScan recovers them deterministically even over random garbage. (Structure-
    // sensitive types like TIFF/CFB/PDF need valid trailing content to validate, and
    // are intentionally out of scope here — they are covered by the SignatureDB /
    // RawScan equivalence tests. The fallback simply inherits RawScan's carve policy.)
    // Offsets span both the low and high halves of the image to rule out any
    // offset-dependent gap in the RawScan hand-off.
    struct Spec { size_t sector; std::string hex; std::string label; };
    std::vector<Spec> specs = {
        {10,   "FFD8FFE0",         "jpg@10"},
        {100,  "89504E470D0A1A0A", "png@100"},
        {500,  "474946383961",     "gif@500"},
        {2000, "FFD8FFE0",         "jpg@2000"},
        {4000, "89504E470D0A1A0A", "png@4000"},
    };

    // ── Positive: valid NTFS boot + magics → fallback carves them ──────────────
    {
        // Pseudo-random garbage filler (seeded for reproducibility). This mirrors a
        // real damaged volume far better than a constant byte: some carvers (e.g.
        // pdf's "%%EOF" trailing check, OLE2/CFB structure validation) decline to
        // emit a file over a non-representative repeating filler, which would make
        // the fallback look like it misfired when it is actually the carver guard.
        std::vector<uint8_t> img(kImgSize, 0);
        std::mt19937_64 rng(0x1234abcdULL);
        for (size_t i = 0; i < img.size(); ++i)
            img[i] = static_cast<uint8_t>(rng() & 0xFF);
        PlantNtfsBootSector(img, totalSectors);
        for (auto& s : specs) Plant(img, s.sector * 512, s.hex);

        const char* tmpPath = "deepscan_fallback_ntfs.dd";
        {
            FILE* f = std::fopen(tmpPath, "wb");
            if (f) { std::fwrite(img.data(), 1, img.size(), f); std::fclose(f); }
        }
        ScanResult r = RunDeepScanOnImage(tmpPath, kImgSize);
        std::remove(tmpPath);

        std::set<uint64_t> offs;
        for (auto& fn : r.files) offs.insert(fn.rawOffset);

        std::fprintf(stdout, "  positive: DeepScan reported %zu file(s) "
                             "(structured parse yielded 0, fallback expected)\n",
                     r.files.size());
        check(r.files.size() >= specs.size(),
              "fallback recovered at least as many files as planted magics");
        bool allFound = true;
        for (auto& s : specs) {
            uint64_t off = static_cast<uint64_t>(s.sector) * 512;
            if (offs.find(off) == offs.end()) {
                std::fprintf(stderr, "  MISSING planted %s at offset %zu (0x%zx)\n",
                             s.label.c_str(), off, off);
                allFound = false;
            }
        }
        check(allFound, "every planted 512-aligned magic recovered by the RAW fallback");
    }

    // ── Negative: SAME magics, NO recognised FS boot sector → nothing carved ──
    {
        std::vector<uint8_t> img(kImgSize, 0x11); // NO boot sector planted
        for (auto& s : specs) Plant(img, s.sector * 512, s.hex);

        const char* tmpPath = "deepscan_fallback_none.dd";
        {
            FILE* f = std::fopen(tmpPath, "wb");
            if (f) { std::fwrite(img.data(), 1, img.size(), f); std::fclose(f); }
        }
        ScanResult r = RunDeepScanOnImage(tmpPath, kImgSize);
        std::remove(tmpPath);

        std::fprintf(stdout, "  negative: DeepScan reported %zu file(s) "
                             "(no FS recognised → no fallback expected)\n",
                     r.files.size());
        check(r.files.empty(),
              "without a recognised FS, DeepScan alone carves no magics "
              "(fallback not fired)");
    }

    if (failures == 0) std::fprintf(stdout, "\nALL DEEPSCAN-FALLBACK CHECKS PASSED\n");
    else               std::fprintf(stderr, "\n%d DEEPSCAN-FALLBACK CHECK(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
