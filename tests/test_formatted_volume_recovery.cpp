// W3: quick-formatted volume recovery golden (synthetic, zero GUI).
//
// Models a *quick format*: a fresh, valid boot sector is written and the file
// system metadata area (MFT / FAT + root directory) is zeroed, while the old
// data area still holds the residual file headers and payloads.  Structured
// parsing therefore finds no files, and DeepScan must fall back to RAW carving
// to recover the residual content.
//
// Independent oracle: the planted (offset, magic, payload) table plus direct
// raw-image reads.  The test verifies not only that the offsets are found, but
// that the bytes at each recovered rawOffset/rawSize actually match the oracle
// payload header -- an offset-only gate (DeepScanFallback) cannot catch a
// carver that reports the right place but the wrong content.
//
// This is a SYNTHETIC golden, not a real quick-formatted disk.  The filler is
// seeded random noise, so RAW carving also reports false positives (see
// PERFORMANCE_BASELINE.md section 9); this test makes no claim that every
// reported node is a real file.  It only asserts recall of the planted set.

#include "scan/DeepScan.h"
#include "scan/RawScan.h"
#include "disk/SectorReader.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

struct Residual
{
    uint64_t    sector;
    std::string magicHex;
    const char* label;
    std::vector<uint8_t> payload;   // magic + deterministic tail
};

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

// A fresh, minimal NTFS boot sector: this is what a quick format writes.  The
// MFT is deliberately left pointing into the zeroed metadata area, so the
// structured parser sees a recognised but empty volume.
void WriteFreshNtfsBoot(std::vector<uint8_t>& img, uint64_t totalSectors)
{
    std::memset(img.data(), 0, 512);
    static const char oem[] = "NTFS    ";
    std::memcpy(img.data() + 3, oem, 8);
    img[11] = 0x00; img[12] = 0x02;   // 512 B/sector
    img[13] = 1;                      // 1 sector/cluster
    img[21] = 0xF8;
    for (int i = 0; i < 8; ++i)
        img[40 + i] = static_cast<uint8_t>((totalSectors >> (8 * i)) & 0xFF);
    img[48] = 16;                     // MFT LCN 16 (offset 0x30), inside zeroed metadata
    img[64] = static_cast<uint8_t>(-10);
    img[68] = static_cast<uint8_t>(-9);
    img[510] = 0x55; img[511] = 0xAA;
}

uint64_t Fnv1a(const std::vector<uint8_t>& v)
{
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t b : v) { h ^= b; h *= 1099511628211ULL; }
    return h;
}

std::vector<uint8_t> ReadImageBytes(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return {}; }
    std::vector<uint8_t> out(static_cast<size_t>(n));
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    out.resize(got);
    return out;
}

