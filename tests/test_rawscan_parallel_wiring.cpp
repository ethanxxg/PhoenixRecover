// A1-10 — parallel RAW scan wired into the production carving service.
//
// Before this stage the parallel RawScan::Run(reader, partition, N) overload was
// reachable ONLY from CTest: every production caller used the serial 2-arg
// override. This test covers the wiring end-to-end at the CarveModuleService
// boundary (phase 3 of UnifiedVolumeScanner), plus the two correctness traps
// found while wiring it:
//
//   1. The parallel worker used a default-constructed SignatureDB (513
//      built-ins) instead of cloning the EFFECTIVE database. With a custom or
//      extension-filtered DB, parallel and serial results diverged.
//   2. CarveModuleService::targetExtensions never actually filtered anything:
//      filteredDb was default-constructed (built-ins included) and matching
//      signatures were merely APPENDED.
//
// Build: no Qt, no device I/O. PHOENIX_BUILD_GUI=0.

#include "scan/CarveModuleService.h"
#include "scan/UnifiedVolumeScanner.h"
#include "scan/RawScan.h"
#include "disk/SectorReader.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifndef PHOENIX_SOURCE_DIR
#define PHOENIX_SOURCE_DIR L"."
#endif

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

void Plant(std::vector<uint8_t>& img, size_t off, const std::string& hex)
{
    std::vector<uint8_t> b = HexToBytes(hex);
    if (off + b.size() <= img.size())
        std::memcpy(img.data() + off, b.data(), b.size());
}

const char* const kImgPath = "rawscan_parallel_wiring.dd";
const size_t      kImgSize = 4 * 1024 * 1024;   // 8192 sectors

// Strictly increasing, 512-aligned, header-only graphic magics. Several sit on
// the 4-thread chunk boundaries (2048 / 4096 / 6144) to exercise overlap dedup.
struct Spec { size_t sector; std::string hex; const char* ext; };
const std::vector<Spec>& Specs()
{
    static const std::vector<Spec> s = {
        {16,   "89504E470D0A1A0A", "png"},
        {100,  "FFD8FFE0",         "jpg"},
        {500,  "474946383961",     "gif"},
        {2048, "89504E470D0A1A0A", "png"},   // 4T boundary
        {3000, "FFD8FFE0",         "jpg"},
        {4096, "89504E470D0A1A0A", "png"},   // 2T / 4T boundary
        {6000, "474946383961",     "gif"},
        {6144, "89504E470D0A1A0A", "png"},   // 4T boundary
        {7000, "FFD8FFE0",         "jpg"},
        {7900, "89504E470D0A1A0A", "png"},
    };
    return s;
}

void MakeImage()
{
    std::vector<uint8_t> img(kImgSize, 0);
    std::mt19937_64 rng(0x5a17c0de1234abcdULL);
    for (size_t i = 0; i < img.size(); ++i)
        img[i] = static_cast<uint8_t>(rng() & 0xFF);
    for (const auto& s : Specs()) Plant(img, s.sector * 512, s.hex);
    FILE* f = std::fopen(kImgPath, "wb");
    if (f) { std::fwrite(img.data(), 1, img.size(), f); std::fclose(f); }
}

using NodeMap = std::map<uint64_t, std::pair<std::string, uint64_t>>;

NodeMap Collect(const CarveModuleService::CarveConfig& cfg,
                std::atomic<int>* offMainThread = nullptr,
                std::thread::id mainId = std::thread::id())
{
    NodeMap m;
    SectorReader reader;
    std::string p(kImgPath);
    std::wstring wpath(p.begin(), p.end());
    if (!reader.OpenImage(wpath)) {
        std::fprintf(stderr, "FAIL: OpenImage(%s)\n", kImgPath);
        return m;
    }
    Partition part;
    part.startSector  = 0;
    part.totalSectors = static_cast<uint32_t>(kImgSize / 512);
    part.sizeBytes    = kImgSize;

    ScanResult r = CarveModuleService::Run(
        reader, part, cfg, nullptr,
        [offMainThread, mainId](const ScanProgress&) {
            if (offMainThread && std::this_thread::get_id() != mainId)
                offMainThread->fetch_add(1);
        },
        [offMainThread, mainId](const FileNode& n) {
            if (offMainThread && std::this_thread::get_id() != mainId)
                offMainThread->fetch_add(1);
            (void)n;
        });

    for (const auto& fn : r.files) {
        std::string ext(fn.extension.begin(), fn.extension.end());
        m[fn.rawOffset] = { ext, fn.size };
    }
    return m;
}

