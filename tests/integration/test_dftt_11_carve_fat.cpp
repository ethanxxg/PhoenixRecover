// DFTT Test Image #11 - Basic Data Carving #1 (FAT32 raw partition, corrupted VBR).
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#include "dftt_golden.h"
#include "dftt_test_md5.h"
#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "recover/FileNode.h"
#include "scan/RawScan.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

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
    return (fs::path(PHOENIX_SOURCE_DIR) / "testdata" / "dftt" / "11-carve-fat" / "11-carve-fat.dd")
        .wstring();
#else
    return L"";
#endif
}

struct CarveHit {
    uint64_t offset = 0;
    uint64_t size   = 0;
    int      recoverability = 0;
};

static int RecoverRank(RecoverabilityLevel r)
{
    switch (r) {
    case RecoverabilityLevel::Excellent: return 4;
    case RecoverabilityLevel::Good: return 3;
    case RecoverabilityLevel::Poor: return 2;
    case RecoverabilityLevel::Lost: return 1;
    default: return 0;
    }
}

int main()
{
    std::wstring imagePath = ImagePath();
    if (imagePath.empty() || !std::filesystem::exists(imagePath)) {
        fprintf(stdout, "SKIP: DFTT 11-carve-fat.dd not found at %ls\n", imagePath.c_str());
        return 0;
    }

    char imgMd5[33]{};
    CHECK_MSG(DfttMd5FileHexLower(imagePath.c_str(), imgMd5), "Md5FileHexLower failed");
    CHECK_MSG(DfttHexEqI(imgMd5, "0069813c892a462f88dc6d376624f7d9"), "unexpected image MD5");

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");
    if (!reader.IsOpen())
        return 1;

    Partition part{};
    part.startSector  = 0;
    part.totalSectors = reader.GetTotalSectors();
    part.sizeBytes    = part.totalSectors * reader.GetSectorSize();
    part.fsType       = L"FAT32";

    RawScan raw;
    ScanResult result = raw.Run(reader, part);

    CHECK_MSG(result.errorMsg.empty(), "scan returned errorMsg");
    CHECK_MSG(!result.cancelled, "scan cancelled");
    CHECK(result.foundCount >= 5u);
    CHECK(result.foundCount <= 500u);

    constexpr uint64_t kMaxSweep = 16ULL * 1024 * 1024;
    std::map<std::string, CarveHit> md5Hits;

    for (const FileNode& f : result.files) {
        if (f.size == 0 || f.size > kMaxSweep || f.size > UINT32_MAX)
            continue;
        std::vector<uint8_t> buf(static_cast<size_t>(f.size));
        if (!reader.ReadBytes(f.rawOffset, buf.data(), static_cast<uint32_t>(buf.size())))
            continue;
        char hex[33]{};
        if (!DfttMd5BufferHexLower(buf.data(), buf.size(), hex))
            continue;
        auto& slot = md5Hits[hex];
        if (slot.size == 0) {
            slot.offset           = f.rawOffset;
            slot.size             = f.size;
            slot.recoverability   = RecoverRank(f.recoverability);
        }
    }

    int requiredOk = 0;
    int requiredTotal = 0;
    for (const DfttCarveGolden& g : kDftt11CarveGold) {
        if (g.forbidValidCarve) {
            auto it = md5Hits.find(g.md5);
            if (it != md5Hits.end() && it->second.size == g.sizeBytes &&
                it->second.recoverability >= RecoverRank(RecoverabilityLevel::Good)) {
                fprintf(stderr,
                        "FAIL: corrupted %s must not carve as Good+ JPEG (md5=%s off=0x%llX rank=%d)\n",
                        g.name, g.md5,
                        static_cast<unsigned long long>(it->second.offset),
                        it->second.recoverability);
                ++g_failures;
            } else if (it != md5Hits.end()) {
                fprintf(stdout, "  OK forbid %s present but low rank=%d\n", g.name,
                        it->second.recoverability);
            }
            continue;
        }

        const bool stretch = false;
        if (!g.required && !g.forbidValidCarve)
            continue;
        if (g.required)
            ++requiredTotal;

        auto it = md5Hits.find(g.md5);
        if (it == md5Hits.end()) {
            fprintf(stderr, "FAIL: missing golden carve %s md5=%s\n", g.name, g.md5);
            ++g_failures;
            continue;
        }
        if (g.sizeBytes != 0 && it->second.size != g.sizeBytes) {
            fprintf(stderr, "FAIL: %s size mismatch carved=%llu expected=%llu\n",
                    g.name, static_cast<unsigned long long>(it->second.size),
                    static_cast<unsigned long long>(g.sizeBytes));
            ++g_failures;
            continue;
        }
        ++requiredOk;
        fprintf(stdout, "  OK %s carved=%llu off=0x%llX\n",
                g.name, static_cast<unsigned long long>(it->second.size),
                static_cast<unsigned long long>(it->second.offset));
    }

    CHECK_MSG(requiredOk == requiredTotal,
              "all DFTT #11 golden MD5s must be carved");

    if (g_failures != 0) {
        fprintf(stderr, "DFTT #11 carve test: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: DFTT #11 RawScan carve required=%d/%d\n",
            requiredOk, requiredTotal);
    return 0;
}

#else

int main()
{
    fprintf(stdout, "SKIP: DFTT #11 carve test is Windows-only\n");
    return 0;
}

#endif
