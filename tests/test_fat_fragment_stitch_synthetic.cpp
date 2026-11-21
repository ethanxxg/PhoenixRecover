// Synthetic FAT16 volume: JPEG spans clusters 2+3; cluster 4 is a decoy.
// Verifies FatFragmentStitcher picks the free cluster 3 instead of a wrong tail.
#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "disk/SectorReader.h"
#include "fs/fat/FatStructs.h"
#include "recover/FragmentStitchService.h"
#include "recover/FragmentStitchPolicy.h"
#include "recover/FormatPayloadScore.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK_MSG(expr, msg) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s - %s (%s:%d)\n", #expr, (msg), __FILE__, __LINE__); \
            ++g_failures; \
        } \
    } while (0)

static void WriteLe16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

static void BuildMinimalFat16Image(std::vector<uint8_t>& image)
{
    constexpr uint32_t kSectorSize = 512;
    constexpr uint32_t kTotalSectors = 64;
    constexpr uint32_t kReserved = 1;
    constexpr uint32_t kNumFats = 2;
    constexpr uint32_t kFatSectors = 1;
    constexpr uint32_t kRootEntries = 16; // 1 root sector
    constexpr uint32_t kSpc = 1;
    constexpr uint32_t kDataStart = kReserved + kNumFats * kFatSectors + 1; // sector 4

    image.assign(static_cast<size_t>(kTotalSectors) * kSectorSize, 0);

    // Boot sector (FAT16 BPB)
    uint8_t* boot = image.data();
    boot[0] = 0xEB;
    boot[1] = 0x3C;
    boot[2] = 0x90;
    memcpy(boot + 3, "MSWIN4.1", 8);
    WriteLe16(boot + 11, static_cast<uint16_t>(kSectorSize));
    boot[13] = static_cast<uint8_t>(kSpc);
    WriteLe16(boot + 14, static_cast<uint16_t>(kReserved));
    boot[16] = static_cast<uint8_t>(kNumFats);
    WriteLe16(boot + 17, static_cast<uint16_t>(kRootEntries));
    WriteLe16(boot + 19, static_cast<uint16_t>(kTotalSectors));
    boot[21] = 0xF8; // media
    WriteLe16(boot + 22, static_cast<uint16_t>(kFatSectors));
    boot[36] = 0x29; // boot sig
    memcpy(boot + 54, "FAT16   ", 8);
    boot[510] = 0x55;
    boot[511] = 0xAA;

    auto writeFat = [&](uint32_t fatSectorIndex) {
        uint8_t* fat = image.data() + fatSectorIndex * kSectorSize;
        fat[0] = 0xF8;
        fat[1] = 0xFF;
        WriteLe16(fat + 2, 0xFFFF);
        WriteLe16(fat + 4, 0xFFFF); // cluster 2: EOC (broken chain)
        WriteLe16(fat + 6, 0x0000); // cluster 3: free
        WriteLe16(fat + 8, 0x0000); // cluster 4: free
    };
    writeFat(1);
    writeFat(2);

    const auto clusterSector = [&](uint32_t cluster) -> uint8_t* {
        const uint32_t sec = kDataStart + (cluster - 2) * kSpc;
        return image.data() + static_cast<size_t>(sec) * kSectorSize;
    };

    // JPEG payload (~600 bytes) across clusters 2 and 3
    uint8_t* c2 = clusterSector(2);
    uint8_t* c3 = clusterSector(3);
    constexpr uint32_t kFileSize = 600;
    c2[0] = 0xFF;
    c2[1] = 0xD8;
    memset(c2 + 2, 0xCC, kSectorSize - 2);
    memset(c3, 0xCC, kSectorSize);
    // EOI must fall within kFileSize (600): byte 598-599 => offset 86-87 in cluster 3
    const uint32_t eoiOffInC3 = kFileSize - kSectorSize - 2; // 86
    c3[eoiOffInC3]     = 0xFF;
    c3[eoiOffInC3 + 1] = 0xD9;

    // Decoy cluster 4: not a valid JPEG (should score below the true [2,3] chain)
    uint8_t* c4 = clusterSector(4);
    memset(c4, 0x00, kSectorSize);
}

