// A1-4b-a -- configuration-driven, non-GUI composition of read-only sources.
//
// Verifies that a text configuration can assemble
//   container -> RAID (mdadm or conservative probe) -> BitLocker -> ReFS
// from injected byte sources, with NO GUI, NO device handle and NO writes.
//
// Expectations come from three independent golden tables:
//   * testdata/raid-probe-golden/chain.tsv    (Python oracle virtual volume)
//   * testdata/bitlocker-golden/sectors.tsv   (A1-3a decrypted-sector oracle)
//   * testdata/refs-golden/files.tsv          (A1-5a directory oracle)

#include "disk/ScanSourceChain.h"
#include "disk/BitLockerCrypto.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace phoenix;

namespace {

int g_fail = 0;
int g_checks = 0;

#define CHECK(c)                                                             \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(c)) {                                                          \
            std::cerr << "FAIL line " << __LINE__ << ": " #c "\n";           \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

#define CHECK_MSG(c, msg)                                                    \
    do {                                                                     \
        ++g_checks;                                                          \
        if (!(c)) {                                                          \
            std::cerr << "FAIL line " << __LINE__ << ": " << (msg) << "\n";  \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

uint64_t Fnv1a(const uint8_t* p, size_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

std::string HexBytes(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

std::string Sha256Hex(const std::vector<uint8_t>& data)
{
    const auto h = bitlocker_crypto::Sha256(data);
    return HexBytes(h.data(), h.size());
}

std::vector<std::string> Split(const std::string& s, char sep)
{
    std::vector<std::string> v;
    size_t a = 0;
    for (;;) {
        const size_t p = s.find(sep, a);
        v.push_back(s.substr(a, p == std::string::npos ? p : p - a));
        if (p == std::string::npos) break;
        a = p + 1;
    }
    return v;
}

class VecReader : public IVirtualDiskReader
{
public:
    explicit VecReader(std::vector<uint8_t> bytes) : m_bytes(std::move(bytes)) {}
    uint64_t Size() const override { return m_bytes.size(); }
    bool Read(uint64_t off, void* dst, size_t len) const override
    {
        if (off > m_bytes.size() || static_cast<uint64_t>(len) > m_bytes.size() - off)
            return false;
        if (len) std::memcpy(dst, m_bytes.data() + off, len);
        return true;
    }
    const std::vector<uint8_t>& Bytes() const { return m_bytes; }
private:
    std::vector<uint8_t> m_bytes;
};

std::vector<uint8_t> Slurp(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
}

std::string SlurpText(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    std::ostringstream s; s << f.rdbuf(); return s.str();
}

std::string FindRoot()
{
    std::vector<std::string> candidates;
    const std::string file = __FILE__;
    const size_t slash = file.find_last_of("/\\");
    if (slash != std::string::npos) {
        const std::string tests = file.substr(0, slash);
        const size_t up = tests.find_last_of("/\\");
        if (up != std::string::npos) candidates.push_back(tests.substr(0, up));
    }
    const char* rel[] = { ".", "..", "../..", "../../..", "../../../..",
                          "D:/dev/PhoenixRecover" };
    for (const char* r : rel) candidates.push_back(r);
    for (const std::string& d : candidates) {
        std::ifstream f(d + "/testdata/raid-probe-golden/cases.tsv");
        if (f.good()) return d;
    }
    return {};
}

// Inject all member images of a case as named slots. Owners keep the bytes
// alive for the whole chain lifetime.
struct Fixture
{
    std::vector<std::unique_ptr<VecReader>> owners;
    std::map<std::string, const IVirtualDiskReader*> slots;
    std::vector<std::vector<uint8_t>> originals;

    bool Load(const std::string& root, const std::string& name, uint32_t supplied)
    {
        for (uint32_t i = 0; i < supplied; ++i) {
            char path[512];
            std::snprintf(path, sizeof(path),
                          "%s/testdata/raid-probe-golden/members/%s/disk%02u.bin",
                          root.c_str(), name.c_str(), i);
            std::vector<uint8_t> b = Slurp(path);
            if (b.empty()) return false;
            originals.push_back(b);
            owners.emplace_back(new VecReader(b));
            slots["slot" + std::to_string(i)] = owners.back().get();
        }
        return true;
    }

    bool Unchanged() const
    {
        for (size_t i = 0; i < owners.size(); ++i)
            if (owners[i]->Bytes() != originals[i]) return false;
        return true;
    }
};

} // namespace

int main()
{
    const std::string root = FindRoot();
    if (root.empty()) {
        std::cerr << "FATAL: golden corpus root not found\n";
        return 1;
    }
    std::cout << "root: " << root << "\n";
    const std::string gdir = root + "/testdata/raid-probe-golden";

    // ------------------------------------------------------------------
    // 1. Strict configuration parser
    // ------------------------------------------------------------------
    {
        ChainSpec s; std::string e;
        CHECK(ParseChainConfig("", s, e) == ChainParseResult::EmptyConfig);
        CHECK(ParseChainConfig("# comment only\n", s, e) == ChainParseResult::EmptyConfig);
        CHECK(ParseChainConfig("members=a\nfoo=bar\n", s, e) == ChainParseResult::UnknownKey);
        CHECK(ParseChainConfig("members=a\nmembers=b\n", s, e) == ChainParseResult::DuplicateKey);
        CHECK(ParseChainConfig("members=a\nraid=99\n", s, e) == ChainParseResult::BadValue);
        CHECK(ParseChainConfig("members=a\nrefs=maybe\n", s, e) == ChainParseResult::BadValue);
        CHECK(ParseChainConfig("members=a\nraid.probe_anchor=zz:00\n", s, e) ==
              ChainParseResult::BadValue);
        CHECK(ParseChainConfig("members=a\nraid.probe_chunks=1000\n", s, e) ==
              ChainParseResult::BadValue);
        CHECK(ParseChainConfig("raid=auto\n", s, e) == ChainParseResult::MissingMembers);
        CHECK(ParseChainConfig("members = a ; b ; c\nraid = auto\n", s, e) ==
              ChainParseResult::Ok);
        CHECK(s.members.size() == 3u && s.members[1] == "b");
        CHECK(s.raidMode == ChainRaidMode::Auto);

        const std::string cfg =
            "name=full\n"
            "members=a;b\n"
            "container=auto\n"
            "raid=5\n"
            "raid.layout=left-symmetric\n"
            "raid.chunk=65536\n"
            "raid.data_offset=4096\n"
            "raid.missing=1\n"
            "raid.probe_stripes=3\n"
            "raid.probe_sectors=16\n"
            "raid.probe_max_candidates=500\n"
            "raid.probe_max_permutations=120\n"
            "raid.probe_use_metadata=false\n"
            "raid.probe_allow_blind=true\n"
            "raid.probe_chunks=4096;8192;65536\n"
            "raid.probe_anchor=0:01020304\n"
            "raid.probe_anchor=2000:aabbccdd\n"
            "unlock=recovery\n"
            "secret=SUPERSECRET-DO-NOT-LOG\n"
            "refs=1\n";
        CHECK(ParseChainConfig(cfg, s, e) == ChainParseResult::Ok);
        CHECK(s.raidConfig.level == RaidLevel::Raid5);
        CHECK(s.raidConfig.chunkBytes == 65536u);
        CHECK(s.raidConfig.dataOffset == 4096u);
        CHECK(s.raidConfig.missing.size() == 1u && s.raidConfig.missing[0] == 1u);
        CHECK(s.probeOptions.stripesToCheck == 3u);
        CHECK(s.probeOptions.sectorsPerStripe == 16u);
        CHECK(s.probeOptions.maxCandidates == 500u);
        CHECK(s.probeOptions.maxPermutations == 120u);
        CHECK(!s.probeOptions.useMetadata && s.probeOptions.allowBlind);
        CHECK(s.probeOptions.chunkCandidates.size() == 3u);
        CHECK(s.probeOptions.anchors.size() == 2u);
        CHECK(s.probeOptions.anchors[1].offset == 0x2000u);
        CHECK(s.probeOptions.anchors[1].bytes.size() == 4u);
        CHECK(s.unlock == ChainUnlock::RecoveryPassword);
        CHECK(s.refs);
    }

    // ------------------------------------------------------------------
    // 2. Golden chain: config text -> SourceChain -> per-layer fingerprints
    // ------------------------------------------------------------------
    {
        std::ifstream f(gdir + "/chain.tsv");
        std::string line;
        std::string current;
        SourceChain chain;
        const IVirtualDiskReader* head = nullptr;
        Fixture fx;
        int rows = 0, opened = 0;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            const auto c = Split(line, '\t');
            if (c.size() < 6) continue;
            const std::string& name = c[0];
            if (name != current) {
                current = name;
                chain.Close();
                fx = Fixture{};
                const std::string cfgText = SlurpText(gdir + "/configs/" + name + ".conf");
                CHECK_MSG(!cfgText.empty(), name + ": config missing");
                ChainSpec spec; std::string err;
                const ChainParseResult pr = ParseChainConfig(cfgText, spec, err);
                CHECK_MSG(pr == ChainParseResult::Ok, name + ": " + err);
                uint32_t supplied = static_cast<uint32_t>(spec.members.size());
                CHECK_MSG(fx.Load(root, name, supplied), name + ": members missing");
                const ChainOpenResult r = chain.Open(spec, fx.slots);
                CHECK_MSG(r == ChainOpenResult::Ok,
                          name + ": chain open " + ChainOpenDetail(r) + " -- " +
                          chain.LastError());
                head = chain.Head();
                CHECK_MSG(head != nullptr, name + ": null head");
                if (r == ChainOpenResult::Ok) ++opened;
                CHECK_MSG(!chain.Report().empty(), name + ": empty diagnostic report");
            }
            ++rows;
            if (head == nullptr) continue;
            const uint64_t off    = std::stoull(c[2]);
            const size_t   length = static_cast<size_t>(std::stoull(c[3]));
            std::vector<uint8_t> buf(length, 0);
            const bool ok = head->Read(off, buf.data(), buf.size());
            CHECK_MSG(ok, name + ": head read failed at " + c[2]);
            if (!ok) continue;
            std::ostringstream fnv;
            fnv << std::hex << std::setw(16) << std::setfill('0') << Fnv1a(buf.data(), buf.size());
            CHECK_MSG(fnv.str() == c[4],
                      name + ": FNV mismatch at " + c[2] + " (" + fnv.str() + " != " + c[4] + ")");
            CHECK_MSG(Sha256Hex(buf) == c[5], name + ": SHA-256 mismatch at " + c[2]);
        }
        CHECK(rows >= 16);
        CHECK(opened >= 4);
        // Source images must be byte-identical after a full chain read.
        CHECK_MSG(fx.Unchanged(), "last chain case mutated a member image");
    }

    // ------------------------------------------------------------------
    // 3. BitLocker wiring (expectations from the A1-3a sector oracle)
    // ------------------------------------------------------------------
    {
        const std::string vol = root + "/testdata/bitlocker-golden/xts128_clear.bin";
        std::vector<uint8_t> bytes = Slurp(vol);
        CHECK_MSG(!bytes.empty(), "BitLocker golden volume missing");
        VecReader reader(bytes);
        std::map<std::string, const IVirtualDiskReader*> slots{{"vol0", &reader}};
        ChainSpec spec; std::string err;
        const std::string cfg =
            "name=bitlocker_chain\nmembers=vol0\ncontainer=raw\n"
            "raid=none\nunlock=clear\nrefs=0\n";
        CHECK(ParseChainConfig(cfg, spec, err) == ChainParseResult::Ok);
        SourceChain chain;
        const ChainOpenResult r = chain.Open(spec, slots);
        CHECK_MSG(r == ChainOpenResult::Ok,
                  std::string("BitLocker chain: ") + ChainOpenDetail(r) + " -- " +
                  chain.LastError());
        if (r == ChainOpenResult::Ok) {
            std::ifstream sf(root + "/testdata/bitlocker-golden/sectors.tsv");
            std::string line; int sectorRows = 0;
            while (std::getline(sf, line)) {
                if (line.empty() || line[0] == '#') continue;
                const auto c = Split(line, '\t');
                if (c.size() < 4 || c[0] != "xts128_clear") continue;
                ++sectorRows;
                const uint64_t off = std::stoull(c[1]);
                const size_t len = static_cast<size_t>(std::stoull(c[2]));
                std::vector<uint8_t> buf(len, 0);
                CHECK_MSG(chain.Head()->Read(off, buf.data(), buf.size()),
                          "BitLocker head read at " + c[1]);
                std::ostringstream fnv;
                fnv << std::hex << std::setw(16) << std::setfill('0')
                    << Fnv1a(buf.data(), buf.size());
                CHECK_MSG(fnv.str() == c[3],
                          "BitLocker sector " + c[1] + " FNV " + fnv.str() + " != " + c[3]);
            }
            CHECK(sectorRows >= 4);
            CHECK(chain.Head()->Size() == 262144u);
        }
        CHECK(reader.Bytes() == bytes);
    }

    // ------------------------------------------------------------------
    // 4. ReFS wiring (expectations from the A1-5a directory oracle)
    // ------------------------------------------------------------------
    {
        const std::string vol = root + "/testdata/refs-golden/refs_v314_crc64_basic.bin";
        std::vector<uint8_t> bytes = Slurp(vol);
        CHECK_MSG(!bytes.empty(), "ReFS golden volume missing");
        VecReader reader(bytes);
        std::map<std::string, const IVirtualDiskReader*> slots{{"vol0", &reader}};
        ChainSpec spec; std::string err;
        const std::string cfg =
            "name=refs_chain\nmembers=vol0\ncontainer=none\n"
            "raid=none\nunlock=none\nrefs=1\n";
        CHECK(ParseChainConfig(cfg, spec, err) == ChainParseResult::Ok);
        SourceChain chain;
        const ChainOpenResult r = chain.Open(spec, slots);
        CHECK_MSG(r == ChainOpenResult::Ok,
                  std::string("ReFS chain: ") + ChainOpenDetail(r) + " -- " + chain.LastError());
        if (r == ChainOpenResult::Ok) {
            CHECK(chain.RefsImage() != nullptr);
            refs::RefsVolume volObj;
            const refs::RefsStatus st = volObj.Open(*chain.RefsImage());
            CHECK_MSG(st == refs::RefsStatus::Ok,
                      std::string("RefsVolume via bridge: ") + refs::RefsStatusName(st) +
                      " -- " + volObj.LastError());
            if (st == refs::RefsStatus::Ok) {
                const auto list = volObj.ListDirectory(1536u);   // 0x600 root
                std::ifstream ff(root + "/testdata/refs-golden/files.tsv");
                std::string line; int expectedRows = 0;
                while (std::getline(ff, line)) {
                    if (line.empty() || line[0] == '#') continue;
                    const auto c = Split(line, '\t');
                    if (c.size() < 6 || c[0] != "refs_v314_crc64_basic" ||
                        c[1] != "1536") continue;
                    ++expectedRows;
                    bool found = false;
                    for (const auto& e : list) {
                        std::string n(e.name.begin(), e.name.end());
                        if (n == c[2] && e.size == std::stoull(c[4]) &&
                            e.isDirectory == (c[3] == "1")) { found = true; break; }
                    }
                    CHECK_MSG(found, "ReFS directory entry missing via chain: " + c[2]);
                }
                CHECK(expectedRows >= 4);
            }
        }
        CHECK(reader.Bytes() == bytes);
    }

    // ------------------------------------------------------------------
    // 5. Refusal semantics and secret hygiene
    // ------------------------------------------------------------------
    {
        // A refused probe must never fall back to scanning a single member.
        Fixture fx;
        CHECK(fx.Load(root, "blind_no_anchors", 3));
        const std::string cfgText = SlurpText(gdir + "/configs/blind_no_anchors.conf");
        ChainSpec spec; std::string err;
        CHECK(ParseChainConfig(cfgText, spec, err) == ChainParseResult::Ok);
        SourceChain chain;
        const ChainOpenResult r = chain.Open(spec, fx.slots);
        CHECK(r == ChainOpenResult::ProbeFailed);
        CHECK(chain.Head() == nullptr);
        CHECK(!chain.IsOpen());
        CHECK(fx.Unchanged());
    }
    {
        // Secrets must never appear in the diagnostic report or error text.
        std::vector<uint8_t> bytes = Slurp(root + "/testdata/bitlocker-golden/xts128_clear.bin");
        VecReader reader(bytes);
        std::map<std::string, const IVirtualDiskReader*> slots{{"vol0", &reader}};
        ChainSpec spec; std::string err;
        const std::string cfg =
            "name=secret_hygiene\nmembers=vol0\ncontainer=none\n"
            "raid=none\nunlock=recovery\n"
            "secret=TOPSECRET-PLAINTEXT-42\nrefs=0\n";
        CHECK(ParseChainConfig(cfg, spec, err) == ChainParseResult::Ok);
        SourceChain chain;
        const ChainOpenResult r = chain.Open(spec, slots);
        CHECK(r == ChainOpenResult::UnlockFailed || r == ChainOpenResult::UnlockOpenFailed);
        CHECK(chain.Head() == nullptr);
        std::string all = chain.Describe() + " " + chain.LastError();
        for (const std::string& s : chain.Report()) all += " " + s;
        CHECK(all.find("TOPSECRET") == std::string::npos);
        CHECK(chain.Describe().find("redacted") != std::string::npos);
    }
    {
        // Unknown slot / invalid raid=none member count are hard errors.
        ChainSpec spec; std::string err;
        CHECK(ParseChainConfig("members=a;b\nraid=none\n", spec, err) ==
              ChainParseResult::Ok);
        SourceChain chain;
        std::map<std::string, const IVirtualDiskReader*> empty;
        CHECK(chain.Open(spec, empty) == ChainOpenResult::MissingSlot);
        std::vector<uint8_t> one(4096, 0);
        VecReader r1(one), r2(one);
        std::map<std::string, const IVirtualDiskReader*> two{{"a", &r1}, {"b", &r2}};
        CHECK(chain.Open(spec, two) == ChainOpenResult::BadConfig);
    }

    std::cout << "ScanSourceChain: " << g_checks << " checks, " << g_fail
              << " failures\n";
    return g_fail != 0;
}
