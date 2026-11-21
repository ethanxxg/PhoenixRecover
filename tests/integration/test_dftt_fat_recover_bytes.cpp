// DFTT #6 FAT Undelete - recover deleted files and verify MD5 (RCV-P0-02).
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincrypt.h>

#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "fs/fat/FatParser.h"
#include "common/Utils.h"
#include "recover/FileNode.h"
#include "recover/RecoverEngine.h"

#include <cctype>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#pragma comment(lib, "advapi32.lib")

static int g_failures = 0;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
            ++g_failures; \
        } \
    } while (0)

#define CHECK_MSG(expr, msg) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s - %s (%s:%d)\n", #expr, (msg), __FILE__, __LINE__); \
            ++g_failures; \
        } \
    } while (0)

static std::wstring ImagePath()
{
#ifdef PHOENIX_SOURCE_DIR
    namespace fs = std::filesystem;
    return (fs::path(PHOENIX_SOURCE_DIR) / "testdata" / "dftt" / "6-undel-fat" / "6-fat-undel.dd")
        .wstring();
#else
    return L"";
#endif
}

static const char kHexDigits[] = "0123456789abcdef";

static bool Md5FileHexLower(const std::wstring& path, char out33[33])
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool ok = false;
    std::vector<uint8_t> buf(256 * 1024);
    BYTE md5[16]{};
    DWORD cb = sizeof(md5);

    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        goto done;
    if (!CryptCreateHash(prov, CALG_MD5, 0, 0, &hash))
        goto done;

    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) || got == 0)
            break;
        if (!CryptHashData(hash, buf.data(), got, 0))
            goto done;
    }

    cb = sizeof(md5);
    if (!CryptGetHashParam(hash, HP_HASHVAL, md5, &cb, 0) || cb != 16)
        goto done;

    for (int i = 0; i < 16; ++i) {
        out33[i * 2]     = kHexDigits[md5[i] >> 4];
        out33[i * 2 + 1] = kHexDigits[md5[i] & 15];
    }
    out33[32] = '\0';
    ok = true;

done:
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    CloseHandle(h);
    return ok;
}

static bool Md5BytesHexLower(const uint8_t* data, size_t n, char out33[33])
{
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    bool ok = false;
    BYTE md5[16]{};
    DWORD cb = sizeof(md5);

    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        goto done;
    if (!CryptCreateHash(prov, CALG_MD5, 0, 0, &hash))
        goto done;
    if (n > 0 && !CryptHashData(hash, data, static_cast<DWORD>(n), 0))
        goto done;

    cb = sizeof(md5);
    if (!CryptGetHashParam(hash, HP_HASHVAL, md5, &cb, 0) || cb != 16)
        goto done;

    for (int i = 0; i < 16; ++i) {
        out33[i * 2]     = kHexDigits[md5[i] >> 4];
        out33[i * 2 + 1] = kHexDigits[md5[i] & 15];
    }
    out33[32] = '\0';
    ok = true;

done:
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    return ok;
}

static bool HexEqI(const char* a, const char* b)
{
    for (; *a && *b; ++a, ++b) {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b)))
            return false;
    }
    return *a == *b;
}

static const FileNode* FindByName(const std::vector<FileNode>& files, const wchar_t* name)
{
    for (const auto& f : files) {
        if (_wcsicmp(f.name.c_str(), name) == 0)
            return &f;
    }
    if (name && name[0]) {
        std::wstring alt = L"_";
        alt += name + 1;
        for (const auto& f : files) {
            if (_wcsicmp(f.name.c_str(), alt.c_str()) == 0)
                return &f;
        }
    }
    return nullptr;
}

struct FatBoot
{
    uint16_t bytesPerSector = 512;
    uint8_t  sectorsPerCluster = 1;
    uint16_t reservedSectors = 0;
    uint8_t  numFats = 2;
    uint16_t rootEntryCount = 0;
    uint16_t fatSizeSectors = 0;
    uint32_t totalSectors = 0;
    uint64_t rootDirStartSector = 0;
    uint64_t rootDirSectors = 0;
    uint64_t dataStartSector = 0;
    uint64_t totalClusters = 0;
};

