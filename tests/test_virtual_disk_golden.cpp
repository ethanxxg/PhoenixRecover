// A1-1c: golden regression against REAL virtual disk images.
//
// The oracle (testdata/virtual-disk-golden/expectations.tsv) is produced by a
// separate, independently written Python reference reader
// (testdata/tools/Verify-VirtualDiskGolden.py). This test opens the very same
// real Hyper-V / Virtual PC / VMware / GRUB images through the production C++
// VirtualDiskSource and must reproduce, sector for sector, every fingerprint.
//
// Samples live in testdata/ (not version-controlled, consistent with the rest
// of the project's forensic corpora) and are reproducible with
//   python testdata/tools/Fetch-VirtualDiskGolden.py
//   python testdata/tools/Verify-VirtualDiskGolden.py
// If they are absent the test prints an explicit SKIP and returns 0 so a
// checkout without the corpora stays green; CTest output makes the skip
// visible. Never silently pass when expectations exist but do not match.
//
// Strictly read-only: FileVirtualDiskReader opens with std::ios::in.

#include "disk/VirtualDiskSource.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef PHOENIX_GOLDEN_DIR
#define PHOENIX_GOLDEN_DIR "testdata/virtual-disk-golden/"
#endif

using namespace phoenix;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            std::cerr << "FAIL " << (msg) << " (line " << __LINE__ << ")\n";\
            return 1;                                                       \
        }                                                                   \
    } while (0)

namespace {

std::string Hex64(uint64_t v)
{
    static const char* digits = "0123456789abcdef";
    std::string s(16, '0');
    for (int i = 15; i >= 0; --i) {
        s[static_cast<size_t>(i)] = digits[v & 0xFULL];
        v >>= 4;
    }
    return s;
}

std::string Fnv1a(const uint8_t* p, size_t n)
{
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ULL;
    }
    return Hex64(h);
}

std::vector<std::string> Split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::istringstream is(s);
    std::string part;
    while (std::getline(is, part, sep)) out.push_back(part);
    return out;
}

struct GoldenRow
{
    std::string name;
    int         format       = 0;
    uint64_t    virtualBytes = 0;
    uint32_t    sectorSize   = 0;
    uint64_t    blockSize    = 0;
    uint64_t    fileSize     = 0;
    std::vector<std::pair<uint64_t, std::string>> probes; // OOR = out of range
};

// Open results are a property of the production implementation, so they are
// pinned here rather than derived from the Python reference reader.
const std::map<std::string, VirtualDiskOpenResult>& ExpectedOpenResults()
{
    static const std::map<std::string, VirtualDiskOpenResult> k = {
        {"virtualpc-dynamic.vhd",            VirtualDiskOpenResult::Ok},
        {"hyperv2012r2-dynamic.vhd",         VirtualDiskOpenResult::Ok},
        {"d2v-zerofilled.vhd",               VirtualDiskOpenResult::Ok},
        {"iotest-dynamic-1G.vhdx",           VirtualDiskOpenResult::Ok},
        {"test-disk2vhd.vhdx",               VirtualDiskOpenResult::Ok},
        {"iotest-dirtylog-10G-4M.vhdx",      VirtualDiskOpenResult::Ok},
        {"iotest-version3.vmdk",             VirtualDiskOpenResult::UnsupportedVariant},
        {"afl9.vmdk",                        VirtualDiskOpenResult::BadHeader},
        {"grub_mbr.raw",                     VirtualDiskOpenResult::Ok},
        {"afl5.img",                         VirtualDiskOpenResult::Ok},
    };
    return k;
}

bool LoadRows(const std::string& path, std::vector<GoldenRow>& rows)
{
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const std::vector<std::string> f = Split(line, '\t');
        if (f.size() != 7) {
            std::cerr << "FAIL malformed oracle row: " << line << "\n";
            return false;
        }
        GoldenRow r;
        r.name         = f[0];
        r.format       = std::stoi(f[1]);
        r.virtualBytes = std::stoull(f[2]);
        r.sectorSize   = static_cast<uint32_t>(std::stoul(f[3]));
        r.blockSize    = std::stoull(f[4]);
        r.fileSize     = std::stoull(f[5]);
        if (f[6] != "-") {
            for (const std::string& p : Split(f[6], ',')) {
                const size_t pos = p.find(':');
                if (pos == std::string::npos) return false;
                r.probes.emplace_back(std::stoull(p.substr(0, pos)),
                                      p.substr(pos + 1));
            }
        }
        rows.push_back(std::move(r));
    }
    return !rows.empty();
}

// CTest runs from a build subdirectory, while manual runs usually start at the
// project root. Probe a few locations so the corpus is found in both cases.
std::string ResolveGoldenDir()
{
    std::vector<std::string> candidates;
    if (const char* env = std::getenv("PHOENIX_GOLDEN_DIR")) {
        if (*env) candidates.emplace_back(env);
    }
    candidates.emplace_back(PHOENIX_GOLDEN_DIR);
    candidates.emplace_back("../testdata/virtual-disk-golden/");
    candidates.emplace_back("../../testdata/virtual-disk-golden/");
    candidates.emplace_back("../../../testdata/virtual-disk-golden/");
    candidates.emplace_back("../../../../testdata/virtual-disk-golden/");
    for (std::string d : candidates) {
        if (d.empty()) continue;
        if (d.back() != '/' && d.back() != '\\') d += '/';
        if (std::ifstream(d + "expectations.tsv").good()) return d;
    }
    return std::string(PHOENIX_GOLDEN_DIR) + "/";
}

