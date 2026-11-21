// A1-4a -- VirtualRaid read-only reassembly, cross-validated against an
// INDEPENDENT forward layout generator.
//
// Independence properties (deliberate):
//   * The oracle maps (stripe, data-chunk) -> member with closed-form
//     arithmetic, while the product enumerates role slots.
//   * The oracle computes GF(2^8) products by bitwise polynomial multiply;
//     the product uses exp/log tables. A wrong field table shows up as a
//     RAID6 two-disk rebuild failure.
//   * Virtual volume bytes are a deterministic pseudo-random stream with
//     anchors at chunk boundaries, so a wrong chunk size / rotation / member
//     order cannot accidentally compare equal.
//
// No device I/O, no Qt, no Win32. All arrays live in memory.

#include "disk/VirtualRaid.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace phoenix;

namespace {

int g_fail = 0;

#define CHECK(c)                                                            \
    do {                                                                    \
        if (!(c)) {                                                         \
            std::cerr << "FAIL line " << __LINE__ << ": " #c "\n";          \
            ++g_fail;                                                       \
        }                                                                   \
    } while (0)

#define REQUIRE(c)                                                          \
    do {                                                                    \
        if (!(c)) {                                                         \
            std::cerr << "FATAL line " << __LINE__ << ": " #c "\n";         \
            ++g_fail;                                                       \
            return 1;                                                       \
        }                                                                   \
    } while (0)

const uint32_t SECTOR = 512;

// ---------------------------------------------------------------------------
// Independent GF(2^8) implementation: shift-and-reduce, no lookup tables.
// ---------------------------------------------------------------------------
uint8_t GfMulSlow(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1u) r = static_cast<uint8_t>(r ^ a);
        const uint8_t hi = static_cast<uint8_t>(a & 0x80u);
        a = static_cast<uint8_t>(a << 1);
        if (hi) a = static_cast<uint8_t>(a ^ 0x1Du);
        b = static_cast<uint8_t>(b >> 1);
    }
    return r;
}

uint8_t GfPowSlow(uint32_t k)
{
    uint8_t r = 1;
    for (uint32_t i = 0; i < k; ++i) r = GfMulSlow(r, 2);
    return r;
}

// ---------------------------------------------------------------------------
// Minimal byte-source for tests (independent of VirtualDiskSource helpers).
// ---------------------------------------------------------------------------
class VecReader : public IVirtualDiskReader
{
public:
    explicit VecReader(std::vector<uint8_t> bytes) : m_bytes(std::move(bytes)) {}
    uint64_t Size() const override { return m_bytes.size(); }
    bool Read(uint64_t offset, void* dst, size_t length) const override
    {
        if (offset > m_bytes.size() ||
            static_cast<uint64_t>(length) > m_bytes.size() - offset) return false;
        if (length) std::memcpy(dst, m_bytes.data() + offset, length);
        return true;
    }
    std::vector<uint8_t>& Bytes() { return m_bytes; }
    const std::vector<uint8_t>& Bytes() const { return m_bytes; }
private:
    std::vector<uint8_t> m_bytes;
};

struct ForwardOracle
{
    RaidConfig cfg;
    uint32_t n            = 0;
    uint32_t dataDisks    = 0;
    uint32_t parityCount  = 0;
    uint64_t chunkSectors = 0;
    uint64_t stripes      = 0;
    uint64_t memberBytes  = 0;
    uint64_t virtualSize  = 0;

    explicit ForwardOracle(const RaidConfig& c) : cfg(c), n(c.members)
    {
        chunkSectors = c.chunkBytes / SECTOR;
        switch (c.level) {
        case RaidLevel::Jbod:   dataDisks = 1;     break;
        case RaidLevel::Raid0:  dataDisks = n;     break;
        case RaidLevel::Raid1:  dataDisks = 1;     break;
        case RaidLevel::Raid10: dataDisks = n / 2; break;
        default:                dataDisks = n - (c.level == RaidLevel::Raid6 ? 2 : 1);
        }
        parityCount = (c.level == RaidLevel::Raid4 || c.level == RaidLevel::Raid5)
                      ? 1u : (c.level == RaidLevel::Raid6 ? 2u : 0u);
        memberBytes = c.memberBytes;
        stripes     = (c.level == RaidLevel::Jbod)
                      ? memberBytes / SECTOR : memberBytes / c.chunkBytes;
        virtualSize = (c.level == RaidLevel::Jbod)
                      ? memberBytes * n
                      : stripes * dataDisks * c.chunkBytes;
    }