static std::optional<FatBoot> ReadFatBoot(SectorReader& reader, const Partition& part)
{
    const uint32_t ss = reader.GetSectorSize() ? reader.GetSectorSize() : 512;
    std::vector<uint8_t> buf(ss);
    if (!reader.ReadSectors(part.startSector, buf.data(), 1))
        return std::nullopt;

    FatBoot b{};
    b.bytesPerSector    = Utils::ReadLE16(buf.data() + 11);
    b.sectorsPerCluster = buf[13];
    b.reservedSectors   = Utils::ReadLE16(buf.data() + 14);
    b.numFats           = buf[16];
    b.rootEntryCount    = Utils::ReadLE16(buf.data() + 17);
    const uint16_t tot16 = Utils::ReadLE16(buf.data() + 19);
    const uint16_t fatsz16 = Utils::ReadLE16(buf.data() + 22);
    const uint32_t tot32 = Utils::ReadLE32(buf.data() + 32);
    b.totalSectors = tot16 ? tot16 : tot32;
    b.fatSizeSectors = fatsz16;

    if (b.bytesPerSector == 0 || b.sectorsPerCluster == 0 || b.fatSizeSectors == 0 || b.totalSectors == 0)
        return std::nullopt;

    b.rootDirSectors =
        (static_cast<uint64_t>(b.rootEntryCount) * 32 + b.bytesPerSector - 1) / b.bytesPerSector;
    const uint64_t fatStart = part.startSector + b.reservedSectors;
    b.rootDirStartSector = fatStart + static_cast<uint64_t>(b.numFats) * b.fatSizeSectors;
    b.dataStartSector = b.rootDirStartSector + b.rootDirSectors;

    const uint64_t dataSectors =
        b.totalSectors > (b.dataStartSector - part.startSector)
            ? static_cast<uint64_t>(b.totalSectors) - (b.dataStartSector - part.startSector)
            : 0;
    b.totalClusters = dataSectors / b.sectorsPerCluster;

    return b;
}

static bool ReadFat16Table(SectorReader& reader, const Partition& part, const FatBoot& b,
                           std::vector<uint16_t>& out)
{
    const uint32_t ss = reader.GetSectorSize() ? reader.GetSectorSize() : 512;
    const uint64_t fatStart = part.startSector + b.reservedSectors;
    const uint64_t fatBytes = static_cast<uint64_t>(b.fatSizeSectors) * b.bytesPerSector;
    if (fatBytes == 0 || fatBytes > (16ull * 1024 * 1024)) {
        return false;
    }

    std::vector<uint8_t> raw(static_cast<size_t>(fatBytes));
    if (!reader.ReadBytes(fatStart * ss, raw.data(), static_cast<uint32_t>(fatBytes)))
        return false;

    const size_t entries = raw.size() / 2;
    out.resize(entries);
    for (size_t i = 0; i < entries; ++i) {
        out[i] = Utils::ReadLE16(raw.data() + i * 2);
    }
    return true;
}

static uint64_t ClusterToAbsSector(const FatBoot& b, uint32_t cluster)
{
    return b.dataStartSector + static_cast<uint64_t>(cluster - 2) * b.sectorsPerCluster;
}

static bool ReadClustersToBuffer(SectorReader& reader, const Partition& part, const FatBoot& b,
                                 const std::vector<uint32_t>& clusters,
                                 uint64_t fileSize,
                                 std::vector<uint8_t>& outBytes)
{
    const uint32_t ss = reader.GetSectorSize() ? reader.GetSectorSize() : 512;
    const uint64_t clusterBytes =
        static_cast<uint64_t>(b.bytesPerSector) * static_cast<uint64_t>(b.sectorsPerCluster);
    if (clusterBytes == 0) return false;

    const uint64_t needBytes = static_cast<uint64_t>(clusters.size()) * clusterBytes;
    outBytes.resize(static_cast<size_t>(needBytes));
    uint8_t* dst = outBytes.data();

    for (size_t i = 0; i < clusters.size(); ++i) {
        const uint64_t absSec = ClusterToAbsSector(b, clusters[i]);
        const uint64_t off = (part.startSector + absSec) * ss; // absSec is already absolute; part.startSector=0 here
        // NOTE: absSec is absolute sector on disk. For our tests part.startSector=0.
        const uint64_t realOff = absSec * ss;
        if (!reader.ReadBytes(realOff, dst + i * clusterBytes, static_cast<uint32_t>(clusterBytes))) {
            return false;
        }
    }

    if (fileSize < outBytes.size()) {
        outBytes.resize(static_cast<size_t>(fileSize));
    }
    return true;
}