bool CheckSemanticAnchors(const GoldenRow& row, VirtualDiskSource& src)
{
    // Facts visible in the real images themselves, independent of BOTH
    // parsers: a genuine MBR prologue and qemu-iotest's 0xA5 fill pattern.
    uint8_t buf[512] = {};
    if (row.name == "test-disk2vhd.vhdx") {
        if (!src.ReadSectors(0, 1, buf)) return false;
        if (!(buf[0] == 0x33 && buf[1] == 0xC0 && buf[2] == 0x8E)) {
            std::cerr << "FAIL real MBR prologue missing from " << row.name << "\n";
            return false;
        }
    } else if (row.name == "grub_mbr.raw") {
        if (!src.ReadSectors(0, 1, buf)) return false;
        if (!(buf[0] == 0xEB && buf[1] == 0x63 && buf[2] == 0x90)) {
            std::cerr << "FAIL GRUB MBR prologue missing\n";
            return false;
        }
    } else if (row.name == "iotest-dynamic-1G.vhdx" ||
               row.name == "iotest-dirtylog-10G-4M.vhdx") {
        if (!src.ReadSectors(0, 1, buf)) return false;
        for (int i = 0; i < 512; ++i) {
            if (buf[i] != 0xA5) {
                std::cerr << "FAIL qemu iotest 0xA5 pattern broken at byte "
                          << i << " in " << row.name << "\n";
                return false;
            }
        }
    }
    if (row.name == "iotest-dirtylog-10G-4M.vhdx" &&
        src.Info().warnings.empty()) {
        std::cerr << "FAIL VHDX dirty-log warning not surfaced\n";
        return false;
    }
    return true;
}

} // namespace

int main()
{
    const std::string dir = ResolveGoldenDir();
    const std::string oracle = dir + "expectations.tsv";
    std::vector<GoldenRow> rows;
    if (!LoadRows(oracle, rows)) {
        if (!std::ifstream(oracle).good()) {
            std::cout << "SKIPPED VirtualDiskGolden: real-image corpus absent.\n"
                      << "  run: python testdata/tools/Fetch-VirtualDiskGolden.py\n"
                      << "       python testdata/tools/Verify-VirtualDiskGolden.py\n";
            return 0;
        }
        std::cerr << "FAIL cannot parse " << oracle << "\n";
        return 1;
    }

    const auto& expected = ExpectedOpenResults();
    size_t sectorChecks = 0, oorChecks = 0;
    for (const GoldenRow& row : rows) {
        const auto it = expected.find(row.name);
        CHECK(it != expected.end(), "unexpected sample " + row.name);

        FileVirtualDiskReader reader;
        CHECK(reader.Open(dir + row.name), "cannot open sample " + row.name);
        CHECK(reader.Size() == row.fileSize, "size mismatch " + row.name);

        VirtualDiskSource src(reader);
        const VirtualDiskOpenResult res = src.Open();
        CHECK(res == it->second,
              std::string("open result mismatch ") + row.name + ": got " +
              VirtualDiskOpenDetail(res) + ", want " +
              VirtualDiskOpenDetail(it->second));
        CHECK(static_cast<int>(src.Format()) == row.format,
              "format mismatch " + row.name);
        CHECK(src.Info().virtualBytes == row.virtualBytes,
              "virtual size mismatch " + row.name);
        CHECK(src.Info().sectorSize == row.sectorSize,
              "sector size mismatch " + row.name);
        CHECK(src.Info().blockSize == row.blockSize,
              "block size mismatch " + row.name);

        if (res == VirtualDiskOpenResult::Ok) {
            CHECK(src.SectorCount() == row.virtualBytes / 512,
                  "sector count mismatch " + row.name);
            CHECK(CheckSemanticAnchors(row, src),
                  "semantic anchor failure " + row.name);
        }

        uint8_t buf[512] = {};
        for (const auto& probe : row.probes) {
            const uint64_t lba = probe.first;
            const bool wantOor = (probe.second == "OOR");
            const bool ok = src.ReadSectors(lba, 1, buf);
            if (wantOor) {
                CHECK(!ok, "out-of-range LBA accepted " + row.name);
                ++oorChecks;
            } else {
                CHECK(ok, "read failed " + row.name + " LBA " +
                      std::to_string(lba));
                const std::string got = Fnv1a(buf, 512);
                CHECK(got == probe.second,
                      "sector mismatch " + row.name + " LBA " +
                      std::to_string(lba) + " got " + got + " want " +
                      probe.second);
                ++sectorChecks;
            }
        }
        std::cout << "  OK " << row.name << " (" << row.probes.size()
                  << " probes)\n";
    }

    std::cout << "VirtualDiskGolden: " << rows.size() << " real images, "
              << sectorChecks << " sector fingerprints, " << oorChecks
              << " out-of-range checks -- all matched\n";
    return 0;
}