    // Closed-form parity placement, expressed independently from the product's
    // role-slot enumeration. Single-parity levels use one fixed/rotating slot;
    // RAID6 uses an adjacent rotating P/Q pair.
    void Parity(uint64_t stripe, uint32_t& p, uint32_t& q, bool& hasQ) const
    {
        hasQ = (cfg.level == RaidLevel::Raid6);
        const uint32_t rot = static_cast<uint32_t>(stripe % n);
        const bool fixedFirst = (cfg.layout == RaidLayout::ParityFirst);
        const bool fixedLast  = (cfg.layout == RaidLayout::ParityLast);
        const bool left = (cfg.layout == RaidLayout::LeftSymmetric ||
                           cfg.layout == RaidLayout::LeftAsymmetric);

        if (cfg.level == RaidLevel::Raid4) {
            p = fixedFirst ? 0 : n - 1;
            q = p;
            return;
        }
        if (!hasQ) {
            if (fixedFirst)      p = 0;
            else if (fixedLast)  p = n - 1;
            else                 p = left ? (n - 1u - rot) : rot;
            q = p;
            return;
        }
        if (fixedFirst) { p = 0;     q = 1;     return; }
        if (fixedLast)  { p = n - 2; q = n - 1; return; }
        p = left ? (n - 1u - rot) : rot;
        q = left ? ((p + n - 1u) % n) : ((p + 1u) % n);
    }

    // The dd-th data chunk of `stripe` lives on this member.
    uint32_t DataDisk(uint64_t stripe, uint32_t dd) const
    {
        if (cfg.level == RaidLevel::Raid0)  return dd;
        if (cfg.level == RaidLevel::Raid10) return dd * 2;
        uint32_t p = 0, q = 0; bool hasQ = false;
        Parity(stripe, p, q, hasQ);

        if (cfg.level == RaidLevel::Raid4 ||
            cfg.layout == RaidLayout::ParityFirst ||
            cfg.layout == RaidLayout::ParityLast ||
            cfg.layout == RaidLayout::LeftAsymmetric ||
            cfg.layout == RaidLayout::RightAsymmetric) {
            // Asymmetric / fixed: walk member 0..n-1, skipping parity slots.
            uint32_t seen = 0;
            for (uint32_t d = 0; d < n; ++d) {
                if (d == p || (hasQ && d == q)) continue;
                if (seen == dd) return d;
                ++seen;
            }
            return n;
        }
        // Symmetric: start just after P, wrap around, skipping parity slots.
        uint32_t seen = 0;
        for (uint32_t s = 0; s < n; ++s) {
            const uint32_t d = (p + 1u + s) % n;
            if (d == p || (hasQ && d == q)) continue;
            if (seen == dd) return d;
            ++seen;
        }
        return n;
    }

};

struct ArrayFixture
{
    std::vector<uint8_t> volume;
    std::vector<std::unique_ptr<VecReader>> readers;
};

uint8_t ByteAt(uint64_t i, uint32_t seed)
{
    uint64_t x = i * 0x9E3779B97F4A7C15ull + seed * 0x2545F4914F6CDD1Dull + 1;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 32;
    return static_cast<uint8_t>(x & 0xFFu);
}