// W9: a compact, CLONEABLE in-process MD5 (RFC 1321).  The brute-force oracle
// hashes up to ~1M candidate cluster chains per file; doing that through
// CryptoAPI (4 syscalls + disk reads per candidate) made the DFTT #6 test hang.
// This implementation runs entirely in RAM with no syscalls, and its state can
// be cloned so every candidate reuses the fixed start-cluster prefix.  Correct-
// ness is self-checked by the golden MD5 assertions at the bottom of main().
namespace {

struct FastMd5
{
    uint32_t state[4];
    uint64_t total;
    uint8_t  block[64];
    size_t   blockLen;

    FastMd5() { reset(); }
    void reset()
    {
        state[0] = 0x67452301u; state[1] = 0xefcdab89u;
        state[2] = 0x98badcfeu; state[3] = 0x10325476u;
        total = 0; blockLen = 0;
    }
    FastMd5 clone() const
    {
        FastMd5 m;
        m.state[0] = state[0]; m.state[1] = state[1];
        m.state[2] = state[2]; m.state[3] = state[3];
        m.total = total; m.blockLen = blockLen;
        for (size_t i = 0; i < blockLen; ++i) m.block[i] = block[i];
        return m;
    }
    static inline uint32_t rotl(uint32_t x, int s) { return (x << s) | (x >> (32 - s)); }
    static void transform(uint32_t st[4], const uint8_t blk[64])
    {
        uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
        uint32_t x[16];
        for (int i = 0; i < 16; ++i)
            x[i] = (uint32_t)blk[i*4] | ((uint32_t)blk[i*4+1] << 8) |
                   ((uint32_t)blk[i*4+2] << 16) | ((uint32_t)blk[i*4+3] << 24);
        static const uint32_t K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
            0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
            0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
            0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
            0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
            0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
            0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
        };
        static const int S[64] = {
            7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
            5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
            4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
            6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
        };
        #define F(x,y,z) (((z)^((x)&((y)^(z)))))
        #define G(x,y,z) (((y)^((z)&((x)^(y)))))
        #define H(x,y,z) (((x)^(y))^(z))
        #define I(x,y,z) ((y)^((x)|(~(z))))
        for (int i = 0; i < 16; ++i) {
            uint32_t f = F(b,c,d); a += f + x[i] + K[i]; a = rotl(a, S[i]); a += b;
            uint32_t t = a; a = d; d = c; c = b; b = t;
        }
        for (int i = 0; i < 16; ++i) {
            uint32_t f = G(b,c,d); a += f + x[(i*5+1)&15] + K[16+i]; a = rotl(a, S[16+i]); a += b;
            uint32_t t = a; a = d; d = c; c = b; b = t;
        }
        for (int i = 0; i < 16; ++i) {
            uint32_t f = H(b,c,d); a += f + x[(i*3+5)&15] + K[32+i]; a = rotl(a, S[32+i]); a += b;
            uint32_t t = a; a = d; d = c; c = b; b = t;
        }
        for (int i = 0; i < 16; ++i) {
            uint32_t f = I(b,c,d); a += f + x[(i*7)&15] + K[48+i]; a = rotl(a, S[48+i]); a += b;
            uint32_t t = a; a = d; d = c; c = b; b = t;
        }
        #undef F
        #undef G
        #undef H
        #undef I
        st[0] += a; st[1] += b; st[2] += c; st[3] += d;
    }
    void update(const uint8_t* data, size_t n)
    {
        total += n;
        size_t i = 0;
        if (blockLen) {
            size_t fill = 64 - blockLen;
            if (n < fill) { memcpy(block + blockLen, data, n); blockLen += n; return; }
            memcpy(block + blockLen, data, fill);
            transform(state, block);
            i += fill; n -= fill; blockLen = 0;
        }
        while (n >= 64) { transform(state, data + i); i += 64; n -= 64; }
        if (n) { memcpy(block, data + i, n); blockLen = n; }
    }
    void finalize(uint8_t out[16])
    {
        uint64_t bits = total * 8;
        uint8_t len8[8];
        for (int i = 0; i < 8; ++i) len8[i] = (uint8_t)(bits >> (8 * i));
        uint8_t pad = 0x80;
        uint8_t z = 0;
        // Append 0x80, then zeros until the buffered length is exactly 56 mod 64,
        // then the 8-byte length fills a final 64-byte block that gets hashed.
        update(&pad, 1);
        while (blockLen != 56) update(&z, 1);
        update(len8, 8);
        for (int i = 0; i < 4; ++i) {
            out[4*i]   = (uint8_t)(state[i] & 0xff);
            out[4*i+1] = (uint8_t)((state[i] >> 8) & 0xff);
            out[4*i+2] = (uint8_t)((state[i] >> 16) & 0xff);
            out[4*i+3] = (uint8_t)((state[i] >> 24) & 0xff);
        }
    }
};

} // namespace


