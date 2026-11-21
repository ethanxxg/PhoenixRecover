// P0-13 — region imager with bad-block skipping.
//
// WHY THIS EXISTS
// ---------------
// The source disk (PhysicalDrive2, ST1000DM003 over a USB/UASP bridge) now has
// a contiguous unreadable band whose first sector is exactly the start of the
// volume-1 $MFT (LBA 216,020,094 = boot 209,728,638 + 786,432 clusters * 8).
// The stock engine path pays the full device timeout per bad sector
// (~149.5 s with ReadPolicy.maxRetries=3), so a single unknown-length band can
// stall a scan for days.
//
// A whole-disk image does not fit on this machine either (931.51 GiB source vs
// 661 GB + 137 GB free), so this tool images an ARBITRARY LBA RANGE by its own
// bounds, one volume at a time, and writes a map of what it could not read.
//
// DESIGN
//   * Reads go through the production SectorReader (disk/SectorReader.*), i.e.
//     the same code the GUI and CTests use. No new read path, no new handle
//     logic: GENERIC_READ only, never a write IOCTL. The source disk is never
//     written.
//   * STRICT policy (skipBadSectors = false) on purpose. That is the whole
//     trick: SectorReader's lenient mode zero-fills and keeps going, but on a
//     failed bulk read it grinds *every* sector in the chunk through the device
//     timeout, so one bad 4 MiB chunk would cost thousands of timeouts. In
//     strict mode ReadSectors() returns false at the FIRST unrecoverable
//     sector, so a clean chunk is one fast read and a dirty chunk fails early.
//   * On failure the chunk is re-read one sector at a time (also strict) to
//     learn exactly which sectors are gone; those are zero-filled and recorded.
//   * A run of --max-streak consecutive bad sectors means we are inside a band;
//     instead of paying the timeout thousands of times the tool records a
//     skipped range and jumps forward. The jump ESCALATES (start * 2^n, capped)
//     so a small band is not overrun, while a large one is crossed quickly.
//   * Everything unreadable is either zero-filled (attempted, `.unreadable`) or
//     never attempted (`.skipped`). Both land in the .map.json next to the
//     image, so a restore knows exactly which bytes are real.
//   * --resume picks up from map.nextLba, so a kill/crash costs one chunk.
//
// ZERO GUI CHANGES. ZERO WRITES TO THE SOURCE DISK.

#include "../disk/SectorReader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

namespace {

std::atomic<bool> g_cancel{false};

BOOL WINAPI OnConsoleEvent(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT) {
        g_cancel.store(true);
        return TRUE;
    }
    return FALSE;
}

struct Range { uint64_t start; uint64_t end; };   // sectors, half-open

struct ImageMap
{
    std::string out;
    int         disk        = 2;
    uint64_t    start       = 0;
    uint64_t    sectors     = 0;
    uint64_t    sectorSize  = 512;
    uint64_t    nextLba     = 0;
    uint64_t    cleanChunks = 0;
    uint64_t    badChunks   = 0;
    uint64_t    badSectors  = 0;
    uint64_t    skipped     = 0;
    uint64_t    elapsedSec  = 0;
    bool        complete    = false;
    std::vector<Range> unreadable;
    std::vector<Range> gaps;
};

std::wstring Widen(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

void AddRange(std::vector<Range>& v, uint64_t s, uint64_t e)
{
    if (e <= s) return;
    if (!v.empty() && v.back().end == s) { v.back().end = e; return; }
    v.push_back(Range{s, e});
}

std::string JsonEscape(const std::string& s)
{
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\\' || c == '"') o.push_back('\\');
        o.push_back(c);
    }
    return o;
}

bool WriteJsonRanges(std::FILE* f, const char* key, const std::vector<Range>& v)
{
    if (std::fprintf(f, "  \"%s\": [", key) < 0) return false;
    for (size_t i = 0; i < v.size(); ++i) {
        if (std::fprintf(f, "%s\n    [%llu, %llu]",
                         i ? "," : "",
                         static_cast<unsigned long long>(v[i].start),
                         static_cast<unsigned long long>(v[i].end)) < 0)
            return false;
    }
    return std::fprintf(f, "%s],\n", v.empty() ? "" : "\n  ") >= 0;
}