ArrayFixture BuildArray(const RaidConfig& cfg, uint32_t seed)
{
    ForwardOracle o(cfg);
    ArrayFixture f;
    f.volume.assign(static_cast<size_t>(o.virtualSize), 0);
    for (uint64_t i = 0; i < o.virtualSize; ++i) {
        f.volume[static_cast<size_t>(i)] = ByteAt(i, seed);
    }
    // Semantic anchors: MBR signature and a marker at every chunk start.
    if (o.virtualSize >= SECTOR) {
        f.volume[510] = 0x55;
        f.volume[511] = 0xAA;
    }
    for (uint64_t off = 0; off + 8 <= o.virtualSize; off += cfg.chunkBytes) {
        const char* magic = "PHXRAID";
        for (uint32_t k = 0; k < 7; ++k)
            f.volume[static_cast<size_t>(off + k)] = static_cast<uint8_t>(magic[k]);
    }

    const size_t memberLen = static_cast<size_t>(cfg.dataOffset + cfg.memberBytes);
    f.readers.reserve(o.n);
    for (uint32_t d = 0; d < o.n; ++d) {
        f.readers.emplace_back(new VecReader(
            std::vector<uint8_t>(memberLen, 0xCC)));
    }

    auto put = [&](uint32_t disk, uint64_t memberSector, const uint8_t* src) {
        std::vector<uint8_t>& m = f.readers[disk]->Bytes();
        const size_t off = static_cast<size_t>(cfg.dataOffset +
                                               memberSector * SECTOR);
        std::memcpy(m.data() + off, src, SECTOR);
    };
    auto get = [&](uint32_t disk, uint64_t memberSector) -> uint8_t* {
        std::vector<uint8_t>& m = f.readers[disk]->Bytes();
        return m.data() + static_cast<size_t>(cfg.dataOffset +
                                              memberSector * SECTOR);
    };

    const uint64_t volumeSectors = o.virtualSize / SECTOR;
    for (uint64_t vs = 0; vs < volumeSectors; ++vs) {
        const uint8_t* src = f.volume.data() + static_cast<size_t>(vs * SECTOR);
        if (cfg.level == RaidLevel::Jbod) {
            const uint64_t memberSectors = cfg.memberBytes / SECTOR;
            put(static_cast<uint32_t>(vs / memberSectors),
                vs % memberSectors, src);
        } else if (cfg.level == RaidLevel::Raid1) {
            for (uint32_t d = 0; d < o.n; ++d) put(d, vs, src);
        } else {
            const uint64_t perStripe =
                static_cast<uint64_t>(o.dataDisks) * o.chunkSectors;
            const uint64_t stripe = vs / perStripe;
            const uint64_t inStripe = vs % perStripe;
            const uint32_t dd = static_cast<uint32_t>(inStripe / o.chunkSectors);
            const uint64_t sectorInChunk = inStripe % o.chunkSectors;
            const uint64_t memberSector = stripe * o.chunkSectors + sectorInChunk;
            if (cfg.level == RaidLevel::Raid10) {
                put(dd * 2, memberSector, src);
                put(dd * 2 + 1, memberSector, src);
            } else if (cfg.level == RaidLevel::Raid0) {
                put(dd, memberSector, src);
            } else {
                put(o.DataDisk(stripe, dd), memberSector, src);
            }
        }
    }

    // Compute P / Q for every stripe sector from the data members.
    if (o.parityCount > 0) {
        for (uint64_t stripe = 0; stripe < o.stripes; ++stripe) {
            uint32_t p = 0, q = 0; bool hasQ = false;
            o.Parity(stripe, p, q, hasQ);
            for (uint64_t s = 0; s < o.chunkSectors; ++s) {
                const uint64_t memberSector = stripe * o.chunkSectors + s;
                uint8_t pv[SECTOR] = {};
                uint8_t qv[SECTOR] = {};
                for (uint32_t dd = 0; dd < o.dataDisks; ++dd) {
                    const uint32_t disk = o.DataDisk(stripe, dd);
                    const uint8_t* data = get(disk, memberSector);
                    const uint8_t coef = GfPowSlow(dd);
                    for (uint32_t i = 0; i < SECTOR; ++i) {
                        pv[i] = static_cast<uint8_t>(pv[i] ^ data[i]);
                        qv[i] = static_cast<uint8_t>(qv[i] ^ GfMulSlow(coef, data[i]));
                    }
                }
                put(p, memberSector, pv);
                if (hasQ) put(q, memberSector, qv);
            }
        }
    }
    return f;
}

std::vector<const IVirtualDiskReader*> MemberPointers(
    const ArrayFixture& f, const std::vector<uint32_t>& missing)
{
    std::vector<const IVirtualDiskReader*> ptrs;
    ptrs.reserve(f.readers.size());
    for (size_t i = 0; i < f.readers.size(); ++i) {
        bool absent = false;
        for (uint32_t m : missing) if (m == i) absent = true;
        ptrs.push_back(absent ? nullptr : f.readers[i].get());
    }
    return ptrs;
}