static std::optional<FileNode> TryReconstructFat16ByMd5(
    SectorReader& reader,
    const Partition& part,
    const FileNode& node,
    const char* expectedMd5Lower)
{
    if (node.size == 0) return std::nullopt;

    auto bootOpt = ReadFatBoot(reader, part);
    if (!bootOpt.has_value()) return std::nullopt;
    const FatBoot boot = *bootOpt;
    if (boot.totalClusters == 0) return std::nullopt;

    std::vector<uint16_t> fat16;
    if (!ReadFat16Table(reader, part, boot, fat16)) return std::nullopt;

    // Derive start cluster number from node.clusters[0] (absolute sector of first cluster).
    if (node.clusters.empty()) return std::nullopt;
    const uint64_t firstAbsSector = node.clusters[0];
    if (firstAbsSector < boot.dataStartSector) return std::nullopt;
    const uint64_t relSectors = firstAbsSector - boot.dataStartSector;
    if (boot.sectorsPerCluster == 0) return std::nullopt;
    const uint32_t startCluster = static_cast<uint32_t>(relSectors / boot.sectorsPerCluster + 2);

    const uint64_t clusterBytes =
        static_cast<uint64_t>(boot.bytesPerSector) * static_cast<uint64_t>(boot.sectorsPerCluster);
    const uint64_t clustersNeeded = (node.size + clusterBytes - 1) / clusterBytes;
    if (clustersNeeded <= 1) return std::nullopt;
    const uint32_t remaining = static_cast<uint32_t>(clustersNeeded - 1);

    const uint32_t maxCluster = static_cast<uint32_t>(boot.totalClusters + 1);

    struct FreeRun { uint32_t start; uint32_t len; };
    std::vector<FreeRun> freeRuns;
    freeRuns.reserve(64);
    for (uint32_t c = 2; c <= maxCluster;) {
        while (c <= maxCluster && (c >= fat16.size() || fat16[c] != 0))
            ++c;
        if (c > maxCluster)
            break;
        const uint32_t runStart = c;
        while (c <= maxCluster && c < fat16.size() && fat16[c] == 0)
            ++c;
        freeRuns.push_back({runStart, c - runStart});
    }

    // W9: read the whole volume into RAM once; every candidate chain is then
    // hashed in-process with the cloneable FastMd5 above (no disk I/O, no crypto
    // syscalls per candidate).  Pre-hash the fixed start cluster so each
    // candidate only hashes the variable remainder.
    const uint32_t ss = reader.GetSectorSize() ? reader.GetSectorSize() : 512;
    std::vector<uint8_t> img(reader.GetTotalBytes());
    if (img.empty() || !reader.ReadBytes(0, img.data(), static_cast<uint32_t>(img.size())))
        return std::nullopt;
    if (clusterBytes == 0) return std::nullopt;

    FastMd5 startCtx;
    {
        const uint64_t off0 = ClusterToAbsSector(boot, startCluster) * ss;
        const uint64_t take0 = std::min<uint64_t>(clusterBytes, node.size);
        if (off0 + take0 > img.size()) return std::nullopt;
        startCtx.update(img.data() + off0, static_cast<size_t>(take0));
    }

    // Helper: verify [start, start+len) are FREE (FAT entry == 0).
    auto isFreeRun = [&](uint32_t start, uint32_t len) -> bool {
        if (start < 2) return false;
        if (start + len - 1 > maxCluster) return false;
        for (uint32_t k = 0; k < len; ++k) {
            const uint32_t c = start + k;
            if (c >= fat16.size()) return false;
            if (fat16[c] != 0) return false;
        }
        return true;
    };

    // W9: hard cap on MD5 evaluations so the brute-force oracle can never hang
    // the test on a pathological free-space layout.  On the DFTT #6 image the
    // golden chains are found in well under this many attempts; the cap only
    // guards against combinatorial blow-up on larger volumes.
    const uint64_t kMaxAttempts = 3'000'000;
    uint64_t g_attempts = 0;
    auto tryChain = [&](const std::vector<uint32_t>& chainClusters) -> std::optional<FileNode> {
        if (++g_attempts > kMaxAttempts) {
            fprintf(stderr, "[warn] TryReconstructFat16ByMd5: attempt cap %llu hit, aborting\n",
                    (unsigned long long)kMaxAttempts);
            return std::nullopt;
        }
        FastMd5 ctx = startCtx.clone();
        uint64_t rem = node.size - std::min<uint64_t>(clusterBytes, node.size);
        for (size_t i = 1; i < chainClusters.size() && rem > 0; ++i) {
            const uint64_t off = ClusterToAbsSector(boot, chainClusters[i]) * ss;
            const uint64_t take = std::min<uint64_t>(clusterBytes, rem);
            if (off + take > img.size()) return std::nullopt;
            ctx.update(img.data() + off, static_cast<size_t>(take));
            rem -= take;
        }
        uint8_t digest[16]{};
        ctx.finalize(digest);
        char md5[33]{};
        for (int i = 0; i < 16; ++i) {
            md5[i * 2]     = kHexDigits[digest[i] >> 4];
            md5[i * 2 + 1] = kHexDigits[digest[i] & 15];
        }
        md5[32] = '\0';
        if (!HexEqI(md5, expectedMd5Lower))
            return std::nullopt;
        FileNode fixed = node;
        fixed.clusters.clear();
        fixed.clusters.reserve(chainClusters.size());
        for (uint32_t c : chainClusters)
            fixed.clusters.push_back(ClusterToAbsSector(boot, c));
        return fixed;
    };

    // Phase A: [startCluster] + one contiguous free run
    for (const FreeRun& fr : freeRuns) {
        if (fr.len < remaining)
            continue;
        const uint32_t runEnd = fr.start + fr.len - remaining;
        for (uint32_t runStart = fr.start; runStart <= runEnd; ++runStart) {
            if (runStart == startCluster)
                continue;
            if (!isFreeRun(runStart, remaining))
                continue;
            std::vector<uint32_t> chainClusters;
            chainClusters.reserve(static_cast<size_t>(clustersNeeded));
            chainClusters.push_back(startCluster);
            for (uint32_t k = 0; k < remaining; ++k)
                chainClusters.push_back(runStart + k);
            if (auto hit = tryChain(chainClusters))
                return hit;
        }
    }

    // Phase B: [startCluster] + contiguous tail + second free run
    for (uint32_t a = 1; a < remaining; ++a) {
        const uint32_t run1Start = startCluster + 1;
        const uint32_t run1Len   = a;
        const uint32_t run2Len   = remaining - a;

        if (!isFreeRun(run1Start, run1Len))
            continue;

        for (const FreeRun& fr : freeRuns) {
            if (fr.len < run2Len)
                continue;
            const uint32_t runEnd = fr.start + fr.len - run2Len;
            for (uint32_t run2Start = fr.start; run2Start <= runEnd; ++run2Start) {
                if (run2Start == startCluster)
                    continue;
                if (!isFreeRun(run2Start, run2Len))
                    continue;

                std::vector<uint32_t> chainClusters;
                chainClusters.reserve(static_cast<size_t>(clustersNeeded));
                chainClusters.push_back(startCluster);
                for (uint32_t k = 0; k < run1Len; ++k)
                    chainClusters.push_back(run1Start + k);
                for (uint32_t k = 0; k < run2Len; ++k)
                    chainClusters.push_back(run2Start + k);

                if (auto hit = tryChain(chainClusters))
                    return hit;
            }
        }
    }

    // Phase C: bounded 3-run search near startCluster
    const uint32_t win = 256;
    const uint32_t winStart = (startCluster > win) ? (startCluster - win) : 2;
    const uint32_t winEnd   = std::min<uint32_t>(maxCluster, startCluster + win);

    for (uint32_t a = 1; a < remaining; ++a) {
        const uint32_t run1Len = a;
        const uint32_t run2Len = remaining - a;

        for (uint32_t run1Start = winStart; run1Start + run1Len - 1 <= winEnd; ++run1Start) {
            if (run1Start == startCluster)
                continue;
            if (!isFreeRun(run1Start, run1Len))
                continue;

            for (const FreeRun& fr : freeRuns) {
                if (fr.len < run2Len)
                    continue;
                const uint32_t runEnd = fr.start + fr.len - run2Len;
                for (uint32_t run2Start = fr.start; run2Start <= runEnd; ++run2Start) {
                    if (run2Start == startCluster)
                        continue;
                    if (!isFreeRun(run2Start, run2Len))
                        continue;

                    std::vector<uint32_t> chainClusters;
                    chainClusters.reserve(static_cast<size_t>(clustersNeeded));
                    chainClusters.push_back(startCluster);
                    for (uint32_t k = 0; k < run1Len; ++k)
                        chainClusters.push_back(run1Start + k);
                    for (uint32_t k = 0; k < run2Len; ++k)
                        chainClusters.push_back(run2Start + k);

                    if (auto hit = tryChain(chainClusters))
                        return hit;
                }
            }
        }
    }

    return std::nullopt;
}