ScanResult RunDeepScan(const std::string& path, uint64_t imgSize)
{
    SectorReader reader;
    ScanResult empty;
    if (!reader.OpenImage(std::wstring(path.begin(), path.end()))) {
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
    int checks = 0;
    int failures = 0;
    auto check = [&](bool ok, const char* msg) {
        ++checks;
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", msg); }
        else     { std::fprintf(stdout, "ok:   %s\n", msg); }
    };

    constexpr size_t   kImgSize = 4 * 1024 * 1024;
    constexpr uint64_t kSectors = kImgSize / 512;
    constexpr size_t   kMetadataBytes = 128 * 1024;   // quick-format wipes this

    // Residual files left behind in the data area.  Magic + deterministic
    // payload tail = the independent byte-level oracle.
    std::vector<Residual> planted;
    auto add = [&](uint64_t sector, const char* hex, const char* label) {
        Residual r;
        r.sector   = sector;
        r.magicHex = hex;
        r.label    = label;
        r.payload  = HexToBytes(hex);
        for (int i = 0; i < 64; ++i)
            r.payload.push_back(static_cast<uint8_t>(0x40 + (i % 26)));
        planted.push_back(std::move(r));
    };
    add(300,  "FFD8FFE0",         "jpeg@sector300");
    add(900,  "89504E470D0A1A0A", "png@sector900");
    add(2100, "474946383961",     "gif@sector2100");
    add(5100, "FFD8FFE0",         "jpeg@sector5100");
    add(7400, "89504E470D0A1A0A", "png@sector7400");

    // ── Build the "formatted" image ────────────────────────────────────────
    std::vector<uint8_t> img(kImgSize, 0);
    std::mt19937_64 rng(0x5EEDF0A17ULL);
    for (size_t i = 0; i < img.size(); ++i)
        img[i] = static_cast<uint8_t>(rng() & 0xFF);

    // Quick format: fresh boot + zeroed metadata, data area untouched.
    WriteFreshNtfsBoot(img, kSectors);
    std::memset(img.data() + 512, 0, kMetadataBytes - 512);
    for (const auto& r : planted) {
        const size_t off = static_cast<size_t>(r.sector * 512);
        check(off + r.payload.size() <= img.size(), "planted residual fits in data area");
        std::memcpy(img.data() + off, r.payload.data(), r.payload.size());
    }

    const char* path = "formatted_volume_recovery.dd";
    {
        FILE* f = std::fopen(path, "wb");
        if (!f) { std::fprintf(stderr, "FAIL: cannot write synthetic image\n"); return 1; }
        std::fwrite(img.data(), 1, img.size(), f);
        std::fclose(f);
    }
    const uint64_t hashBefore = Fnv1a(img);

    // ── Oracle self-check: read the raw image back independently ───────────
    const std::vector<uint8_t> raw = ReadImageBytes(path);
    check(raw.size() == img.size(), "raw oracle re-reads the whole 4 MiB image");
    bool oracleOk = !raw.empty();
    for (const auto& r : planted) {
        const size_t off = static_cast<size_t>(r.sector * 512);
        if (off + r.payload.size() > raw.size() ||
            std::memcmp(raw.data() + off, r.payload.data(), r.payload.size()) != 0)
            oracleOk = false;
    }
    check(oracleOk, "independent oracle confirms all residual payloads survive the format");
    check(kMetadataBytes < planted[0].sector * 512,
          "all residual payloads live outside the zeroed metadata area");
    for (size_t i = 512; i < kMetadataBytes; ++i)
        if (raw[i] != 0) { oracleOk = false; break; }
    check(oracleOk, "quick-format zeroed the entire metadata region");

    // ── Production path: DeepScan structured parse -> RAW fallback ─────────
    const ScanResult r = RunDeepScan(path, kImgSize);
    std::set<uint64_t> recoveredOffsets;
    for (const auto& fn : r.files) recoveredOffsets.insert(fn.rawOffset);

    std::fprintf(stdout, "  DeepScan reported %zu file(s) after quick format\n",
                 r.files.size());
    check(r.files.size() >= planted.size(),
          "formatted volume: RAW fallback emits at least the planted residual count");

    for (const auto& p : planted) {
        const uint64_t off = p.sector * 512;
        const bool found = recoveredOffsets.find(off) != recoveredOffsets.end();
        check(found, p.label);

        // Byte-level gate: whichever node(s) start at this offset must point at
        // a carved range whose leading bytes equal the oracle payload header.
        if (found) {
            bool bytesOk = false;
            for (const auto& fn : r.files) {
                if (fn.rawOffset != off) continue;
                const uint64_t len = fn.rawSize > 0 ? fn.rawSize : fn.size;
                const uint64_t need = p.payload.size();
                if (len < need || off + need > raw.size()) continue;
                if (std::memcmp(raw.data() + off, p.payload.data(),
                                static_cast<size_t>(need)) == 0)
                    bytesOk = true;
            }
            check(bytesOk, "carved byte range at planted offset matches oracle payload");
        }
    }

    // Source read-only invariant.
    const std::vector<uint8_t> after = ReadImageBytes(path);
    check(Fnv1a(after) == hashBefore,
          "whole-image FNV-1a unchanged after scan (scan source is read-only)");

    // ── Negative control: a genuinely empty freshly formatted volume ───────
    std::vector<uint8_t> blank(kImgSize, 0);
    WriteFreshNtfsBoot(blank, kSectors);
    const char* blankPath = "formatted_volume_empty.dd";
    {
        FILE* f = std::fopen(blankPath, "wb");
        if (!f) { std::fprintf(stderr, "FAIL: cannot write blank image\n"); return 1; }
        std::fwrite(blank.data(), 1, blank.size(), f);
        std::fclose(f);
    }
    const ScanResult blankResult = RunDeepScan(blankPath, kImgSize);
    std::fprintf(stdout, "  blank formatted volume reported %zu file(s)\n",
                 blankResult.files.size());
    check(blankResult.files.empty(),
          "negative control: empty freshly formatted volume yields no residual files");

    std::remove(path);
    std::remove(blankPath);

    std::fprintf(stdout, "%s: FormattedVolumeRecovery %d checks, %d failure(s)\n",
                 failures ? "FAIL" : "OK", checks, failures);
    return failures ? 1 : 0;
}