int VerifyRoundTrip(const RaidConfig& base, const std::vector<uint32_t>& missing,
                    const std::string& tag)
{
    RaidConfig cfg = base;
    cfg.missing = missing;
    ArrayFixture f = BuildArray(cfg, 0x51 + static_cast<uint32_t>(missing.size()));

    VirtualRaid raid;
    const RaidOpenResult r = raid.Open(cfg, MemberPointers(f, missing));
    if (r != RaidOpenResult::Ok) {
        std::cerr << "FAIL [" << tag << "] open: " << RaidOpenDetail(r) << "\n";
        return 1;
    }
    REQUIRE(raid.Size() == f.volume.size());
    REQUIRE(raid.MissingCount() == missing.size());
    CHECK(raid.IsDegraded() == !missing.empty());

    std::vector<uint8_t> got(f.volume.size(), 0);
    REQUIRE(raid.Read(0, got.data(), got.size()));
    if (got != f.volume) {
        size_t at = 0;
        while (at < got.size() && got[at] == f.volume[at]) ++at;
        std::cerr << "FAIL [" << tag << "] mismatch at byte " << at
                  << " (sector " << at / SECTOR << ")\n";
        return 1;
    }

    // Unaligned reads must stitch sectors correctly.
    if (f.volume.size() > 1400) {
        uint8_t part[777];
        REQUIRE(raid.Read(137, part, sizeof(part)));
        CHECK(std::memcmp(part, f.volume.data() + 137, sizeof(part)) == 0);
        uint8_t tail[33];
        REQUIRE(raid.Read(f.volume.size() - 33, tail, sizeof(tail)));
        CHECK(std::memcmp(tail, f.volume.data() + f.volume.size() - 33, 33) == 0);
    }
    // Strict out-of-range.
    uint8_t one[2];
    CHECK(!raid.Read(f.volume.size() - 1, one, 2));
    CHECK(!raid.Read(f.volume.size(), one, 1));
    CHECK(raid.Read(f.volume.size(), one, 0));   // zero-length is a no-op

    return 0;
}

int TestAllLevels()
{
    const RaidLayout all5[] = {
        RaidLayout::LeftSymmetric, RaidLayout::LeftAsymmetric,
        RaidLayout::RightSymmetric, RaidLayout::RightAsymmetric,
        RaidLayout::ParityFirst, RaidLayout::ParityLast
    };
    const RaidLayout all6[] = {
        RaidLayout::LeftSymmetric, RaidLayout::RightSymmetric,
        RaidLayout::ParityFirst, RaidLayout::ParityLast
    };

    // JBOD / RAID0 / RAID1 / RAID10
    {
        RaidConfig c;
        c.level = RaidLevel::Jbod; c.members = 3;
        c.chunkBytes = 1024; c.memberBytes = 4 * 1024;
        REQUIRE(VerifyRoundTrip(c, {}, "jbod") == 0);
    }
    {
        RaidConfig c;
        c.level = RaidLevel::Raid0; c.members = 3;
        c.chunkBytes = 1024; c.memberBytes = 4 * 1024;
        REQUIRE(VerifyRoundTrip(c, {}, "raid0") == 0);
    }
    {
        RaidConfig c;
        c.level = RaidLevel::Raid1; c.members = 3;
        c.chunkBytes = 1024; c.memberBytes = 4 * 1024;
        REQUIRE(VerifyRoundTrip(c, {}, "raid1-full") == 0);
        REQUIRE(VerifyRoundTrip(c, {0}, "raid1-miss0") == 0);
        REQUIRE(VerifyRoundTrip(c, {0, 2}, "raid1-miss0+2") == 0);
    }
    {
        RaidConfig c;
        c.level = RaidLevel::Raid10; c.members = 6;
        c.chunkBytes = 1024; c.memberBytes = 4 * 1024;
        REQUIRE(VerifyRoundTrip(c, {}, "raid10-full") == 0);
        REQUIRE(VerifyRoundTrip(c, {0}, "raid10-missA") == 0);
        REQUIRE(VerifyRoundTrip(c, {3}, "raid10-missB") == 0);
        REQUIRE(VerifyRoundTrip(c, {0, 3, 4}, "raid10-miss3") == 0);
    }

    // RAID4 with a non-zero data offset (superblock-like offset).
    for (RaidLayout layout : {RaidLayout::ParityFirst, RaidLayout::ParityLast}) {
        RaidConfig c;
        c.level = RaidLevel::Raid4; c.layout = layout; c.members = 4;
        c.chunkBytes = 1024; c.memberBytes = 6 * 1024; c.dataOffset = 4096;
        REQUIRE(VerifyRoundTrip(c, {}, "raid4-full") == 0);
        for (uint32_t m = 0; m < 4; ++m) {
            REQUIRE(VerifyRoundTrip(c, {m}, "raid4-miss" + std::to_string(m)) == 0);
        }
    }

    // RAID5: every layout, every single-member failure.
    for (RaidLayout layout : all5) {
        RaidConfig c;
        c.level = RaidLevel::Raid5; c.layout = layout; c.members = 4;
        c.chunkBytes = 1024; c.memberBytes = 8 * 1024;
        const std::string name = RaidLayoutName(layout);
        REQUIRE(VerifyRoundTrip(c, {}, "raid5-" + name + "-full") == 0);
        for (uint32_t m = 0; m < 4; ++m) {
            REQUIRE(VerifyRoundTrip(c, {m},
                    "raid5-" + name + "-miss" + std::to_string(m)) == 0);
        }
    }

    // RAID6: full, single failure, and all two-failure shape classes.
    for (RaidLayout layout : all6) {
        RaidConfig c;
        c.level = RaidLevel::Raid6; c.layout = layout; c.members = 5;
        c.chunkBytes = 1024; c.memberBytes = 8 * 1024;
        const std::string name = RaidLayoutName(layout);
        REQUIRE(VerifyRoundTrip(c, {}, "raid6-" + name + "-full") == 0);
        for (uint32_t m = 0; m < 5; ++m) {
            REQUIRE(VerifyRoundTrip(c, {m},
                    "raid6-" + name + "-miss" + std::to_string(m)) == 0);
        }
        REQUIRE(VerifyRoundTrip(c, {0, 1}, "raid6-" + name + "-2data") == 0);
        REQUIRE(VerifyRoundTrip(c, {1, 4}, "raid6-" + name + "-2data-b") == 0);
        REQUIRE(VerifyRoundTrip(c, {2, 4}, "raid6-" + name + "-P+Q") == 0);
        REQUIRE(VerifyRoundTrip(c, {0, 2}, "raid6-" + name + "-data+parity") == 0);
    }
    return 0;
}

