// PartitionProbe + SectorReader integration test
//
// Opens the CFReDS dfr-01-fat.dd (has MBR with 3 FAT partitions) and
// the DFTT 10-ntfs-autodetect image (has MBR with 2 NTFS partitions),
// verifies SectorReader::OpenImage and DetectByVbr produce correct results.
//
// Pure C++ on Windows (no Qt).

#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#include "disk/SectorReader.h"
#include "disk/PartitionProbe.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "FAIL: %s  (%s:%d)\n", #expr, __FILE__, __LINE__); \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

#define CHECK_MSG(expr, msg)                                                \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "FAIL: %s — %s  (%s:%d)\n",                    \
                    #expr, (msg), __FILE__, __LINE__);                      \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

static std::wstring SourceDir()
{
#ifdef PHOENIX_SOURCE_DIR
    return PHOENIX_SOURCE_DIR;
#else
    return L".";
#endif
}

static void TestSectorReaderOpenImage()
{
    fprintf(stdout, "\n--- TestSectorReaderOpenImage ---\n");
    namespace fs = std::filesystem;
    auto imagePath = (fs::path(SourceDir()) / "testdata" / "dftt" / "6-undel-fat" / "6-fat-undel.dd").wstring();

    if (!fs::exists(imagePath)) {
        fprintf(stdout, "SKIP: %ls not found\n", imagePath.c_str());
        return;
    }

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");
    CHECK(reader.IsOpen());
    CHECK(reader.GetSectorSize() == 512);
    CHECK(reader.GetTotalSectors() == 12032);
    CHECK(reader.GetTotalBytes() == 12032ULL * 512);

    // Read sector 0 and verify it looks like a FAT VBR
    std::vector<uint8_t> sec(512);
    CHECK(reader.ReadSectors(0, sec.data(), 1));
    // FAT VBR: bytes per sector at offset 11
    uint16_t bps = *reinterpret_cast<uint16_t*>(&sec[11]);
    CHECK_MSG(bps == 512, "bytes-per-sector should be 512");

    // Stats
    CHECK(reader.GetSectorsRead() >= 1);
    CHECK(reader.GetBadSectorCount() == 0);

    reader.Close();
    CHECK(!reader.IsOpen());
    fprintf(stdout, "  PASSED\n");
}

static void TestMbrParsing()
{
    fprintf(stdout, "\n--- TestMbrParsing (CFReDS dfr-01-fat.dd) ---\n");
    namespace fs = std::filesystem;
    auto imagePath = (fs::path(SourceDir()) / "testdata" / "cfreds" / "dfr-01-fat.dd").wstring();

    if (!fs::exists(imagePath)) {
        fprintf(stdout, "SKIP: %ls not found\n", imagePath.c_str());
        return;
    }

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");

    std::vector<uint8_t> mbr(512);
    CHECK(reader.ReadSectors(0, mbr.data(), 1));
    CHECK_MSG(mbr[510] == 0x55 && mbr[511] == 0xAA, "MBR signature");

    // Count partitions
    int count = 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = mbr.data() + 446 + i * 16;
        if (e[4] != 0) ++count;
    }
    CHECK_MSG(count == 3, "Expected 3 partitions in CFReDS FAT image MBR");
    fprintf(stdout, "  MBR has %d partitions\n", count);

    // Test DetectByVbr on each partition
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = mbr.data() + 446 + i * 16;
        if (e[4] == 0) continue;
        uint32_t startLBA = *reinterpret_cast<const uint32_t*>(e + 8);
        uint32_t numSects = *reinterpret_cast<const uint32_t*>(e + 12);

        std::vector<uint8_t> vbr(512);
        CHECK(reader.ReadSectors(startLBA, vbr.data(), 1));

        std::wstring fsType;
        uint64_t fsSectors = 0;
        bool detected = PartitionProbe::DetectByVbr(vbr.data(), 512, fsType, fsSectors);
        fprintf(stdout, "  Part[%d]: start=%u sectors=%u detected=%d fs=%ls fsSectors=%llu\n",
                i, startLBA, numSects, detected ? 1 : 0, fsType.c_str(), fsSectors);
        CHECK_MSG(detected, "DetectByVbr should succeed on a valid FAT partition");
    }

    reader.Close();
    fprintf(stdout, "  PASSED\n");
}

