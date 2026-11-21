// A1-12 / real-disk batch recovery support.
//
// Verifies that the opt-in SectorReader read-ahead window is a PURE cache:
//   1. default is OFF (no behaviour change whatsoever for GUI / existing tests)
//   2. when enabled, ReadBytes() returns byte-for-byte identical data
//   3. window-boundary, oversized-request and end-of-image cases are safe
//   4. source-level guards exist (negative verification is done by temporarily
//      disabling the guard in SectorReader_win.cpp and re-running this test)
//
// Uses a synthetic 8 MiB raw image only - never touches a physical disk.

#include "../disk/SectorReader.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool ok, const std::string& what)
{
    ++g_checks;
    if (!ok) {
        ++g_fails;
        std::cout << "FAIL: " << what << "\n";
    } else {
        std::cout << "OK: " << what << "\n";
    }
}

static const char* kImage = "phoenix_readahead_probe.dd";
static const uint64_t kImageBytes = 8ULL * 1024 * 1024;

static uint8_t pattern(uint64_t i)
{
    // Sector-varying pattern: catches any window/offset misalignment.
    return static_cast<uint8_t>((i * 37u + (i / 512u) * 11u + (i >> 17)) & 0xFFu);
}

static bool make_image()
{
    std::ofstream f(kImage, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    std::vector<uint8_t> block(64 * 1024);
    uint64_t pos = 0;
    while (pos < kImageBytes) {
        const uint64_t n = std::min<uint64_t>(block.size(), kImageBytes - pos);
        for (uint64_t i = 0; i < n; ++i)
            block[static_cast<size_t>(i)] = pattern(pos + i);
        f.write(reinterpret_cast<const char*>(block.data()),
                static_cast<std::streamsize>(n));
        pos += n;
    }
    f.flush();
    return f.good();
}

struct Req { uint64_t off; uint32_t len; };

// Sequential-ish requests (mimic per-cluster recovery) plus adversarial ones.
static std::vector<Req> requests()
{
    std::vector<Req> r;
    for (uint64_t i = 0; i < 400; ++i)
        r.push_back({ i * 4096 + 7, 4096 });          // 400 clustered reads
    r.push_back({ 1024 * 1024 - 100, 4096 });         // crosses a 1 MiB window edge
    r.push_back({ 1024 * 1024 + 33, 8192 });
    r.push_back({ 3 * 1024 * 1024 + 511, 900 });      // unaligned offset+length
    r.push_back({ 2 * 1024 * 1024, 2 * 1024 * 1024 }); // > window/2 -> fallback path
    r.push_back({ kImageBytes - 4096 - 13, 2048 });   // near end of image
    r.push_back({ kImageBytes - 1024, 1024 });        // exact tail
    r.push_back({ 0, 512 });                          // very first sector
    return r;
}

static std::vector<uint8_t> read_all(SectorReader& rd, const std::vector<Req>& reqs,
                                     bool& allOk)
{
    std::vector<uint8_t> out;
    allOk = true;
    std::vector<uint8_t> buf(4 * 1024 * 1024);
    for (const Req& q : reqs) {
        std::memset(buf.data(), 0xEE, q.len);
        if (!rd.ReadBytes(q.off, buf.data(), q.len)) {
            allOk = false;
            continue;
        }
        for (uint32_t i = 0; i < q.len; ++i) {
            const uint8_t want = pattern(q.off + i);
            if (buf[i] != want) { allOk = false; break; }
        }
        out.insert(out.end(), buf.begin(), buf.begin() + q.len);
    }
    return out;
}

// Source-level guards: these strings MUST exist, otherwise the cache could be
// accidentally always-on (breaking the "zero behaviour change" guarantee) or
// could lose the transparent fallback on a failed speculative read.
static bool source_guard(const std::string& path, const std::string& needle)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string all((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    return all.find(needle) != std::string::npos;
}

int main()
{
    check(make_image(), "synthetic 8 MiB image created");

    // Derive the source root from __FILE__ so the guard works from any build dir.
    const std::string thisFile = __FILE__;
    const size_t slash = thisFile.find_last_of("/\\");
    const std::string root = thisFile.substr(0, slash + 1) + "../";
    check(source_guard(root + "disk/SectorReader.h",
                       "uint32_t m_readAheadWindow = 0;"),
          "header declares read-ahead DISABLED by default");
    check(source_guard(root + "disk/SectorReader_win.cpp",
                       "if (m_readAheadWindow != 0 && m_readAheadPtr != nullptr"),
          "ReadBytes is guarded by the opt-in flag");
    check(source_guard(root + "disk/SectorReader_win.cpp",
                       "Transparent fallback"),
          "failed speculative fills fall back to original path");
    check(source_guard(root + "disk/SectorReader_win.cpp",
                       "m_readAheadLen   = 0;"),
          "Close() invalidates the window (no cross-device stale data)");

    const std::vector<Req> reqs = requests();
    const std::wstring image = L"phoenix_readahead_probe.dd";

    // ── baseline: default OFF ────────────────────────────────────────────
    SectorReader base;
    check(base.GetReadAheadWindow() == 0, "default read-ahead window is 0");
    check(base.OpenImage(image), "baseline reader opened synthetic image");
    bool baseOk = false;
    std::vector<uint8_t> expected = read_all(base, reqs, baseOk);
    check(baseOk, "baseline ReadBytes matches the synthetic pattern everywhere");
    const uint64_t baseSectors = base.GetSectorsRead();
    check(baseSectors > 0, "baseline sectorsRead counter is populated");
    check(base.GetReadAheadHits() == 0 && base.GetReadAheadFills() == 0,
          "baseline performed zero cached reads");
    check(base.GetBadSectorCount() == 0, "baseline reports no bad sectors");
    base.Close();

    // ── cached: ON, 1 MiB window ─────────────────────────────────────────
    SectorReader fast;
    check(fast.OpenImage(image), "cached reader opened synthetic image");
    fast.SetReadAheadWindow(1024 * 1024);
    check(fast.GetReadAheadWindow() == 1024 * 1024,
          "read-ahead window set to 1 MiB");
    bool fastOk = false;
    std::vector<uint8_t> actual = read_all(fast, reqs, fastOk);
    check(fastOk, "cached ReadBytes matches the synthetic pattern everywhere");
    check(actual == expected,
          "cached bytes are byte-for-byte identical to baseline");
    check(fast.GetReadAheadFills() > 0, "speculative window fills occurred");
    check(fast.GetReadAheadHits() > 0, "cached hits occurred");
    check(fast.GetReadAheadHits() >= 300,
          "majority of clustered reads served from cache");
    check(fast.GetBadSectorCount() == 0, "cached path reports no bad sectors");
    fast.Close();

    // ── disabling again restores the exact original path ─────────────────
    fast.SetReadAheadWindow(0);
    check(fast.GetReadAheadWindow() == 0, "window can be disabled again");
    check(fast.OpenImage(image), "reader re-opened after disabling");
    bool againOk = false;
    std::vector<uint8_t> again = read_all(fast, reqs, againOk);
    check(againOk && again == expected,
          "disabling the window restores baseline bytes");
    check(fast.GetReadAheadHits() == 0,
          "no cached reads after disabling");
    fast.Close();

    std::remove(kImage);

    std::cout << "\n" << (g_checks - g_fails) << "/" << g_checks
              << " checks passed\n";
    return g_fails ? 1 : 0;
}