// Simulates an unreadable sector on a member that is otherwise present.
class BadSectorReader : public IVirtualDiskReader
{
public:
    BadSectorReader(const IVirtualDiskReader* inner, uint64_t sector)
        : m_inner(inner), m_sector(sector) {}
    uint64_t Size() const override { return m_inner->Size(); }
    bool Read(uint64_t offset, void* dst, size_t length) const override
    {
        if (length == 0) return true;
        const uint64_t first = offset / SECTOR;
        const uint64_t last  = (offset + length - 1) / SECTOR;
        if (first <= m_sector && m_sector <= last) return false;
        return m_inner->Read(offset, dst, length);
    }
private:
    const IVirtualDiskReader* m_inner;
    uint64_t m_sector;
};

int TestReadErrorFallback()
{
    RaidConfig c;
    c.level = RaidLevel::Raid5; c.layout = RaidLayout::LeftSymmetric;
    c.members = 4; c.chunkBytes = 1024; c.memberBytes = 8 * 1024;

    // RAID5: an unreadable target chunk is rebuilt from P.
    ArrayFixture f5 = BuildArray(c, 0xE1);
    BadSectorReader bad(f5.readers[0].get(), 0);
    VirtualRaid r5;
    REQUIRE(r5.Open(c, {&bad, f5.readers[1].get(),
                        f5.readers[2].get(), f5.readers[3].get()}) ==
            RaidOpenResult::Ok);
    std::vector<uint8_t> got(f5.volume.size());
    REQUIRE(r5.Read(0, got.data(), got.size()));
    CHECK(got == f5.volume);

    // RAID5 already degraded + a second unreadable chunk -> failure, never
    // silently fabricated bytes.
    RaidConfig d5 = c; d5.missing = {1};
    ArrayFixture g5 = BuildArray(d5, 0xE1);
    BadSectorReader bad2(g5.readers[0].get(), 0);
    VirtualRaid q5;
    REQUIRE(q5.Open(d5, {&bad2, nullptr, g5.readers[2].get(),
                         g5.readers[3].get()}) == RaidOpenResult::Ok);
    uint8_t sec[SECTOR];
    CHECK(!q5.Read(0, sec, SECTOR));
    CHECK(q5.Read(SECTOR, sec, SECTOR));   // unaffected neighbour still reads

    // RAID6 degraded + one unreadable chunk -> two-unknown P/Q solve.
    RaidConfig d6 = c;
    d6.level = RaidLevel::Raid6; d6.members = 5; d6.missing = {1};
    ArrayFixture g6 = BuildArray(d6, 0xF2);
    BadSectorReader bad3(g6.readers[0].get(), 0);
    VirtualRaid q6;
    REQUIRE(q6.Open(d6, {&bad3, nullptr, g6.readers[2].get(),
                         g6.readers[3].get(), g6.readers[4].get()}) ==
            RaidOpenResult::Ok);
    std::vector<uint8_t> got6(g6.volume.size());
    REQUIRE(q6.Read(0, got6.data(), got6.size()));
    CHECK(got6 == g6.volume);

    // RAID0 has no redundancy: a read error must propagate.
    RaidConfig c0 = c; c0.level = RaidLevel::Raid0;
    ArrayFixture f0 = BuildArray(c0, 0xE3);
    BadSectorReader bad4(f0.readers[0].get(), 0);
    VirtualRaid r0;
    REQUIRE(r0.Open(c0, {&bad4, f0.readers[1].get(),
                         f0.readers[2].get(), f0.readers[3].get()}) ==
            RaidOpenResult::Ok);
    CHECK(!r0.Read(0, sec, SECTOR));
    return 0;
}

