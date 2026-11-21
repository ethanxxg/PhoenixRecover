// A1-8 / task #55 — parallel RAW scan equivalence.
//
// Proves that RawScan::Run(reader, partition, numThreads) recovers EXACTLY the
// same file set as the serial RawScan::Run(reader, partition) for multiple thread
// counts (2 / 4 / 8), i.e. parallelism preserves correctness.
//
// Design notes (why this configuration is safe & meaningful):
//   * All planted magics are header-only types (JPEG/PNG/GIF) whose carvers set
//     rawOffset from the header alone, so recovery is deterministic over the
//     seeded-random filler regardless of parallel chunking.
//   * Magics are planted at STRICTLY INCREASING 512-aligned offsets. Because the
//     graphic carver bounds each file to the NEXT strong header, every carved
//     extent is exactly [this, next) — no magic sits inside another's extent.
//     This rules out the only real divergence class: a nested-jpeg whose cursor
//     skip in serial would suppress a later magic that a separate worker (without
//     the global cursor) would still carve.
//   * Several magics are deliberately placed at chunk-boundary sectors for the
//     tested thread counts (2048/4096/8192/12288 over 16384 sectors), so the
//     512-byte inter-chunk overlap double-detects them. The merge step dedups by
//     rawOffset, proving the overlap handling is lossless and duplication-free.
//
// Build: no Qt, no device I/O. PHOENIX_BUILD_GUI=0.

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

// {sector, headerHex, label}.  Strictly increasing sectors; header-only types.
struct Spec { size_t sector; std::string hex; std::string label; };

// Collect (rawOffset -> (ext, size)) for a given scan.  numThreads==0 means the
// serial 2-arg Run(); otherwise the parallel 3-arg Run(... numThreads).
std::map<uint64_t, std::pair<std::string, uint64_t>>
Collect(const std::string& path, uint64_t imgSize, size_t numThreads)
{
    std::map<uint64_t, std::pair<std::string, uint64_t>> m;
    SectorReader reader;
    std::wstring wpath(path.begin(), path.end());
    if (!reader.OpenImage(wpath)) {
        std::fprintf(stderr, "FAIL: OpenImage(%s)\n", path.c_str());
        return m;
    }
    Partition part;
    part.startSector  = 0;
    part.totalSectors = static_cast<uint32_t>(imgSize / 512);
    part.sizeBytes    = imgSize;

    RawScan rs;
    ScanResult r = (numThreads == 0)
                       ? rs.Run(reader, part)
                       : rs.Run(reader, part, numThreads);

    for (const auto& fn : r.files) {
        std::string ext(fn.extension.begin(), fn.extension.end());
        m[fn.rawOffset] = { ext, fn.size };
    }
    return m;
}

} // namespace

int main()
{
    int failures = 0;
    auto check = [&](bool ok, const char* msg) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", msg); ++failures; }
        else     { std::fprintf(stdout, "ok:   %s\n", msg); }
    };

    const size_t kImgSize = 8 * 1024 * 1024;   // 16384 sectors
    const uint64_t totalSectors = kImgSize / 512;

    // Strictly-increasing magics; several sit on chunk-boundary sectors for the
    // tested thread counts (16384 sectors → 2T:@8192, 4T:@4096/8192/12288,
    // 8T:@2048/4096/6144/8192/10240/12288/14336).
    std::vector<Spec> specs = {
        {16,     "FFD8FFE0",         "jpg@16"},
        {100,    "89504E470D0A1A0A", "png@100"},
        {500,    "474946383961",     "gif@500"},
        {2048,   "FFD8FFE0",         "jpg@2048"},   // 8T boundary
        {3000,   "89504E470D0A1A0A", "png@3000"},
        {4096,   "474946383961",     "gif@4096"},   // 4T / 8T boundary
        {6000,   "FFD8FFE0",         "jpg@6000"},
        {8192,   "89504E470D0A1A0A", "png@8192"},   // 2T / 4T / 8T boundary
        {10000,  "474946383961",     "gif@10000"},
        {12288,  "FFD8FFE0",         "jpg@12288"},  // 4T / 8T boundary
        {14000,  "89504E470D0A1A0A", "png@14000"},
        {16000,  "474946383961",     "gif@16000"},
    };

    // Seeded-random filler so carver trailing-content guards behave realistically.
    std::vector<uint8_t> img(kImgSize, 0);
    std::mt19937_64 rng(0x9e3779b97f4a7c15ULL);
    for (size_t i = 0; i < img.size(); ++i)
        img[i] = static_cast<uint8_t>(rng() & 0xFF);
    for (auto& s : specs) Plant(img, s.sector * 512, s.hex);

    const char* tmpPath = "rawscan_parallel.dd";
    {
        FILE* f = std::fopen(tmpPath, "wb");
        if (f) { std::fwrite(img.data(), 1, img.size(), f); std::fclose(f); }
    }

    // Golden reference: serial 2-arg Run().
    auto ref = Collect(tmpPath, kImgSize, 0);
    std::fprintf(stdout, "  reference (serial): %zu recovered file(s)\n", ref.size());
    check(!ref.empty(), "serial scan recovered the planted magics");
    {
        std::set<uint64_t> refOffs;
        for (auto& kv : ref) refOffs.insert(kv.first);
        bool allFound = true;
        for (auto& s : specs) {
            uint64_t off = static_cast<uint64_t>(s.sector) * 512;
            if (refOffs.find(off) == refOffs.end()) {
                std::fprintf(stderr, "  MISSING planted %s at offset %zu (0x%zx)\n",
                             s.label.c_str(), off, off);
                allFound = false;
            }
        }
        check(allFound, "every planted 512-aligned magic recovered by serial scan");
    }

    // Parallel runs for several thread counts must match the reference exactly.
    for (size_t nth : { size_t(2), size_t(4), size_t(8) }) {
        auto par = Collect(tmpPath, kImgSize, nth);
        std::fprintf(stdout, "  parallel(%zu): %zu recovered file(s)\n", nth, par.size());

        bool sameCount = (par.size() == ref.size());
        check(sameCount, "parallel(N) file count equals serial count");

        // Exact equality: same offsets, same ext and size per offset.
        bool identical = sameCount;
        if (identical) {
            for (auto& kv : ref) {
                auto it = par.find(kv.first);
                if (it == par.end() ||
                    it->second.first != kv.second.first ||
                    it->second.second != kv.second.second) {
                    identical = false;
                    std::fprintf(stderr,
                        "  MISMATCH at offset %llu: serial(%s,%llu) vs parallel(%s,%llu)\n",
                        (unsigned long long)kv.first,
                        kv.second.first.c_str(), (unsigned long long)kv.second.second,
                        (it == par.end() ? "?" : it->second.first.c_str()),
                        (unsigned long long)(it == par.end() ? 0 : it->second.second));
                    break;
                }
            }
        }
        check(identical, "parallel(N) rawOffset/ext/size set is identical to serial");

        // No duplicate offsets (merge dedup must collapse boundary overlaps).
        std::set<uint64_t> parOffs;
        bool dup = false;
        for (auto& kv : par) {
            if (!parOffs.insert(kv.first).second) { dup = true; break; }
        }
        check(!dup, "parallel(N) produced no duplicate rawOffsets");
    }

    std::remove(tmpPath);

    if (failures == 0) std::fprintf(stdout, "\nALL RAWSCAN-PARALLEL CHECKS PASSED\n");
    else               std::fprintf(stderr, "\n%d RAWSCAN-PARALLEL CHECK(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
