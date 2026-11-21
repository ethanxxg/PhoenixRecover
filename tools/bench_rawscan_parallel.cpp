// A1-11 — standalone RAW-scan parallel throughput benchmark.
//
// Measures the wall-clock cost of the production RAW carving pipeline at
// different worker counts on a deterministic synthetic image. NOT a CTest:
// timing assertions would be flaky, so this is an opt-in tool
// (cmake -DPHOENIX_BUILD_BENCH=ON) whose output is recorded by hand in
// docs/PERFORMANCE_BASELINE.md.
//
// Workload: a constant 0x5A filler (no accidental signature hits) with
// header-only graphic magics planted at fixed 256 KiB intervals. Every carved
// extent is [this, next), so no magic is nested inside another's extent and
// serial / parallel results must be identical even across chunk boundaries.
//
// Caveats (see PERFORMANCE_BASELINE.md §8): synthetic cached file, not a real
// disk; low-entropy filler avoids the false-positive-heavy workload; the
// global post-processing passes (RescanNestedGraphicHeaders / AddFragmentNodes)
// remain serial and limit the achievable speedup (Amdahl).

#include "scan/RawScan.h"
#include "scan/CarveModuleService.h"
#include "disk/SectorReader.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#endif

namespace {

struct Options
{
    size_t sizeMiB   = 128;
    size_t repeat    = 3;
    std::vector<size_t> threads{1, 2, 4, 8};
    std::string csvPath;
    std::string imagePath = "bench_rawscan_parallel.dd";
    bool keep = false;
    bool viaService = false;   // exercise the A1-10 production config chain
};

constexpr uint64_t kSectorSize   = 512;
constexpr uint64_t kMagicSpacing = 256ULL * 1024;

struct Magic { const char* hex; size_t bytes; };
const Magic kMagics[] = {
    { "\x89\x50\x4E\x47\x0D\x0A\x1A\x0A", 8 },  // png
    { "\xFF\xD8\xFF\xE0",                 4 },  // jpg
    { "\x47\x49\x46\x38\x39\x61",         6 },  // gif
};

void Usage(const char* exe)
{
    std::fprintf(stderr,
        "Usage: %s [--size-mib N] [--repeat N] [--threads 1,2,4,8]\n"
        "       [--csv path] [--image path] [--keep]\n\n"
        "  --size-mib   synthetic image size in MiB (default 128)\n"
        "  --repeat     timed repetitions per thread count; median is reported (default 3)\n"
        "  --threads    comma-separated worker counts; 1 = serial baseline (default 1,2,4,8)\n"
        "  --csv        also append a CSV result table to this path\n"
        "  --image      temporary image path (deleted unless --keep)\n"
        "  --via-service  drive the scan through CarveModuleService (A1-10 production\n"
        "               config chain) instead of calling RawScan::Run directly\n", exe);
}

bool ParseSize(const char* s, size_t& out)
{
    if (!s || !*s) return false;
    char* end = nullptr;
    unsigned long long v = std::strtoull(s, &end, 10);
    if (end == s || (end && *end != '\0') || v == 0) return false;
    out = static_cast<size_t>(v);
    return true;
}

std::vector<size_t> ParseThreads(const char* s)
{
    std::vector<size_t> out;
    std::string cur;
    for (const char* p = s;; ++p) {
        if (*p == ',' || *p == '\0') {
            size_t v = 0;
            if (ParseSize(cur.c_str(), v)) out.push_back(v);
            cur.clear();
            if (*p == '\0') break;
        } else {
            cur.push_back(*p);
        }
    }
    return out;
}

bool ParseArgs(int argc, char** argv, Options& o)
{
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            return (i + 1 < argc) ? argv[++i] : nullptr;
        };
        if (a == "--size-mib") {
            if (!ParseSize(next(), o.sizeMiB)) return false;
        } else if (a == "--repeat") {
            if (!ParseSize(next(), o.repeat)) return false;
        } else if (a == "--threads") {
            const char* v = next();
            if (!v) return false;
            o.threads = ParseThreads(v);
            if (o.threads.empty()) return false;
        } else if (a == "--csv") {
            const char* v = next(); if (!v) return false; o.csvPath = v;
        } else if (a == "--image") {
            const char* v = next(); if (!v) return false; o.imagePath = v;
        } else if (a == "--keep") {
            o.keep = true;
        } else if (a == "--via-service") {
            o.viaService = true;
        } else if (a == "--help" || a == "-h") {
            return false;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", a.c_str());
            return false;
        }
    }
    return true;
}