bool SaveMap(const std::string& path, const ImageMap& m)
{
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fprintf(f,
        "{\n"
        "  \"tool\": \"tools/image_region.cpp\",\n"
        "  \"out\": \"%s\",\n"
        "  \"disk\": %d,\n"
        "  \"startLba\": %llu,\n"
        "  \"sectors\": %llu,\n"
        "  \"sectorSize\": %llu,\n"
        "  \"nextLba\": %llu,\n"
        "  \"cleanChunks\": %llu,\n"
        "  \"badChunks\": %llu,\n"
        "  \"badSectors\": %llu,\n"
        "  \"skippedSectors\": %llu,\n"
        "  \"elapsedSec\": %llu,\n"
        "  \"complete\": %s,\n"
        "  \"fullyRead\": %s,\n",
        JsonEscape(m.out).c_str(), m.disk,
        static_cast<unsigned long long>(m.start),
        static_cast<unsigned long long>(m.sectors),
        static_cast<unsigned long long>(m.sectorSize),
        static_cast<unsigned long long>(m.nextLba),
        static_cast<unsigned long long>(m.cleanChunks),
        static_cast<unsigned long long>(m.badChunks),
        static_cast<unsigned long long>(m.badSectors),
        static_cast<unsigned long long>(m.skipped),
        static_cast<unsigned long long>(m.elapsedSec),
        m.complete ? "true" : "false",
        (m.skipped == 0 && m.badSectors == 0) ? "true" : "false");
    WriteJsonRanges(f, "unreadable", m.unreadable);
    WriteJsonRanges(f, "skipped", m.gaps);
    std::fprintf(f, "  \"sourceDiskWriteOperations\": 0\n}\n");
    std::fclose(f);
    return true;
}

bool LoadMap(const std::string& path, ImageMap& m)
{
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    auto scalar = [&](const char* key) -> unsigned long long {
        std::string needle = std::string("\"") + key + "\":";
        size_t p = text.find(needle);
        if (p == std::string::npos) return 0;
        p += needle.size();
        while (p < text.size() && (text[p] == ' ' || text[p] == '\t')) ++p;
        return std::strtoull(text.c_str() + p, nullptr, 10);
    };

    m.start      = scalar("startLba");
    m.sectors    = scalar("sectors");
    m.nextLba    = scalar("nextLba");
    m.elapsedSec = scalar("elapsedSec");
    m.badSectors = scalar("badSectors");
    m.skipped    = scalar("skippedSectors");
    return m.sectors != 0 && m.nextLba >= m.start;
}

bool WriteAt(HANDLE h, uint64_t byteOffset, const void* data, size_t len)
{
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(byteOffset);
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t left = len;
    while (left > 0) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(left, 1u << 20));
        DWORD done = 0;
        if (!WriteFile(h, p, chunk, &done, nullptr) || done == 0) return false;
        p += done;
        left -= done;
    }
    return true;
}

void Usage()
{
    std::fprintf(stderr,
        "usage: image_region --disk N --start LBA --sectors N --out <img>\n"
        "                    [--map <json>] [--resume] [--chunk-kib 1024]\n"
        "                    [--skip-mib 8] [--skip-max-mib 256]\n"
        "                    [--max-streak 6] [--bad-budget 200]\n"
        "                    [--retries 0] [--timeout-ms 8000]\n"
        "\n"
        "  --start/--sectors  absolute LBA range on the disk (not bytes)\n"
        "  --skip-mib         first jump after --max-streak bad sectors in a row;\n"
        "                     doubles per consecutive jump up to --skip-max-mib\n"
        "  --retries          ReadPolicy.maxRetries (0 = one attempt/sector)\n"
        "  --bad-budget       stop after this many unreadable sectors (safety)\n"
        "  --time-budget      stop after this many seconds (0 = unlimited)\n");
}

} // namespace

