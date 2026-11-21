// P0-5 regression gate: NTFS alternate data stream (ADS) handling.
//
// Discovered on the real damaged LDM disk (2026-11-14, batch_003): 11 files
// were reported as "engine byte mismatches" because the independent oracle
// parsed a named $DATA attribute ("Zone.Identifier", 26..277 bytes) as the
// default stream. The engine was in fact CORRECT: it read the unnamed
// non-resident $DATA (up to 5 MB) and ignored the ADS. Re-parsing those MFT
// records directly from disk with an ADS-safe parser gave 11/11 exact
// SHA-256 matches against the archived engine output.
//
// This gate builds a tiny synthetic NTFS volume containing ONE file record
// with BOTH:
//   * an unnamed non-resident $DATA  -> the real payload (1500 bytes, LCN 48)
//   * a named resident $DATA "Zone.Identifier" (26 bytes)
//
// Gate 1: primary node takes the UNNAMED $DATA (size=1500, clusters=[48,49],
//         no residentData). This is what a correct recovery engine must do.
// Gate 2: the ADS becomes a separate node named "<file>:Zone.Identifier" with
//         size=26 and the resident bytes attached.
// Gate 3: byte-level recovery through RecoverEngine::RecoverBatch writes the
//         1500 payload bytes back from the image, identical to the oracle.
// Gate 4: defect lock -- NtfsParser.cpp re-assigns each ADS node id as
//         (recordIndex << 4) | (adsIdx+1). That derived number can collide
//         with an UNRELATED file's recordIndex. On the real disk all four
//         batch_003 idCollisions come from exactly this encoding:
//           837<<4|1=13393, 15017<<4|1=240273,
//           15729<<4|1=251665, 18990<<4|1=303841
//         Each collision produced one surplus engine row (the ADS bytes)
//         paired against the other file's truth. Pinned here so a future
//         fix cannot silently drift the archive interpretation.
//
// Negative guard: PHOENIX_NEGATIVE_ADS_GUARD=1 mislabels the primary $DATA as
// a named ADS (nameLength=1) and sets $FILE_NAME.realSize to the ADS length.
// Gate 1 MUST then fail, proving the gate really checks stream selection.
//
// Zero production changes: NtfsParser / RecoverEngine are linked exactly as
// in the GUI. Only synthetic image bytes vary.

#include "common/Constants.h"
#include "disk/Partition.h"
#include "disk/SectorReader.h"
#include "fs/ntfs/NtfsParser.h"
#include "fs/ntfs/NtfsStructs.h"
#include "recover/FileNode.h"
#include "recover/RecoverEngine.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace Constants;

static int g_failures = 0;

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__,     \
                         __LINE__);                                          \
            ++g_failures;                                                    \
        }                                                                   \
    } while (0)