size_t MakeImage(const std::string& path, size_t sizeMiB)
{
    const size_t bytes = sizeMiB * 1024ULL * 1024ULL;
    std::vector<uint8_t> img(bytes, 0x5A);
    size_t planted = 0;
    for (uint64_t off = 32 * kSectorSize;
         off + kMagicSpacing < bytes; off += kMagicSpacing) {
        const Magic& m = kMagics[planted % (sizeof(kMagics) / sizeof(kMagics[0]))];
        std::memcpy(img.data() + off, m.hex, m.bytes);
        ++planted;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "cannot create %s\n", path.c_str()); return 0; }
    const size_t written = std::fwrite(img.data(), 1, img.size(), f);
#ifdef _WIN32
    // Force the 256 MiB write back to disk. Without this, background writeback
    // contends with the first few timed scans and inflates the apparent
    // speedup (the early thread counts are measured cold, the later ones warm).
    if (f) _commit(_fileno(f));
#endif
    std::fclose(f);
    if (written != img.size()) { std::fprintf(stderr, "short write\n"); return 0; }

    // Pre-read the whole image so every timed scan sees a warm page cache.
    FILE* r = std::fopen(path.c_str(), "rb");
    if (r) {
        std::vector<uint8_t> sink(1024 * 1024);
        while (std::fread(sink.data(), 1, sink.size(), r) > 0) {}
        std::fclose(r);
    }
    return planted;
}

using NodeMap = std::map<uint64_t, std::pair<std::string, uint64_t>>;

ScanResult RunOnce(SectorReader& reader, const Partition& part,
                   size_t nth, bool viaService)
{
    if (viaService) {
        CarveModuleService::CarveConfig cfg;
        cfg.numThreads = nth;
        return CarveModuleService::Run(reader, part, cfg);
    }
    RawScan rs;
    return (nth == 1) ? rs.Run(reader, part) : rs.Run(reader, part, nth);
}

double Median(std::vector<double>& v)
{
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    if (n == 0) return 0.0;
    if (n % 2) return v[n / 2];
    return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

} // namespace

int main(int argc, char** argv)
{
    Options opt;
    if (!ParseArgs(argc, argv, opt)) { Usage(argv[0]); return 2; }

    const size_t bytes = opt.sizeMiB * 1024ULL * 1024ULL;
    const size_t planted = MakeImage(opt.imagePath, opt.sizeMiB);
    if (planted == 0) return 1;

    const std::wstring wpath(opt.imagePath.begin(), opt.imagePath.end());

    std::printf("=== PhoenixRecover RAW-scan parallel benchmark (A1-11) ===\n");
#ifdef __GNUC__
    std::printf("compiler            : %s\n", __VERSION__);
#else
    std::printf("compiler            : (non-GCC)\n");
#endif
    std::printf("build               : %s\n",
#ifdef NDEBUG
                "Release (NDEBUG)");
#else
                "Debug (no NDEBUG)");