static std::wstring WriteTempImage(const std::vector<uint8_t>& image)
{
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path =
        std::wstring(tmp) + L"PhoenixRecover_stitch_syn_" + std::to_wstring(GetTickCount64()) + L".img";
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    CHECK_MSG(h != INVALID_HANDLE_VALUE, "CreateFile image");
    if (h == INVALID_HANDLE_VALUE) return L"";
    DWORD written = 0;
    const BOOL ok =
        WriteFile(h, image.data(), static_cast<DWORD>(image.size()), &written, nullptr) && written == image.size();
    CloseHandle(h);
    CHECK_MSG(ok, "WriteFile image");
    return path;
}

int main()
{
    std::vector<uint8_t> image;
    BuildMinimalFat16Image(image);
    const std::wstring path = WriteTempImage(image);
    if (path.empty()) return 1;

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(path), "OpenImage");
    if (!reader.IsOpen()) {
        std::filesystem::remove(path);
        return 1;
    }

    constexpr uint32_t kSectorSize = 512;
    constexpr uint32_t kDataStart = 4;
    constexpr uint32_t kSpc = 1;
    constexpr uint32_t kTotalSectors = 64;
    constexpr uint32_t kTotalClusters = kTotalSectors - kDataStart;

    std::vector<uint8_t> fatTable(kSectorSize * 2, 0);
    CHECK_MSG(reader.ReadBytes(1 * kSectorSize, fatTable.data(), static_cast<uint32_t>(kSectorSize)),
              "read FAT");

    FatVolumeGeometry geo{};
    geo.bytesPerSector    = kSectorSize;
    geo.sectorsPerCluster = kSpc;
    geo.dataStartSector   = kDataStart;
    geo.totalClusters     = kTotalClusters;
    geo.fatType           = 16;

    FragmentStitchPolicy policy;
    policy.enabled = true;

    constexpr uint64_t kFileSize = 600;
    constexpr uint32_t kStartCluster = 2;
    // Wrong tail: cluster 4 (decoy) instead of cluster 3 (true continuation)
    std::vector<uint64_t> chain = {kDataStart + 0, kDataStart + 2};

    auto scoreAbsChain = [&](const std::vector<uint64_t>& absChain) -> FormatPayloadScore {
        std::vector<uint8_t> buf(static_cast<size_t>(kFileSize));
        uint64_t written = 0;
        for (uint64_t sec : absChain) {
            if (written >= kFileSize) break;
            const uint32_t toRead =
                static_cast<uint32_t>(std::min<uint64_t>(kSectorSize, kFileSize - written));
            if (!reader.ReadBytes(sec * kSectorSize, buf.data() + written, toRead)) {
                return FormatPayloadScore::Invalid;
            }
            written += toRead;
        }
        return FormatPayloadScoreUtil::Score(L"jpg", buf.data(), static_cast<size_t>(written));
    };

    const auto gold = scoreAbsChain({kDataStart + 0, kDataStart + 1});
    fprintf(stdout, "  score gold chain (2,3) = %d\n", static_cast<int>(gold));

    const auto before = scoreAbsChain(chain);
    fprintf(stdout, "  score before stitch (2,4) = %d\n", static_cast<int>(before));

    const bool improved = FragmentStitchService::TryImproveFatClusterChain(
        reader, geo, &fatTable, kStartCluster, kFileSize, L"jpg", chain, policy);
    CHECK_MSG(improved, "TryImprove should find cluster 3");
    CHECK_MSG(chain.size() == 2, "chain length");
    CHECK_MSG(chain[0] == kDataStart + 0, "cluster 2 sector");
    CHECK_MSG(chain[1] == kDataStart + 1, "cluster 3 sector");

    const auto after = scoreAbsChain(chain);
    fprintf(stdout, "  score after stitch = %d (sectors %llu,%llu)\n",
            static_cast<int>(after),
            static_cast<unsigned long long>(chain[0]),
            static_cast<unsigned long long>(chain[1]));
    CHECK_MSG(after >= FormatPayloadScore::Complete, "expected Complete JPEG");
    CHECK_MSG(static_cast<int>(after) > static_cast<int>(before), "score should improve");

    reader.Close();
    std::filesystem::remove(path);

    if (g_failures != 0) {
        fprintf(stderr, "FatFragmentStitchSynthetic: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: FatFragmentStitchSynthetic\n");
    return 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: FatFragmentStitchSynthetic requires volume I/O platform\n");
    return 0;
}

#endif