int TestParityVerification()
{
    RaidConfig c;
    c.level = RaidLevel::Raid6; c.layout = RaidLayout::LeftSymmetric;
    c.members = 5; c.chunkBytes = 1024; c.memberBytes = 8 * 1024;
    ArrayFixture f = BuildArray(c, 0x77);
    VirtualRaid raid;
    REQUIRE(raid.Open(c, MemberPointers(f, {})) == RaidOpenResult::Ok);
    RaidVerifyStats st;
    CHECK(raid.VerifyStripe(0, 2, &st) == RaidVerifyResult::Ok);
    CHECK(st.sectorsChecked == 2);
    CHECK(st.mismatchP == 0 && st.mismatchQ == 0);

    // Corrupt one P byte and one Q byte, each must be detected separately.
    ArrayFixture fp = BuildArray(c, 0x77);
    VirtualRaid rp;
    REQUIRE(rp.Open(c, MemberPointers(fp, {})) == RaidOpenResult::Ok);
    {
        ForwardOracle o(c);
        uint32_t p = 0, q = 0; bool hasQ = false;
        o.Parity(0, p, q, hasQ);
        fp.readers[p]->Bytes()[0] ^= 0x40;
    }
    CHECK(rp.VerifyStripe(0, 1) == RaidVerifyResult::MismatchP);

    ArrayFixture fq = BuildArray(c, 0x77);
    VirtualRaid rq;
    REQUIRE(rq.Open(c, MemberPointers(fq, {})) == RaidOpenResult::Ok);
    {
        ForwardOracle o(c);
        uint32_t p = 0, q = 0; bool hasQ = false;
        o.Parity(0, p, q, hasQ);
        fq.readers[q]->Bytes()[0] ^= 0x40;
    }
    CHECK(rq.VerifyStripe(0, 1) == RaidVerifyResult::MismatchQ);

    // Degraded array -> partial, not a false "ok".
    RaidConfig degraded = c;
    degraded.missing = {3};
    VirtualRaid rd;
    REQUIRE(rd.Open(degraded, MemberPointers(f, {3})) == RaidOpenResult::Ok);
    CHECK(rd.VerifyStripe(0, 1) == RaidVerifyResult::Partial);
    CHECK(rd.VerifyStripe(99999, 1) == RaidVerifyResult::BadStripe);

    // No-parity levels report not-applicable.
    RaidConfig c0 = c;
    c0.level = RaidLevel::Raid0;
    ArrayFixture f0 = BuildArray(c0, 0x78);
    VirtualRaid r0;
    REQUIRE(r0.Open(c0, MemberPointers(f0, {})) == RaidOpenResult::Ok);
    CHECK(r0.VerifyStripe(0, 1) == RaidVerifyResult::NotApplicable);
    return 0;
}

int TestNegativeGeometry()
{
    RaidConfig c;
    c.level = RaidLevel::Raid5; c.layout = RaidLayout::LeftSymmetric;
    c.members = 4; c.chunkBytes = 1024; c.memberBytes = 8 * 1024;

    // A wrong layout must NOT reproduce the same volume (guards against a
    // test that would pass no matter what mapping is used).
    ArrayFixture f = BuildArray(c, 0x99);
    RaidConfig wrong = c;
    wrong.layout = RaidLayout::RightSymmetric;
    VirtualRaid bad;
    REQUIRE(bad.Open(wrong, MemberPointers(f, {})) == RaidOpenResult::Ok);
    std::vector<uint8_t> got(f.volume.size());
    REQUIRE(bad.Read(0, got.data(), got.size()));
    CHECK(got != f.volume);

    // A wrong chunk size must also fail to reproduce the volume.
    ArrayFixture f2 = BuildArray(c, 0x99);
    RaidConfig wrongChunk = c;
    wrongChunk.chunkBytes = 2048;
    wrongChunk.memberBytes = 8 * 1024;
    VirtualRaid bad2;
    REQUIRE(bad2.Open(wrongChunk, MemberPointers(f2, {})) == RaidOpenResult::Ok);
    std::vector<uint8_t> got2(f2.volume.size());
    REQUIRE(bad2.Read(0, got2.data(), got2.size()));
    CHECK(got2 != f2.volume);
    return 0;
}