static void TestNtfsAutodetectDualFs()
{
    fprintf(stdout, "\n--- TestNtfsAutodetectDualFs (DFTT #10 part images) ---\n");
    namespace fs = std::filesystem;
    struct Case {
        const wchar_t* file;
        const wchar_t* expectA;
        const wchar_t* expectB;
    };
    static const Case kCases[] = {
        {L"10-ntfs-part1.dd", L"NTFS", L"ext2"},
        {L"10-ntfs-part2.dd", L"NTFS", L"UFS1"},
    };

    for (const Case& c : kCases) {
        auto imagePath = (fs::path(SourceDir()) / "testdata" / "dftt" / "10-ntfs-autodetect" / c.file)
                             .wstring();
        if (!fs::exists(imagePath)) {
            fprintf(stdout, "SKIP: %ls not found\n", c.file);
            continue;
        }

        SectorReader reader;
        CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");
        const auto found = PartitionProbe::DetectAllFileSystems(reader, 0, reader.GetSectorSize());
        reader.Close();

        bool hasA = false;
        bool hasB = false;
        for (const PartitionCandidate& p : found) {
            fprintf(stdout, "  %ls: fs=%ls source=%ls conf=%d\n",
                    c.file, p.fsType.c_str(), p.source.c_str(), p.confidence);
            if (p.fsType == c.expectA)
                hasA = true;
            if (p.fsType == c.expectB)
                hasB = true;
            if (c.expectB == L"ext2" &&
                (p.fsType == L"ext2" || p.fsType == L"ext3" || p.fsType == L"ext4"))
                hasB = true;
        }
        CHECK_MSG(hasA && hasB,
                  "partition must expose both coexisting file systems (dual-FS warning)");
    }
    fprintf(stdout, "  PASSED\n");
}

static void TestPartitionProbeBounds()
{
    fprintf(stdout, "\n--- TestPartitionProbeBounds ---\n");
    namespace fs = std::filesystem;
    auto imagePath = (fs::path(SourceDir()) / "testdata" / "dftt" /
                      "6-undel-fat" / "6-fat-undel.dd").wstring();
    if (!fs::exists(imagePath)) {
        fprintf(stdout, "SKIP: bounds image not found\n");
        return;
    }

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");

    std::wstring fsType;
    uint64_t sectors = 0;
    CHECK(!PartitionProbe::DetectByVbr(nullptr, 512, fsType, sectors));
    CHECK(!PartitionProbe::DetectExt4(reader, reader.GetTotalSectors(),
                                      reader.GetSectorSize(), sectors, &fsType));
    CHECK(!PartitionProbe::DetectUfs1(reader, reader.GetTotalSectors(),
                                      reader.GetSectorSize()));

    PartitionProbe::Options opt;
    opt.mode = PartitionProbe::ProbeMode::LostOnly;
    opt.scanStartSector = reader.GetTotalSectors();
    opt.scanEndSector = 0;
    auto none = PartitionProbe::ProbeVolume(reader, opt, nullptr, nullptr);
    CHECK(none.empty());

    reader.Close();
    fprintf(stdout, "  PASSED\n");
}

static void TestNtfsAutodetect()
{
    fprintf(stdout, "\n--- TestNtfsAutodetect (DFTT #10) ---\n");
    namespace fs = std::filesystem;
    auto imagePath = (fs::path(SourceDir()) / "testdata" / "dftt" / "10-ntfs-autodetect" / "10-ntfs-disk.dd").wstring();

    if (!fs::exists(imagePath)) {
        fprintf(stdout, "SKIP: %ls not found\n", imagePath.c_str());
        return;
    }

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage failed");

    std::vector<uint8_t> mbr(512);
    CHECK(reader.ReadSectors(0, mbr.data(), 1));
    CHECK_MSG(mbr[510] == 0x55 && mbr[511] == 0xAA, "MBR signature");

    int ntfsCount = 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t* e = mbr.data() + 446 + i * 16;
        if (e[4] == 0) continue;
        uint32_t startLBA = *reinterpret_cast<const uint32_t*>(e + 8);

        std::vector<uint8_t> vbr(512);
        if (!reader.ReadSectors(startLBA, vbr.data(), 1)) continue;

        std::wstring fsType;
        uint64_t fsSectors = 0;
        if (PartitionProbe::DetectByVbr(vbr.data(), 512, fsType, fsSectors)) {
            fprintf(stdout, "  Part[%d]: start=%u fs=%ls sectors=%llu\n",
                    i, startLBA, fsType.c_str(), fsSectors);
            if (fsType == L"NTFS") ++ntfsCount;
        }
    }

    CHECK_MSG(ntfsCount >= 2, "Expected at least 2 NTFS partitions in DFTT #10 image");
    reader.Close();
    fprintf(stdout, "  PASSED (%d NTFS partitions)\n", ntfsCount);
}

int main()
{
    TestSectorReaderOpenImage();
    TestMbrParsing();
    TestNtfsAutodetect();
    TestNtfsAutodetectDualFs();
    TestPartitionProbeBounds();

    if (g_failures == 0) {
        fprintf(stdout, "\n=== PartitionProbe + SectorReader: ALL CHECKS PASSED ===\n");
    } else {
        fprintf(stderr, "\n=== PartitionProbe + SectorReader: %d CHECK(S) FAILED ===\n", g_failures);
    }
    return g_failures ? 1 : 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: PartitionProbe test requires volume I/O platform.\n");
    return 0;
}

#endif
