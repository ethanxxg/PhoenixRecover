// A1-5a ReFS integrity-primitive regression.
//
// Cross-checks the C++ checksums against (a) hard-coded standard check values
// and (b) the independent pure-Python oracle's primitives.tsv, which also
// carries the VBR ROR1+ADD checksum of every synthetic volume.
//
// Zero GUI / zero device I/O / zero writes.

#include "../fs/refs/RefsCrypto.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace phoenix::refs;

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, (what));                 \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static std::vector<std::string> SplitTabs(const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    std::istringstream is(line);
    while (std::getline(is, cur, '\t')) out.push_back(cur);
    return out;
}

static std::vector<std::string> ReadLines(const std::string& path)
{
    std::vector<std::string> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] != '#') out.push_back(line);
    }
    return out;
}

static std::string Hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

static std::string GoldenDir()
{
    std::vector<std::string> candidates;
    const char* env = std::getenv("PHOENIX_REFS_GOLDEN");
    if (env && *env) candidates.emplace_back(std::string(env) + "/");
    candidates.emplace_back("testdata/refs-golden/");
    candidates.emplace_back("../testdata/refs-golden/");
    candidates.emplace_back("../../testdata/refs-golden/");
    candidates.emplace_back("../../../testdata/refs-golden/");
    candidates.emplace_back("../../../../testdata/refs-golden/");
    for (const std::string& d : candidates) {
        std::ifstream f(d + "primitives.tsv");
        if (f.good()) return d;
    }
    return "testdata/refs-golden/";
}

int main()
{
    // ---- Standard check values (FIPS / CRC catalogue) ---------------------
    const uint8_t digits[] = "123456789";
    CHECK(Crc32C(digits, 9) == 0xE3069283u, "CRC-32C(\"123456789\")");
    CHECK(Crc64Nvme(digits, 9) == 0xAE8B14860A799888ull,
          "CRC-64/NVME(\"123456789\")");
    const uint8_t abc[] = "abc";
    CHECK(Hex(Sha256Of(abc, 3).data(), 32) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256(\"abc\")");
    CHECK(Crc32C(digits, 0) == 0u, "CRC-32C(empty) = 0 (xorout of init)");
    CHECK(Crc64Nvme(digits, 0) == 0ull, "CRC-64/NVME(empty) = 0");
    CHECK(Fnv1a64(abc, 3) == 0xe71fa2190541574bull, "FNV-1a64(\"abc\")");

    // Incremental CRC must equal the one-shot value (used for streamed pages).
    {
        std::vector<uint8_t> data(4096);
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xFFu);
        uint32_t c32 = 0xFFFFFFFFu;
        uint64_t c64 = 0xFFFFFFFFFFFFFFFFull;
        for (size_t i = 0; i < data.size(); i += 64) {
            c32 = Crc32CUpdate(c32, data.data() + i, 64);
            c64 = Crc64NvmeUpdate(c64, data.data() + i, 64);
        }
        CHECK(Crc32CFinish(c32) == Crc32C(data.data(), data.size()),
              "incremental CRC-32C");
        CHECK(Crc64NvmeFinish(c64) == Crc64Nvme(data.data(), data.size()),
              "incremental CRC-64/NVME");
    }

    // ---- VBR ROR1+ADD self-checksum ---------------------------------------
    {
        uint8_t vbr[512] = {};
        std::memcpy(vbr + 0x03, "ReFS\0\0\0\0", 8);
        std::memcpy(vbr + 0x10, "FSRS", 4);
        vbr[0x20] = 0; vbr[0x21] = 2;   // 512
        for (int i = 0x30; i < 512; ++i) vbr[i] = static_cast<uint8_t>(i & 0xFF);
        const uint16_t a = VbrChecksum(vbr);
        vbr[0x16] = static_cast<uint8_t>(a & 0xFF);
        vbr[0x17] = static_cast<uint8_t>(a >> 8);
        CHECK(VbrChecksum(vbr) == a, "stored VBR checksum bytes are excluded");
        vbr[0x30] ^= 0x01;
        CHECK(VbrChecksum(vbr) != a, "VBR checksum detects a flipped byte");
    }

    // ---- Independent Python oracle vectors --------------------------------
    const std::string dir = GoldenDir();
    std::printf("RefsCrypto golden dir: %s\n", dir.c_str());
    const auto lines = ReadLines(dir + "primitives.tsv");
    CHECK(lines.size() >= 16, "primitives.tsv has all vectors");
    int compared = 0;
    for (size_t li = 1; li < lines.size(); ++li) {
        const auto c = SplitTabs(lines[li]);
        if (c.size() < 4) { CHECK(false, "malformed primitives.tsv row"); continue; }
        const std::string& kind = c[0];
        const std::string& input = c[1];
        const std::string& expected = c[2];
        const std::string& reference = c[3];
        if (kind == "vbr_checksum") {
            // Two corpus cases deliberately carry a wrong / absent stored VBR
            // checksum; there the mismatch IS the expected negative signal.
            const bool negative = input.find("bad_vbr_checksum") != std::string::npos ||
                                  input.find("not_refs") != std::string::npos;
            if (negative)
                CHECK(expected != reference,
                      (kind + "[" + input + "] negative mismatch preserved").c_str());
            else
                CHECK(expected == reference,
                      (kind + "[" + input + "] oracle self-consistency").c_str());
        } else {
            CHECK(expected == reference, (kind + " oracle self-consistency").c_str());
        }

        std::string got;
        if (kind == "crc32c") {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%08x",
                          Crc32C(reinterpret_cast<const uint8_t*>(input.data()),
                                 input.size()));
            got = buf;
        } else if (kind == "crc64_nvme") {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%016llx",
                          static_cast<unsigned long long>(
                              Crc64Nvme(reinterpret_cast<const uint8_t*>(input.data()),
                                        input.size())));
            got = buf;
        } else if (kind == "sha256") {
            got = Hex(Sha256Of(reinterpret_cast<const uint8_t*>(input.data()),
                               input.size()).data(), 32);
        } else if (kind == "vbr_checksum") {
            std::ifstream f(dir + input + ".bin", std::ios::binary);
            uint8_t vbr[512] = {};
            if (f.good()) {
                f.read(reinterpret_cast<char*>(vbr), 512);
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%04x", VbrChecksum(vbr));
                got = buf;
            } else {
                got = "<missing>";
            }
        } else {
            CHECK(false, ("unknown primitive " + kind).c_str());
            continue;
        }
        CHECK(got == expected, (kind + "[" + input + "]").c_str());
        ++compared;
    }
    CHECK(compared >= 15, "all primitive vectors exercised");

    std::printf("RefsCrypto: %d checks, %d failure(s), %d oracle vectors\n",
                g_checks, g_fail, compared);
    if (g_fail != 0)
        std::printf("  corpus missing? run: python testdata/tools/Gen-ReFSGolden.py\n");
    return g_fail == 0 ? 0 : 1;
}