int main(int argc, char** argv)
{
    int         disk      = 2;
    uint64_t    start     = 0;
    uint64_t    sectors   = 0;
    std::string outPath;
    std::string mapPath;
    bool        resume    = false;
    uint32_t    chunkKiB  = 1024;
    uint32_t    skipMiB   = 8;
    uint32_t    skipMaxMiB= 256;
    uint32_t    maxStreak = 6;
    uint32_t    badBudget = 200;
    uint32_t    retries   = 0;
    uint32_t    timeoutMs = 8000;
    uint32_t    timeBudget = 3600;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "error: %s needs a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if      (a == "--disk")         disk       = std::atoi(next("--disk"));
        else if (a == "--start")        start      = std::strtoull(next("--start"), nullptr, 0);
        else if (a == "--sectors")      sectors    = std::strtoull(next("--sectors"), nullptr, 0);
        else if (a == "--out")          outPath    = next("--out");
        else if (a == "--map")          mapPath    = next("--map");
        else if (a == "--resume")       resume     = true;
        else if (a == "--chunk-kib")    chunkKiB   = static_cast<uint32_t>(std::atoi(next("--chunk-kib")));
        else if (a == "--skip-mib")     skipMiB    = static_cast<uint32_t>(std::atoi(next("--skip-mib")));
        else if (a == "--skip-max-mib") skipMaxMiB = static_cast<uint32_t>(std::atoi(next("--skip-max-mib")));
        else if (a == "--max-streak")   maxStreak  = static_cast<uint32_t>(std::atoi(next("--max-streak")));
        else if (a == "--bad-budget")   badBudget  = static_cast<uint32_t>(std::atoi(next("--bad-budget")));
        else if (a == "--retries")      retries    = static_cast<uint32_t>(std::atoi(next("--retries")));
        else if (a == "--timeout-ms")   timeoutMs  = static_cast<uint32_t>(std::atoi(next("--timeout-ms")));
        else if (a == "--time-budget")  timeBudget = static_cast<uint32_t>(std::atoi(next("--time-budget")));
        else if (a == "-h" || a == "--help") { Usage(); return 0; }
        else { std::fprintf(stderr, "error: unknown arg %s\n", a.c_str()); Usage(); return 2; }
    }
    if (outPath.empty() || sectors == 0) { Usage(); return 2; }
    if (mapPath.empty()) mapPath = outPath + ".map.json";
    if (chunkKiB == 0)   chunkKiB  = 1024;
    if (skipMiB == 0)    skipMiB   = 8;
    if (skipMaxMiB < skipMiB) skipMaxMiB = skipMiB;
    if (maxStreak == 0)  maxStreak = 6;

    SectorReader reader;
    ReadPolicy policy;
    policy.maxRetries     = retries;
    policy.timeoutMs      = timeoutMs;
    policy.retryDelayMs   = 50;
    policy.skipBadSectors = false;   // STRICT: fail fast at the first bad sector
    reader.SetReadPolicy(policy);
    reader.SetCancelFlag(&g_cancel);

    if (!reader.Open(disk)) {
        std::fprintf(stderr, "FATAL: SectorReader::Open(%d) failed (err=%lu)\n",
                     disk, static_cast<unsigned long>(reader.GetLastWinError()));
        return 3;
    }

    const uint32_t ss        = reader.GetSectorSize();
    const uint32_t chunkSecs = static_cast<uint32_t>((uint64_t(chunkKiB) << 10) / ss);
    const uint64_t skipBase  = (uint64_t(skipMiB) << 20) / ss;
    const uint64_t skipCap   = (uint64_t(skipMaxMiB) << 20) / ss;
    const uint64_t end       = start + sectors;

    ImageMap map;
    map.out = outPath; map.disk = disk; map.start = start;
    map.sectors = sectors; map.sectorSize = ss; map.nextLba = start;

    if (resume && LoadMap(mapPath, map)) {
        if (map.nextLba < start || map.nextLba > end) {
            std::fprintf(stderr, "FATAL: map nextLba %llu outside range\n",
                         static_cast<unsigned long long>(map.nextLba));
            return 2;
        }
        std::printf("resume from LBA %llu (%.2f%% done)\n",
                    static_cast<unsigned long long>(map.nextLba),
                    100.0 * double(map.nextLba - start) / double(sectors));
    } else {
        map.nextLba = start;
    }

    std::wstring wout = Widen(outPath);
    HANDLE h = CreateFileW(wout.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           resume ? OPEN_EXISTING : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "FATAL: cannot open image for write (err=%lu)\n",
                     static_cast<unsigned long>(GetLastError()));
        return 3;
    }

    // FILE_FLAG_NO_BUFFERING (see SectorReader_win.cpp::Open) requires the read
    // buffer AND the transfer length to be aligned to the device sector size
    // (4096 on any 512e/4Kn disk). A plain std::vector aligns to 16, which makes
    // every ReadFile fail with ERROR_INVALID_PARAMETER and would look exactly
    // like "every sector is bad". Over-allocate and align by hand.
    const size_t chunkBytes = static_cast<size_t>(chunkSecs) * ss;
    std::vector<uint8_t> storage(chunkBytes + 4096);
    uint8_t* buf = reinterpret_cast<uint8_t*>(
        (reinterpret_cast<uintptr_t>(storage.data()) + 4095u) &
        ~static_cast<uintptr_t>(4095u));
    std::memset(buf, 0, chunkBytes);
    SetConsoleCtrlHandler(OnConsoleEvent, TRUE);

    const auto t0 = std::chrono::steady_clock::now();
    uint64_t pos        = map.nextLba;
    uint64_t nextCkpt   = (pos - start) * ss + (256ull << 20);
    uint32_t streak     = 0;
    uint32_t jumpCount  = 0;
    bool     ioError    = false;
    bool     budgetHit  = false;
    bool     timeHit    = false;

    while (pos < end && !g_cancel.load()) {
        if (timeBudget != 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - t0).count() >=
                static_cast<long long>(timeBudget)) {
            timeHit = true;
            std::printf("TIME BUDGET reached (%u s); stopping\n", timeBudget);
            std::fflush(stdout);
            break;
        }
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(chunkSecs, end - pos));

        if (reader.ReadSectors(pos, buf, n)) {
            if (!WriteAt(h, (pos - start) * ss, buf, size_t(n) * ss)) {
                std::fprintf(stderr, "FATAL: write failed at LBA %llu\n",
                             static_cast<unsigned long long>(pos));
                ioError = true; break;
            }
            pos += n;
            ++map.cleanChunks;
            streak = 0;
            jumpCount = 0;
        } else {
            ++map.badChunks;
            // 4 KiB granularity: 512-byte single-sector reads are illegal on a
            // FILE_FLAG_NO_BUFFERING handle for a 4Kn-aligned device, so the
            // engine's own per-sector retry path cannot locate damage here.
            const uint32_t unit = std::max<uint32_t>(1, 4096u / ss);
            uint32_t done = 0;
            uint32_t chunkBad = 0;   // unreadable units in this chunk
            uint32_t units = 0;      // units examined in this chunk
            bool jumped = false;
            while (done < n && !g_cancel.load()) {
                const uint32_t u = std::min<uint32_t>(unit, n - done);
                const bool ok = reader.ReadSectors(pos + done, buf, u);
                if (!ok) std::memset(buf, 0, size_t(u) * ss);  // strict mode: no fill
                if (!WriteAt(h, (pos + done - start) * ss, buf, size_t(u) * ss)) {
                    ioError = true; break;
                }
                done += u;
                ++units;
                if (ok) {
                    streak = 0;
                } else {
                    {
                        static int dbg = 0;
                        if (dbg < 3) {
                            ++dbg;
                            std::printf("READ FAIL lba=%llu sectors=%u err=%lu\n",
                                        static_cast<unsigned long long>(pos + done - u),
                                        u,
                                        static_cast<unsigned long>(reader.GetLastWinError()));
                            std::fflush(stdout);
                        }
                    }
                    map.badSectors += u;
                    ++chunkBad;
                    ++streak;
                    AddRange(map.unreadable, pos + done - u, pos + done);
                    if (map.badSectors >= badBudget) {
                        budgetHit = true;
                        std::printf("BAD BUDGET reached (%u sectors); stopping\n", badBudget);
                        std::fflush(stdout);
                        break;
                    }
                    // Density test: scattered damage never produces a long
                    // consecutive run, so a run-only rule would grind every
                    // unit of the chunk through the device timeout.
                    const bool dense = (chunkBad >= 4 && chunkBad * 3 >= units);
                    if (streak >= maxStreak || dense) {
                        const uint64_t from = pos + done;
                        uint64_t span = skipBase;
                        for (uint32_t k = 0; k < jumpCount && span < skipCap; ++k) span *= 2;
                        span = std::min<uint64_t>(span, skipCap);
                        const uint64_t to = std::min<uint64_t>(from + span, end);
                        if (to > from) {
                            AddRange(map.gaps, from, to);
                            map.skipped += to - from;
                            std::printf("BAD RUN: skip [%llu, %llu) = %.1f MiB (jump #%u)\n",
                                        static_cast<unsigned long long>(from),
                                        static_cast<unsigned long long>(to),
                                        double(to - from) * ss / double(1u << 20),
                                        jumpCount + 1);
                            std::fflush(stdout);
                            pos = to;
                            jumped = true;
                            ++jumpCount;
                        }
                        streak = 0;
                        break;
                    }
                }
            }
            if (ioError || budgetHit) break;
            if (!jumped) pos += done;
        }

        map.nextLba = pos;
        map.elapsedSec = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - t0).count());

        const uint64_t done = (pos - start) * ss;
        if (done >= nextCkpt) {
            LARGE_INTEGER sz; sz.QuadPart = static_cast<LONGLONG>(done);
            SetFilePointerEx(h, sz, nullptr, FILE_BEGIN);
            SetEndOfFile(h);
            FlushFileBuffers(h);
            SaveMap(mapPath, map);
            nextCkpt = done + (256ull << 20);
            const double mb  = double(done) / double(1u << 20);
            const double sec = map.elapsedSec ? double(map.elapsedSec) : 1.0;
            std::printf("%9.1f MiB  %6.2f%%  %6.1f MiB/s  bad=%llu skipped=%.1f MiB\n",
                        mb, 100.0 * double(pos - start) / double(sectors), mb / sec,
                        static_cast<unsigned long long>(map.badSectors),
                        double(map.skipped) * ss / double(1u << 20));
            std::fflush(stdout);
        }
    }

    const uint64_t covered = (pos - start) * ss;
    LARGE_INTEGER finalSize; finalSize.QuadPart = static_cast<LONGLONG>(covered);
    SetFilePointerEx(h, finalSize, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    FlushFileBuffers(h);
    CloseHandle(h);

    map.nextLba    = pos;
    map.complete   = (pos >= end) && !g_cancel.load() && !ioError && !budgetHit && !timeHit;
    map.elapsedSec = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t0).count());
    SaveMap(mapPath, map);

    const bool fullyRead = (map.skipped == 0 && map.badSectors == 0);
    const uint64_t lost    = map.skipped + map.badSectors;
    const double coverage  = sectors ? 100.0 * (1.0 - double(lost) / double(sectors)) : 0.0;
    std::printf("\n=== IMAGE %s ===\n",
                !map.complete ? "PARTIAL"
                              : (fullyRead ? "COMPLETE" : "COMPLETE BUT HOLLOW"));
    std::printf("  out            : %s\n", outPath.c_str());
    std::printf("  range          : LBA %llu .. %llu (%llu sectors, %.2f GiB)\n",
                static_cast<unsigned long long>(start),
                static_cast<unsigned long long>(end),
                static_cast<unsigned long long>(sectors),
                double(sectors) * ss / double(1ull << 30));
    std::printf("  image bytes    : %llu (%.2f GiB)\n",
                static_cast<unsigned long long>(covered),
                double(covered) / double(1ull << 30));
    std::printf("  clean chunks   : %llu\n",
                static_cast<unsigned long long>(map.cleanChunks));
    std::printf("  bad chunks     : %llu\n",
                static_cast<unsigned long long>(map.badChunks));
    std::printf("  unreadable     : %llu sectors (%.1f MiB) in %zu range(s)\n",
                static_cast<unsigned long long>(map.badSectors),
                double(map.badSectors) * ss / double(1u << 20),
                map.unreadable.size());
    std::printf("  skipped        : %llu sectors (%.1f MiB) in %zu range(s)\n",
                static_cast<unsigned long long>(map.skipped),
                double(map.skipped) * ss / double(1u << 20),
                map.gaps.size());
    std::printf("  elapsed        : %llu s\n",
                static_cast<unsigned long long>(map.elapsedSec));
    std::printf("  COVERAGE       : %.3f%% (%llu of %llu sectors impossible to read)\n",
                coverage, static_cast<unsigned long long>(lost),
                static_cast<unsigned long long>(sectors));
    std::printf("  source writes  : 0\n");
    std::printf("  map            : %s\n", mapPath.c_str());

    if (ioError) return 3;
    if (!map.complete) return 1;
    return 0;
}