int TestRejections()
{
    RaidConfig c;
    c.level = RaidLevel::Raid5; c.layout = RaidLayout::LeftSymmetric;
    c.members = 4; c.chunkBytes = 1024; c.memberBytes = 8 * 1024;
    ArrayFixture f = BuildArray(c, 1);
    VirtualRaid raid;

    CHECK(raid.Open(c, {}) == RaidOpenResult::EmptyMembers);

    RaidConfig few = c; few.members = 2;
    CHECK(raid.Open(few, {f.readers[0].get(), f.readers[1].get()}) ==
          RaidOpenResult::TooFewMembers);

    RaidConfig badChunk = c; badChunk.chunkBytes = 700;
    CHECK(raid.Open(badChunk, MemberPointers(f, {})) == RaidOpenResult::BadChunk);

    RaidConfig badOffset = c; badOffset.dataOffset = 100;
    CHECK(raid.Open(badOffset, MemberPointers(f, {})) == RaidOpenResult::BadDataOffset);

    RaidConfig badAlign = c; badAlign.memberBytes = 8 * 1024 + 1;
    CHECK(raid.Open(badAlign, MemberPointers(f, {})) == RaidOpenResult::SizeMismatch);

    RaidConfig tooSmall = c; tooSmall.memberBytes = 64 * 1024;
    CHECK(raid.Open(tooSmall, MemberPointers(f, {})) == RaidOpenResult::BadMemberSize);

    CHECK(raid.Open(c, {nullptr, f.readers[1].get(),
                        f.readers[2].get(), f.readers[3].get()}) ==
          RaidOpenResult::MissingReader);

    RaidConfig r6 = c;
    r6.level = RaidLevel::Raid6; r6.members = 4;
    r6.layout = RaidLayout::LeftAsymmetric;
    CHECK(raid.Open(r6, MemberPointers(f, {})) == RaidOpenResult::UnsupportedLayout);

    RaidConfig r4 = c;
    r4.level = RaidLevel::Raid4; r4.layout = RaidLayout::LeftSymmetric;
    CHECK(raid.Open(r4, MemberPointers(f, {})) == RaidOpenResult::UnsupportedLayout);

    RaidConfig tooManyMissing = c;
    tooManyMissing.missing = {0, 1};
    CHECK(raid.Open(tooManyMissing, MemberPointers(f, {0, 1})) ==
          RaidOpenResult::TooManyMissing);

    RaidConfig r10 = c;
    r10.level = RaidLevel::Raid10;
    r10.missing = {0, 1};
    CHECK(raid.Open(r10, MemberPointers(f, {0, 1})) ==
          RaidOpenResult::TooManyMissing);

    // Not open -> no reads, zero size.
    raid.Close();
    CHECK(!raid.IsOpen());
    CHECK(raid.Size() == 0);
    uint8_t b[SECTOR];
    CHECK(!raid.Read(0, b, SECTOR));

    // No-redundancy levels refuse to fabricate data for a missing member.
    RaidConfig r0 = c; r0.level = RaidLevel::Raid0;
    // (Open itself rejects the degraded config.)
    r0.missing = {1};
    CHECK(raid.Open(r0, MemberPointers(f, {1})) ==
          RaidOpenResult::TooManyMissing);
    return 0;
}