#endif
    std::printf("hardware_concurrency: %u\n", std::thread::hardware_concurrency());
    std::printf("image               : %s, %zu MiB (%llu bytes), 512 B sectors\n",
                opt.imagePath.c_str(), opt.sizeMiB, (unsigned long long)bytes);
    std::printf("filler              : constant 0x5A, %zu planted graphic magics @ 256 KiB\n",
                planted);
    std::printf("entry point         : %s\n",
                opt.viaService ? "CarveModuleService::Run (A1-10 production config chain)"
                               : "RawScan::Run direct");
    std::printf("repeats             : %zu (median reported)\n\n", opt.repeat);

    struct Row
    {
        size_t threads;
        double medianMs;
        double throughput;
        double speedup;
        uint32_t files;
        bool identical;
    };
    std::vector<Row> rows;

    // Two untimed serial warm-ups; the second supplies the equivalence
    // baseline. This amortises any residual writeback / page-cache effect
    // before the first timed measurement.
    NodeMap baseline;
    bool haveBaseline = false;
    for (int w = 0; w < 2; ++w) {
        SectorReader r;
        if (!r.OpenImage(wpath)) continue;
        Partition p;
        p.startSector  = 0;
        p.totalSectors = static_cast<uint32_t>(bytes / kSectorSize);
        p.sizeBytes    = bytes;
        ScanResult warm = RunOnce(r, p, 1, opt.viaService);
        if (w == 1) {
            for (const auto& fn : warm.files) {
                std::string ext(fn.extension.begin(), fn.extension.end());
                baseline[fn.rawOffset] = { ext, fn.size };
            }
            haveBaseline = true;
        }
    }
    if (!haveBaseline) {
        std::fprintf(stderr, "warm-up scan failed\n");
        return 1;
    }

    // Interleaved repeats with a rotating order: no thread count is
    // systematically measured first (cold) or last (warm). Without this the
    // 1→8 sequence inflated the apparent speedup by up to ~2x on a freshly
    // written 256 MiB image.
    bool allIdentical = true;
    std::map<size_t, std::vector<double>> samples;
    std::map<size_t, uint32_t> filesSeen;
    std::map<size_t, bool> identical;
    for (size_t n : opt.threads) {
        samples[n].reserve(opt.repeat);
        identical[n] = true;
    }

    const size_t k = opt.threads.size();
    for (size_t rep = 0; rep < opt.repeat; ++rep) {
        for (size_t i = 0; i < k; ++i) {
            const size_t nth = opt.threads[(i + rep) % k];

            SectorReader reader;
            if (!reader.OpenImage(wpath)) {
                std::fprintf(stderr, "OpenImage failed (%s)\n", opt.imagePath.c_str());
                return 1;
            }
            Partition part;
            part.startSector  = 0;
            part.totalSectors = static_cast<uint32_t>(bytes / kSectorSize);
            part.sizeBytes    = bytes;

            auto t0 = std::chrono::steady_clock::now();
            ScanResult result = RunOnce(reader, part, nth, opt.viaService);
            auto t1 = std::chrono::steady_clock::now();
            samples[nth].push_back(
                std::chrono::duration<double, std::milli>(t1 - t0).count());
            filesSeen[nth] = result.foundCount;

            NodeMap nodes;
            for (const auto& fn : result.files) {
                std::string ext(fn.extension.begin(), fn.extension.end());
                nodes[fn.rawOffset] = { ext, fn.size };
            }
            if (nodes != baseline) identical[nth] = false;
        }
    }

    const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    for (size_t nth : opt.threads) {
        const double med = Median(samples[nth]);
        const double throughput = (med > 0.0) ? (mb * 1000.0 / med) : 0.0;
        if (!identical[nth]) allIdentical = false;
        rows.push_back({nth, med, throughput, 1.0, filesSeen[nth], identical[nth]});
    }

    double serialMs = 0.0;
    for (const auto& r : rows) if (r.threads == 1) serialMs = r.medianMs;
    if (serialMs > 0.0) {
        for (auto& r : rows) r.speedup = serialMs / r.medianMs;
    }


    // Logger writes to the same stream without a trailing newline; separate it.
    std::printf("\n%-8s %12s %14s %10s %10s %10s\n",
                "threads", "median(ms)", "MB/s", "speedup", "files", "identical");
    std::printf("%-8s %12s %14s %10s %10s %10s\n",
                "--------", "------------", "--------------", "----------",
                "----------", "----------");
    for (const auto& r : rows) {
        std::printf("%-8zu %12.1f %14.1f %10.2fx %10u %10s\n",
                    r.threads, r.medianMs, r.throughput, r.speedup, r.files,
                    r.identical ? "yes" : "NO");
    }
    std::printf("\nfile-set equivalence vs serial: %s\n",
                allIdentical ? "PASS" : "FAIL");

    if (!opt.csvPath.empty()) {
        FILE* f = std::fopen(opt.csvPath.c_str(), "w");
        if (f) {
            std::fprintf(f, "threads,median_ms,MBps,speedup,files,identical\n");
            for (const auto& r : rows) {
                std::fprintf(f, "%zu,%.3f,%.3f,%.4f,%u,%d\n",
                             r.threads, r.medianMs, r.throughput, r.speedup,
                             r.files, r.identical ? 1 : 0);
            }
            std::fclose(f);
            std::printf("CSV written to %s\n", opt.csvPath.c_str());
        }
    }

    if (!opt.keep) std::remove(opt.imagePath.c_str());
    return (haveBaseline && allIdentical) ? 0 : 1;
}