bool Same(const NodeMap& a, const NodeMap& b, const char* label)
{
    if (a.size() != b.size()) {
        std::fprintf(stderr, "  %s: count %zu vs %zu\n", label, a.size(), b.size());
        return false;
    }
    for (const auto& kv : a) {
        auto it = b.find(kv.first);
        if (it == b.end() || it->second != kv.second) {
            std::fprintf(stderr, "  %s: mismatch at offset %llu\n",
                         label, (unsigned long long)kv.first);
            return false;
        }
    }
    return true;
}

std::set<uint64_t> Offsets(const NodeMap& m)
{
    std::set<uint64_t> s;
    for (const auto& kv : m) s.insert(kv.first);
    return s;
}

bool ReadSource(const wchar_t* rel, std::string& out)
{
    std::wstring wrel(rel);
    std::string narrow(wrel.begin(), wrel.end());
    const std::wstring root(PHOENIX_SOURCE_DIR);
    std::string path(root.begin(), root.end());
    path += "/";
    path += narrow;
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

} // namespace

int main()
{
    int failures = 0;
    int checks = 0;
    auto check = [&](bool ok, const char* msg) {
        ++checks;
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", msg); ++failures; }
        else     { std::fprintf(stdout, "ok:   %s\n", msg); }
    };

    // ── 1. Config defaults: serial, i.e. zero behaviour change ──────────────
    {
        CarveModuleService::CarveConfig c;
        check(c.numThreads == 1, "CarveConfig default numThreads == 1 (serial)");
        UnifiedScanConfig u;
        check(u.rawScanThreads == 1, "UnifiedScanConfig default rawScanThreads == 1");
        RawScan rs;
        check(rs.GetThreadCount() == 1, "RawScan default thread count == 1");
        rs.SetThreadCount(0);
        check(rs.GetThreadCount() == 1, "RawScan SetThreadCount(0) normalises to 1");
        rs.SetThreadCount(4);
        check(rs.GetThreadCount() == 4, "RawScan SetThreadCount(4) is retained");
    }

    // ── 2. Phase-3 config mapping (inline, headless-testable) ───────────────
    {
        UnifiedScanConfig u;
        u.maxCarveFileSize = 7ULL * 1024 * 1024;
        u.targetExtensions = { L"png", L"jpg" };
        u.rawScanThreads   = 4;
        auto c = MakeCarveConfig(u);
        check(c.maxFileSize == 7ULL * 1024 * 1024, "MakeCarveConfig forwards maxFileSize");
        check(c.targetExtensions.size() == 2 && c.targetExtensions[0] == L"png",
              "MakeCarveConfig forwards targetExtensions");
        check(c.numThreads == 4, "MakeCarveConfig forwards rawScanThreads");
    }

    // ── 3. Exact-signature-set SignatureDB constructor ─────────────────────
    {
        FileSignature sig;
        sig.extension = L"pwx";
        sig.header    = HexToBytes("50484F454E495858"); // "PHOENIXX"
        std::vector<FileSignature> one{sig};
        SignatureDB exact(one);
        check(exact.GetAll().size() == 1, "exact-set DB contains exactly 1 signature");
        check(exact.GetAll()[0].extension == L"pwx", "exact-set DB keeps the custom extension");
        auto magic = HexToBytes("50484F454E49585800");
        check(exact.Match(magic.data(), magic.size()) != nullptr,
              "exact-set DB matches its custom magic");
        auto jpg = HexToBytes("FFD8FFE000000000");
        check(exact.Match(jpg.data(), jpg.size()) == nullptr,
              "exact-set DB does NOT contain the built-in JPEG signature");

        SignatureDB builtIn;
        check(builtIn.GetAll().size() > exact.GetAll().size(),
              "default DB still holds the full built-in signature set");
    }

    MakeImage();

    // ── 4. Custom DB: parallel MUST clone the effective set ─────────────────
    // Before A1-10 each worker built its own default SignatureDB (513 built-ins)
    // and silently ignored customDb, so the two maps were guaranteed to differ.
    {
        SignatureDB pngOnly(std::vector<FileSignature>{});
        FileSignature png;
        png.extension = L"png";
        png.header    = HexToBytes("89504E470D0A1A0A");
        pngOnly.Add(png);

        CarveModuleService::CarveConfig base;
        base.customDb = &pngOnly;

        CarveModuleService::CarveConfig serialCfg = base;
        serialCfg.numThreads = 1;
        CarveModuleService::CarveConfig parCfg = base;
        parCfg.numThreads = 4;

        auto serial = Collect(serialCfg);
        auto par4   = Collect(parCfg);
        std::fprintf(stdout, "  custom-DB serial=%zu parallel(4)=%zu\n",
                     serial.size(), par4.size());
        check(!serial.empty(), "custom-DB serial scan recovered PNG magics");
        check(Same(serial, par4, "custom-DB parallel(4)"),
              "custom-DB parallel(4) result identical to serial (effective DB cloned)");

        auto offs = Offsets(serial);
        bool allPng = true;
        for (const auto& s : Specs()) {
            if (std::string(s.ext) != "png") continue;
            if (offs.find(static_cast<uint64_t>(s.sector) * 512) == offs.end())
                allPng = false;
        }
        check(allPng, "every planted PNG offset recovered with the png-only custom DB");
        bool noJpgGif = true;
        for (const auto& s : Specs()) {
            if (std::string(s.ext) == "png") continue;
            if (offs.find(static_cast<uint64_t>(s.sector) * 512) != offs.end())
                noJpgGif = false;
        }
        check(noJpgGif, "JPEG/GIF offsets absent under png-only custom DB");
    }

    // ── 5. targetExtensions now really filters (pre-existing bug fixed) ─────
    {
        CarveModuleService::CarveConfig base;
        base.targetExtensions = { L"png" };

        CarveModuleService::CarveConfig serialCfg = base;
        serialCfg.numThreads = 1;
        CarveModuleService::CarveConfig parCfg = base;
        parCfg.numThreads = 4;

        auto serial = Collect(serialCfg);
        auto par4   = Collect(parCfg);
        std::fprintf(stdout, "  filtered serial=%zu parallel(4)=%zu\n",
                     serial.size(), par4.size());
        check(Same(serial, par4, "filtered parallel(4)"),
              "targetExtensions filtered parallel(4) result identical to serial");

        auto offs = Offsets(serial);
        bool onlyPngHits = true;
        for (const auto& s : Specs()) {
            const uint64_t off = static_cast<uint64_t>(s.sector) * 512;
            const bool present = offs.find(off) != offs.end();
            if (std::string(s.ext) == "png" && !present) onlyPngHits = false;
            if (std::string(s.ext) != "png" && present) onlyPngHits = false;
        }
        check(onlyPngHits, "targetExtensions={png} recovers PNG offsets and no JPEG/GIF offsets");

        bool allExtPng = true;
        for (const auto& kv : serial) {
            if (kv.second.first != "png") { allExtPng = false; break; }
        }
        check(allExtPng, "targetExtensions={png} emits no non-png extension");
    }

    // ── 6. Callbacks stay on the calling thread under parallelism ───────────
    {
        CarveModuleService::CarveConfig cfg;
        cfg.numThreads = 4;
        std::atomic<int> offMain{0};
        const std::thread::id mainId = std::this_thread::get_id();
        auto nodes = Collect(cfg, &offMain, mainId);
        std::fprintf(stdout, "  callbacks off-main-thread=%d, nodes=%zu\n",
                     offMain.load(), nodes.size());
        check(offMain.load() == 0,
              "file/progress callbacks emitted only on the calling thread");
    }

    // ── 7. Cancellation propagates out of the parallel workers ──────────────
    {
        SectorReader reader;
        std::string p(kImgPath);
        std::wstring wpath(p.begin(), p.end());
        if (reader.OpenImage(wpath)) {
            Partition part;
            part.startSector  = 0;
            part.totalSectors = static_cast<uint32_t>(kImgSize / 512);
            part.sizeBytes    = kImgSize;
            CarveModuleService::CarveConfig cfg;
            cfg.numThreads = 4;
            std::atomic<bool> cancel{true};
            ScanResult r = CarveModuleService::Run(reader, part, cfg, &cancel);
            check(r.cancelled, "parallel workers propagate cancellation to ScanResult");
        } else {
            check(false, "OpenImage for cancellation case");
        }
    }

    // ── 8. Source-level wiring guard (project convention) ───────────────────
    // The behavioural tests above prove correctness of the parallel path, but
    // a future refactor could silently drop the SetThreadCount() call and leave
    // production permanently serial while all tests still pass. Guard it.
    {
        std::string svc, scanner;
        check(ReadSource(L"scan/CarveModuleService.cpp", svc),
              "read scan/CarveModuleService.cpp for wiring guard");
        check(svc.find("engine.SetThreadCount(ResolveThreadCount(config.numThreads))")
                  != std::string::npos,
              "CarveModuleService.cpp wires config.numThreads into RawScan");
        check(ReadSource(L"scan/UnifiedVolumeScanner.cpp", scanner),
              "read scan/UnifiedVolumeScanner.cpp for wiring guard");
        check(scanner.find("MakeCarveConfig(config_)") != std::string::npos,
              "UnifiedVolumeScanner.cpp uses MakeCarveConfig(config_)");
    }

    std::remove(kImgPath);

    std::fprintf(stdout, "\n%d checks, %d failure(s)\n", checks, failures);
    if (failures == 0) std::fprintf(stdout, "ALL RAWSCAN-PARALLEL-WIRING CHECKS PASSED\n");
    return failures == 0 ? 0 : 1;
}