int TestNestedRaid50And60()
{
    // Two inner RAID5 sets, exposed to an outer RAID0 as IVirtualDiskReader.
    RaidConfig in;
    in.level = RaidLevel::Raid5; in.layout = RaidLayout::LeftSymmetric;
    in.members = 3; in.chunkBytes = 1024; in.memberBytes = 4 * 1024;
    ArrayFixture a = BuildArray(in, 0xA1);
    ArrayFixture b = BuildArray(in, 0xB2);
    VirtualRaid innerA, innerB;
    REQUIRE(innerA.Open(in, MemberPointers(a, {})) == RaidOpenResult::Ok);
    REQUIRE(innerB.Open(in, MemberPointers(b, {})) == RaidOpenResult::Ok);

    RaidConfig out;
    out.level = RaidLevel::Raid0; out.members = 2;
    out.chunkBytes = static_cast<uint32_t>(innerA.Size());
    out.memberBytes = innerA.Size();
    VirtualRaid outer;
    REQUIRE(outer.Open(out, {&innerA, &innerB}) == RaidOpenResult::Ok);

    std::vector<uint8_t> expected = a.volume;
    expected.insert(expected.end(), b.volume.begin(), b.volume.end());
    REQUIRE(outer.Size() == expected.size());
    std::vector<uint8_t> got(expected.size());
    REQUIRE(outer.Read(0, got.data(), got.size()));
    CHECK(got == expected);

    // A degraded inner set still serves the outer array.
    VirtualRaid innerDegraded;
    RaidConfig inDeg = in; inDeg.missing = {1};
    REQUIRE(innerDegraded.Open(inDeg, MemberPointers(a, {1})) ==
            RaidOpenResult::Ok);
    VirtualRaid outer2;
    REQUIRE(outer2.Open(out, {&innerDegraded, &innerB}) == RaidOpenResult::Ok);
    std::vector<uint8_t> got2(expected.size());
    REQUIRE(outer2.Read(0, got2.data(), got2.size()));
    CHECK(got2 == expected);

    // RAID60: inner RAID6 sets.
    RaidConfig in6 = in;
    in6.level = RaidLevel::Raid6; in6.members = 4;
    in6.layout = RaidLayout::LeftSymmetric;
    ArrayFixture c6 = BuildArray(in6, 0xC3);
    ArrayFixture d6 = BuildArray(in6, 0xD4);
    VirtualRaid innerC, innerD;
    RaidConfig in6deg = in6; in6deg.missing = {0, 2};
    REQUIRE(innerC.Open(in6deg, MemberPointers(c6, {0, 2})) == RaidOpenResult::Ok);
    REQUIRE(innerD.Open(in6, MemberPointers(d6, {})) == RaidOpenResult::Ok);
    RaidConfig out6 = out;
    out6.chunkBytes = static_cast<uint32_t>(innerC.Size());
    out6.memberBytes = innerC.Size();
    VirtualRaid outer6;
    REQUIRE(outer6.Open(out6, {&innerC, &innerD}) == RaidOpenResult::Ok);
    std::vector<uint8_t> exp6 = c6.volume;
    exp6.insert(exp6.end(), d6.volume.begin(), d6.volume.end());
    std::vector<uint8_t> got6(exp6.size());
    REQUIRE(outer6.Size() == exp6.size());
    REQUIRE(outer6.Read(0, got6.data(), got6.size()));
    CHECK(got6 == exp6);
    return 0;
}

int TestGfTablesCrossCheck()
{
    // Independent shift-and-reduce vs the product's exp/log tables.
    for (uint32_t a = 0; a < 256; ++a) {
        for (uint32_t b = 0; b < 256; ++b) {
            const uint8_t want = GfMulSlow(static_cast<uint8_t>(a),
                                           static_cast<uint8_t>(b));
            const uint8_t got = raid_detail::GfMul(static_cast<uint8_t>(a),
                                                   static_cast<uint8_t>(b));
            if (want != got) {
                std::cerr << "FAIL gf mul " << a << "*" << b << "\n";
                return 1;
            }
        }
    }
    for (uint32_t k = 0; k < 32; ++k) {
        if (GfPowSlow(k) != raid_detail::GfPow(k)) {
            std::cerr << "FAIL gf pow " << k << "\n";
            return 1;
        }
    }
    for (uint32_t a = 1; a < 256; ++a) {
        for (uint32_t b = 1; b < 256; ++b) {
            const uint8_t prod = GfMulSlow(static_cast<uint8_t>(a),
                                           static_cast<uint8_t>(b));
            const uint8_t back = raid_detail::GfDiv(
                prod, static_cast<uint8_t>(b));
            if (back != static_cast<uint8_t>(a)) {
                std::cerr << "FAIL gf div " << a << "," << b << "\n";
                return 1;
            }
        }
    }
    return 0;
}

} // namespace

int main()
{
    std::cout << "A1-4a VirtualRaid: independent forward-oracle cross-check\n";
    int rc = 0;
    rc |= TestGfTablesCrossCheck();
    rc |= TestAllLevels();
    rc |= TestReadErrorFallback();
    rc |= TestParityVerification();
    rc |= TestNegativeGeometry();
    rc |= TestRejections();
    rc |= TestNestedRaid50And60();
    if (rc == 0 && g_fail == 0) {
        std::cout << "ALL PASS: GF tables, 7 levels, 10 layouts, degraded "
                     "rebuild (incl. RAID6 dual), bad-sector fallback, parity "
                     "verify, rejection paths, nested RAID50/60\n";
        return 0;
    }
    std::cerr << "FAILURES: " << g_fail << " (rc=" << rc << ")\n";
    return 1;
}