namespace {

constexpr uint32_t kSectorSize    = 512;
constexpr uint32_t kSpc           = 2;
constexpr uint32_t kClusterBytes  = kSectorSize * kSpc;   // 1024
constexpr uint32_t kRecordSize    = 1024;
constexpr uint64_t kMftLcn        = 4;
constexpr uint64_t kMftClusters   = 32;                    // records 0..31
constexpr uint64_t kDataLcn       = 48;
constexpr uint32_t kTargetRec     = 20;                    // >=16: not a meta file
constexpr uint64_t kTotalSectors  = 8192;                   // 4 MiB image
constexpr uint32_t kMainSize      = 1500;
constexpr uint32_t kAdsSize       = 26;

const wchar_t kFileName[]  = L"ADS.BIN";
const wchar_t kStreamName[] = L"Zone.Identifier";
const char    kAdsValue[]   = "[ZoneTransfer]\r\nZoneId=3\r\n";  // 26 bytes

static uint32_t Align8(uint32_t n) { return (n + 7u) & ~7u; }

static void WriteLe16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

static void WriteLe64(uint8_t* p, uint64_t v)
{
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

static size_t EncodeDataRun(uint8_t* out, uint64_t lcn, uint64_t length)
{
    out[0] = 0x11;                                  // 1-byte length, 1-byte LCN
    out[1] = static_cast<uint8_t>(length & 0xFF);
    out[2] = static_cast<uint8_t>(lcn & 0xFF);
    out[3] = 0x00;                                  // terminator
    return 4;
}

static void WriteMftHeader(uint8_t* rec, uint32_t recordNumber)
{
    std::memset(rec, 0, kRecordSize);
    std::memcpy(rec, NTFS_FILE_MAGIC, 4);
    auto* h = reinterpret_cast<MFT_RECORD_HEADER*>(rec);
    h->usaOffset       = 0;                        // ApplyUSA() => true
    h->usaCount        = 0;
    h->attrOffset      = sizeof(MFT_RECORD_HEADER);
    h->flags           = MFT_FLAG_IN_USE;
    h->bytesAllocated  = kRecordSize;
    h->mftRecordNumber = recordNumber;
}

static uint32_t AppendResident(uint8_t* rec, uint32_t off, uint32_t type,
                               const void* value, uint32_t valueLen)
{
    auto* ah = reinterpret_cast<ATTR_HEADER_COMMON*>(rec + off);
    ah->type        = type;
    ah->nonResident = 0;
    ah->nameLength  = 0;
    ah->nameOffset  = 0;
    ah->flags       = 0;
    ah->attrId      = 0;

    auto* res = reinterpret_cast<ATTR_RESIDENT*>(rec + off);
    res->valueLength = valueLen;
    res->valueOffset = sizeof(ATTR_RESIDENT);
    res->indexedFlag = 0;
    std::memcpy(rec + off + res->valueOffset, value, valueLen);
    ah->length = static_cast<uint32_t>(Align8(res->valueOffset + valueLen));
    return off + ah->length;
}

// The default (unnamed) $DATA stream: non-resident, real payload on disk.
static uint32_t AppendPrimaryData(uint8_t* rec, uint32_t off, bool mislabelAsAds)
{
    auto* ah = reinterpret_cast<ATTR_HEADER_COMMON*>(rec + off);
    ah->type        = ATTR_DATA;
    ah->nonResident = 1;
    ah->nameLength  = mislabelAsAds ? 1 : 0;
    ah->nameOffset  = mislabelAsAds
                    ? static_cast<uint16_t>(sizeof(ATTR_NONRESIDENT)) : 0;
    ah->flags       = 0;
    ah->attrId      = 0;
    if (mislabelAsAds) {
        const wchar_t fake[] = L"X";
        std::memcpy(rec + off + ah->nameOffset, fake, 2);
    }

    auto* nr = reinterpret_cast<ATTR_NONRESIDENT*>(rec + off);
    nr->startVcn        = 0;
    nr->lastVcn         = 1;                        // 2 clusters
    nr->compressionUnit = 0;
    nr->allocatedSize   = 2 * kClusterBytes;
    nr->dataSize        = kMainSize;
    nr->initializedSize = kMainSize;

    const uint32_t runOff = static_cast<uint32_t>(
        offsetof(ATTR_NONRESIDENT, initializedSize) + sizeof(uint64_t));
    nr->dataRunOffset = static_cast<uint16_t>(runOff);
    uint8_t runs[8]{};
    const size_t runsLen = EncodeDataRun(runs, kDataLcn, 2);
    std::memcpy(rec + off + runOff, runs, runsLen);
    ah->length = static_cast<uint32_t>(Align8(runOff + runsLen));
    return off + ah->length;
}

// A NAMED resident $DATA = alternate data stream (ADS).
static uint32_t AppendNamedResidentAds(uint8_t* rec, uint32_t off)
{
    constexpr uint8_t kNameLen = 16;                // L"Zone.Identifier"
    auto* ah = reinterpret_cast<ATTR_HEADER_COMMON*>(rec + off);
    ah->type        = ATTR_DATA;
    ah->nonResident = 0;
    ah->nameLength  = kNameLen;
    ah->nameOffset  = static_cast<uint16_t>(sizeof(ATTR_RESIDENT));
    ah->flags       = 0;
    ah->attrId      = 1;
    std::memcpy(rec + off + ah->nameOffset, kStreamName, kNameLen * sizeof(wchar_t));

    auto* res = reinterpret_cast<ATTR_RESIDENT*>(rec + off);
    const uint32_t valueOff = static_cast<uint32_t>(
        Align8(ah->nameOffset + kNameLen * sizeof(wchar_t)));
    res->valueLength = kAdsSize;
    res->valueOffset = static_cast<uint16_t>(valueOff);
    res->indexedFlag = 0;
    std::memcpy(rec + off + valueOff, kAdsValue, kAdsSize);
    ah->length = static_cast<uint32_t>(Align8(valueOff + kAdsSize));
    return off + ah->length;
}

static void FinalizeRecord(uint8_t* rec, uint32_t endOff)
{
    auto* h = reinterpret_cast<MFT_RECORD_HEADER*>(rec);
    auto* end = reinterpret_cast<ATTR_HEADER_COMMON*>(rec + endOff);
    end->type   = ATTR_END;
    end->length = 0;
    h->bytesInUse = endOff + sizeof(ATTR_HEADER_COMMON);
}

static void WriteBoot(std::vector<uint8_t>& image)
{
    uint8_t* b = image.data();
    std::memcpy(b + 3, "NTFS    ", 8);
    WriteLe16(b + 11, kSectorSize);
    b[13] = static_cast<uint8_t>(kSpc);
    b[21] = 0xF8;
    WriteLe64(b + 40, kTotalSectors);
    WriteLe64(b + 48, kMftLcn);
    WriteLe64(b + 56, kMftLcn + 1);                 // $MFTMirr
    b[64] = static_cast<uint8_t>(-10);              // 1<<10 = 1024 B / record
    b[510] = 0x55; b[511] = 0xAA;
}

static std::vector<uint8_t> MainPayload()
{
    std::vector<uint8_t> p(kMainSize);
    for (size_t i = 0; i < p.size(); ++i)
        p[i] = static_cast<uint8_t>((i * 31u + 7u) & 0xFF);
    return p;
}

static std::vector<uint8_t> BuildImage(bool negativeGuard)
{
    std::vector<uint8_t> image(kTotalSectors * kSectorSize, 0);
    WriteBoot(image);

    // --- MFT record 0 ($MFT itself) : runlist covering LCN 4..35 ----------
    uint8_t rec0[kRecordSize]{};
    WriteMftHeader(rec0, 0);
    uint32_t off0 = sizeof(MFT_RECORD_HEADER);
    auto* ah0 = reinterpret_cast<ATTR_HEADER_COMMON*>(rec0 + off0);
    ah0->type = ATTR_DATA; ah0->nonResident = 1; ah0->nameLength = 0;
    ah0->flags = 0; ah0->attrId = 0;
    auto* nr0 = reinterpret_cast<ATTR_NONRESIDENT*>(rec0 + off0);
    nr0->startVcn = 0; nr0->lastVcn = kMftClusters - 1;
    nr0->compressionUnit = 0;
    nr0->allocatedSize = kMftClusters * kClusterBytes;
    nr0->dataSize = kMftClusters * kClusterBytes;
    nr0->initializedSize = nr0->dataSize;
    const uint32_t runOff0 = static_cast<uint32_t>(
        offsetof(ATTR_NONRESIDENT, initializedSize) + sizeof(uint64_t));
    nr0->dataRunOffset = static_cast<uint16_t>(runOff0);
    uint8_t runs0[8]{};
    const size_t rl0 = EncodeDataRun(runs0, kMftLcn, kMftClusters);
    std::memcpy(rec0 + off0 + runOff0, runs0, rl0);
    ah0->length = static_cast<uint32_t>(Align8(runOff0 + rl0));
    FinalizeRecord(rec0, off0 + ah0->length);
    std::memcpy(image.data() + kMftLcn * kClusterBytes, rec0, kRecordSize);

    // --- MFT record kTargetRec : file with primary $DATA + Zone.Identifier -
    uint8_t rec[kRecordSize]{};
    WriteMftHeader(rec, kTargetRec);
    uint32_t off = sizeof(MFT_RECORD_HEADER);

    uint8_t fnBuf[128]{};
    auto* fn = reinterpret_cast<FILE_NAME_ATTR*>(fnBuf);
    fn->parentDirRef   = 5;                         // root
    fn->allocatedSize  = 2 * kClusterBytes;
    fn->realSize       = negativeGuard ? kAdsSize : kMainSize;
    fn->fileAttributes = 0x20;
    fn->nameLength     = 7;                         // L"ADS.BIN"
    fn->nameType       = FN_NAMESPACE_WIN32;
    std::memcpy(fnBuf + sizeof(FILE_NAME_ATTR), kFileName, 7 * sizeof(wchar_t));
    const uint32_t fnLen = static_cast<uint32_t>(
        sizeof(FILE_NAME_ATTR) + 7 * sizeof(wchar_t));
    off = AppendResident(rec, off, ATTR_FILE_NAME, fnBuf, fnLen);
    off = AppendPrimaryData(rec, off, negativeGuard);
    off = AppendNamedResidentAds(rec, off);
    FinalizeRecord(rec, off);
    std::memcpy(image.data() + kMftLcn * kClusterBytes
                    + static_cast<uint64_t>(kTargetRec) * kRecordSize,
                rec, kRecordSize);

    // --- payload clusters at LCN 48 ----------------------------------------
    const auto payload = MainPayload();
    std::memcpy(image.data() + kDataLcn * kClusterBytes,
                payload.data(), payload.size());
    return image;
}

static std::vector<uint8_t> ReadAll(const std::wstring& p)
{
    std::ifstream f(fs::path(p), std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

} // namespace

int main()
{
    const bool negative = std::getenv("PHOENIX_NEGATIVE_ADS_GUARD") != nullptr;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() /
        ("phoenix_ntfs_ads_" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(base, ec);

    const fs::path imagePath = base / "ads.dd";
    const auto image = BuildImage(negative);
    {
        std::ofstream f(imagePath, std::ios::binary);
        f.write(reinterpret_cast<const char*>(image.data()),
                static_cast<std::streamsize>(image.size()));
    }

    SectorReader reader;
    if (!reader.OpenImage(imagePath.wstring())) {
        std::fprintf(stderr, "FAIL: SectorReader::OpenImage\n");
        return 1;
    }
    Partition part;
    part.startSector  = 0;
    part.totalSectors = kTotalSectors;
    part.sizeBytes    = kTotalSectors * kSectorSize;
    part.fsType       = L"NTFS";

    NtfsParser parser;
    CHECK(parser.Open(reader, part));
    CHECK(parser.ScanMetadata());
    const auto files = parser.GetAllFiles();
    std::printf("parsed nodes=%zu (negative=%d)\n", files.size(), negative ? 1 : 0);

    const FileNode* primary = nullptr;
    const FileNode* ads     = nullptr;
    for (const auto& n : files) {
        std::printf("  node name=[%ls] id=%llu size=%llu clusters=%zu resident=%zu\n",
                    n.name.c_str(),
                    static_cast<unsigned long long>(n.id),
                    static_cast<unsigned long long>(n.size),
                    n.clusters.size(), n.residentData.size());
        if (n.name == kFileName) primary = &n;
        if (n.name.find(std::wstring(L":") + kStreamName) != std::wstring::npos)
            ads = &n;
    }
    CHECK(primary != nullptr);
    CHECK(ads != nullptr);

    if (primary) {
        // ---- Gate 1: the DEFAULT stream, never the ADS ---------------------
        std::printf("primary: id=%llu size=%llu clusters=%zu resident=%zu\n",
                    static_cast<unsigned long long>(primary->id),
                    static_cast<unsigned long long>(primary->size),
                    primary->clusters.size(), primary->residentData.size());
        CHECK(primary->size == kMainSize);
        CHECK(primary->clusters.size() == 2);
        if (primary->clusters.size() == 2) {
            CHECK(primary->clusters[0] == kDataLcn);
            CHECK(primary->clusters[1] == kDataLcn + 1);
        }
        CHECK(primary->residentData.empty());
        CHECK(!primary->isDirectory);
    }
    if (ads) {
        // ---- Gate 2: ADS is a separate node with its own bytes -------------
        std::printf("ads:     id=%llu size=%llu resident=%zu\n",
                    static_cast<unsigned long long>(ads->id),
                    static_cast<unsigned long long>(ads->size),
                    ads->residentData.size());
        CHECK(ads->size == kAdsSize);
        CHECK(ads->residentData.size() == kAdsSize);
        CHECK(std::memcmp(ads->residentData.data(), kAdsValue, kAdsSize) == 0);
        CHECK(ads->clusters.empty());
    }
    if (primary && ads) {
        // ---- Gate 4: pin the ADS id encoding defect ------------------------
        CHECK(ads->id == ((static_cast<uint64_t>(kTargetRec) << 4) | 1u));
        CHECK(ads->id != primary->id);   // not the SAME file's id
        // ...but the derived id can equal some OTHER file's recordIndex.
        struct RealCollision { uint64_t recordIndex, observedId; };
        const RealCollision real[] = {{837, 13393}, {15017, 240273},
                                      {15729, 251665}, {18990, 303841}};
        for (const auto& c : real)
            CHECK(((c.recordIndex << 4) | 1u) == c.observedId);
    }

    // ---- Gate 3: byte-level recovery through the production engine --------
    if (primary) {
        const std::wstring outDir = (base / "out").wstring();
        RecoverEngine engine;
        RecoverBatchResult res = engine.RecoverBatch({*primary}, outDir,
                                                     false, reader, part);
        std::printf("recover: succeeded=%d failed=%d\n", res.succeeded, res.failed);
        CHECK(res.succeeded == 1);
        CHECK(res.failed == 0);
        if (!res.details.empty() && res.details[0].status == RecoverStatus::Success) {
            const auto got = ReadAll(res.details[0].targetPath);
            CHECK(got == MainPayload());
        } else {
            CHECK(false);   // no successfully recovered file to verify
        }
    }

    fs::remove_all(base, ec);
    if (g_failures) {
        std::fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("OK: NtfsAdsStream (default stream selected, ADS isolated, "
                "byte recovery verified, id-collision defect locked)\n");
    return 0;
}