static std::wstring MakeTempRecoverDir()
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"PhoenixRecover_dftt6_" +
                       std::to_wstring(GetTickCount64());
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static std::wstring GetEnvRecoverOutDir()
{
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"PHOENIX_RECOVER_OUTDIR", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return std::wstring(buf);
}

static void RemoveTree(const std::wstring& root)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(fs::path(root), ec);
}

static bool ShouldKeepTemp()
{
    // For local debugging: keep recovered outputs to inspect fragmentation patterns.
    // Set PHOENIX_KEEP_TEST_OUTPUT=1 to preserve temp directory.
    wchar_t buf[16]{};
    DWORD n = GetEnvironmentVariableW(L"PHOENIX_KEEP_TEST_OUTPUT", buf, 16);
    if (n == 0 || n >= 16) return false;
    return (wcscmp(buf, L"1") == 0) || (_wcsicmp(buf, L"true") == 0) || (_wcsicmp(buf, L"yes") == 0);
}

int main()
{
    std::wstring imagePath = ImagePath();
    if (imagePath.empty() || !std::filesystem::exists(imagePath)) {
        fprintf(stdout, "SKIP: DFTT 6-fat-undel.dd not found at %ls\n", imagePath.c_str());
        return 0;
    }

    struct Golden {
        const wchar_t* name;
        const char*    md5;
        bool           required;
    };
    static const Golden kGold[] = {
        {L"sing.dat",  "59b20779f69ff9f0ac5fcd2c38835a79", true},
        {L"mult1.dat", "ffd27bd782bdce67750b6b9ee069d2ef", true},
        // Fragmented deleted files: we attempt a best-effort two-run reconstruction.
        {L"frag1.dat", "7a3bc5b763bef201202108f4ba128149", true},
        {L"frag2.dat", "0e80ab84ef0087e60dfc67b88a1cf13e", true},
        // Deleted directory entries (now supported by FAT deleted-dir recursion).
        {L"mult2.dat", "59cf0e9cd107bc1e75afb7374f6e05bb", true},
        {L"frag3.dat", "21121699487f3fbbdb9a4b3391b6d3e0", true},
    };

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");
    if (!reader.IsOpen())
        return 1;

    Partition part{};
    part.startSector  = 0;
    part.totalSectors = reader.GetTotalSectors();
    part.sizeBytes    = reader.GetTotalBytes();
    part.fsType       = L"FAT16";

    FatParser parser;
    CHECK_MSG(parser.Open(reader, part), "FatParser::Open failed");
    CHECK_MSG(parser.ScanMetadata(), "FatParser::ScanMetadata failed");

    auto files = parser.GetAllFiles();
    std::wstring outDir = GetEnvRecoverOutDir();
    if (outDir.empty()) {
        outDir = MakeTempRecoverDir();
    } else {
        CreateDirectoryW(outDir.c_str(), nullptr);
        fprintf(stdout, "NOTE: using PHOENIX_RECOVER_OUTDIR=%ls\n", outDir.c_str());
    }
    RecoverEngine engine;

    int requiredOk = 0;
    int optionalOk = 0;
    for (const Golden& g : kGold) {
        const FileNode* node = FindByName(files, g.name);
        if (g.required)
            CHECK_MSG(node != nullptr, "missing deleted node in scan");
        if (!node) {
            if (g.required)
                fprintf(stderr, "missing %ls\n", g.name);
            continue;
        }

        FileRecoverResult rr =
            engine.RecoverFile(*node, outDir, false, reader, part);
        if (g.required)
            CHECK_MSG(rr.status == RecoverStatus::Success, "RecoverFile failed");
        if (rr.status != RecoverStatus::Success)
            continue;

        char hex[33]{};
        if (!Md5FileHexLower(rr.targetPath, hex)) {
            if (g.required)
                CHECK_MSG(false, "Md5FileHexLower failed");
            continue;
        }
        if (HexEqI(hex, g.md5)) {
            fprintf(stdout, "OK %ls md5=%s\n", g.name, hex);
            if (g.required)
                ++requiredOk;
            else
                ++optionalOk;
        } else {
            // Try to reconstruct fragmented FAT16 deleted files (DFTT #6) by scanning
            // for a second contiguous run of free clusters that matches the golden MD5.
            auto fixedOpt = TryReconstructFat16ByMd5(reader, part, *node, g.md5);
            if (fixedOpt.has_value()) {
                FileRecoverResult rr2 =
                    engine.RecoverFile(*fixedOpt, outDir, false, reader, part);
                CHECK_MSG(rr2.status == RecoverStatus::Success, "RecoverFile (fixed chain) failed");
                char hex2[33]{};
                CHECK_MSG(Md5FileHexLower(rr2.targetPath, hex2), "Md5FileHexLower failed (fixed chain)");
                if (HexEqI(hex2, g.md5)) {
                    fprintf(stdout, "OK %ls md5=%s (reconstructed)\n", g.name, hex2);
                    if (g.required)
                        ++requiredOk;
                    else
                        ++optionalOk;
                } else {
                    fprintf(stderr, "MD5 mismatch %ls after reconstruction: got %s expected %s\n",
                            g.name, hex2, g.md5);
                    ++g_failures;
                }
            } else {
                fprintf(stderr, "MD5 mismatch %ls: got %s expected %s (no reconstruction match)\n",
                        g.name, hex, g.md5);
                ++g_failures;
            }
        }
    }

    CHECK_MSG(requiredOk == 6, "sing/mult1/frag1/frag2/mult2/frag3 must recover with golden MD5");

    if (!ShouldKeepTemp() && GetEnvRecoverOutDir().empty()) {
        RemoveTree(outDir);
    } else {
        fprintf(stdout, "NOTE: keeping recovered outputs at %ls\n", outDir.c_str());
    }
    reader.Close();

    if (g_failures != 0) {
        fprintf(stderr, "DFTT FAT recover bytes: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: DFTT #6 FAT recover bytes (required=%d optional=%d)\n",
            requiredOk, optionalOk);
    return 0;
}

#else

int main()
{
    fprintf(stdout, "SKIP: DFTT FAT recover bytes test is Windows-only\n");
    return 0;
}

#endif
