// P0-7 regression gate: NTFS $ATTRIBUTE_LIST extension MFT records.
//
// Discovered on the real damaged LDM disk (2026-11-14): engine scan output
// reported a 56,725,784-byte file (MFT record 78) with clusterCount=0, and
// RecoverEngine refused it with "No recoverable data (no clusters and no
// resident data)". The base record had only $STANDARD_INFORMATION,
// $ATTRIBUTE_LIST and $FILE_NAME; the real $DATA lived in three EXTENSION
// records (10902 @VCN0, 11004 @VCN4508, 10737 @VCN9345). NtfsParser walked
// only the base record, so it never saw the runlist.
//
// This gate builds a 4 MiB synthetic NTFS volume with ONE base record whose
// unnamed $DATA is split over two extension records. The $ATTRIBUTE_LIST
// entries are deliberately written OUT OF VCN ORDER (VCN1 first, VCN0 second)
// so that a parser which simply concatenates them reconstructs the wrong
// cluster order and fails the byte check.
//
// Gates:
//   1. node.size == 1500 from the VCN0 segment's dataSize (the VCN1 segment
//      repeats 0 and must NOT overwrite it).
//   2. node.clusters == [48, 49] in VCN order (not [49, 48]).
//   3. RecoverEngine::RecoverBatch writes the exact 1500 payload bytes.
//   4. The extension records themselves (no $FILE_NAME) produce no nodes.
//
// Negative guard: PHOENIX_NEGATIVE_ATTRLIST_GUARD=1 omits $ATTRIBUTE_LIST
// entirely. Gates 2/3 MUST then fail, proving the gate really tests the
// attribute-list path rather than the stale $FILE_NAME.realSize fallback.
//
// The production NtfsParser / RecoverEngine sources are linked as-is.

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
        }                                                                    \
    } while (0)

namespace {

constexpr uint32_t kSectorSize   = 512;
constexpr uint32_t kSpc          = 2;
constexpr uint32_t kClusterBytes = kSectorSize * kSpc;   // 1024
constexpr uint32_t kRecordSize   = 1024;
constexpr uint64_t kMftLcn       = 4;
constexpr uint64_t kMftClusters  = 32;                    // records 0..31
constexpr uint64_t kDataLcn      = 48;                    // VCN0
constexpr uint32_t kTargetRec    = 20;
constexpr uint32_t kExtVcn1      = 22;                    // listed FIRST
constexpr uint32_t kExtVcn0      = 21;                    // listed SECOND
constexpr uint64_t kTotalSectors = 8192;
constexpr uint32_t kMainSize     = 1500;

const wchar_t kFileName[] = L"ATTR.BIN";

static uint32_t Align8(uint32_t n) { return (n + 7u) & ~7u; }

static void WriteLe16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

static void WriteLe32(uint8_t* p, uint32_t v)
{
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

static void WriteLe64(uint8_t* p, uint64_t v)
{
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (8 * i)) & 0xFF);
}

// One data run: 1-byte length + 1-byte LCN delta, then terminator.
static size_t EncodeDataRun(uint8_t* out, uint64_t lcn, uint64_t length)
{
    out[0] = 0x11;
    out[1] = static_cast<uint8_t>(length & 0xFF);
    out[2] = static_cast<uint8_t>(lcn & 0xFF);
    out[3] = 0x00;
    return 4;
}

static void WriteMftHeader(uint8_t* rec, uint32_t recordNumber)
{
    std::memset(rec, 0, kRecordSize);
    std::memcpy(rec, NTFS_FILE_MAGIC, 4);
    auto* h = reinterpret_cast<MFT_RECORD_HEADER*>(rec);
    h->usaOffset       = 0;                       // ApplyUSA() => true
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

// $ATTRIBUTE_LIST entry: 32 bytes, unnamed.
static void PutAttrListEntry(uint8_t* v, uint32_t off, uint32_t type,
                             uint64_t lowestVcn, uint64_t recordIndex)
{
    uint8_t* e = v + off;
    std::memset(e, 0, 32);
    WriteLe32(e + 0, type);
    WriteLe16(e + 4, 32);        // recordLength
    e[6] = 0;                    // nameLength (unnamed = default stream)
    e[7] = 26;                   // nameOffset (unused when nameLength==0)
    WriteLe64(e + 8, lowestVcn);
    WriteLe64(e + 16, recordIndex | (static_cast<uint64_t>(1) << 48));
    WriteLe16(e + 24, 1);        // instance
}

static uint32_t AppendAttrList(uint8_t* rec, uint32_t off)
{
    uint8_t value[128]{};
    PutAttrListEntry(value, 0,  ATTR_STANDARD_INFORMATION, 0, kTargetRec);
    PutAttrListEntry(value, 32, ATTR_FILE_NAME,            0, kTargetRec);
    // Deliberately OUT OF ORDER: VCN1 first, VCN0 second.
    PutAttrListEntry(value, 64, ATTR_DATA,                 1, kExtVcn1);
    PutAttrListEntry(value, 96, ATTR_DATA,                 0, kExtVcn0);
    return AppendResident(rec, off, ATTR_ATTRIBUTE_LIST, value, 128);
}

static uint32_t AppendExtensionData(uint8_t* rec, uint32_t off,
                                    uint64_t startVcn, uint64_t lcn,
                                    uint64_t dataSize)
{
    auto* ah = reinterpret_cast<ATTR_HEADER_COMMON*>(rec + off);
    ah->type        = ATTR_DATA;
    ah->nonResident = 1;
    ah->nameLength  = 0;
    ah->nameOffset  = 0;
    ah->flags       = 0;
    ah->attrId      = 0;

    auto* nr = reinterpret_cast<ATTR_NONRESIDENT*>(rec + off);
    nr->startVcn        = startVcn;
    nr->lastVcn         = startVcn;
    nr->compressionUnit = 0;
    nr->allocatedSize   = kClusterBytes;
    nr->dataSize        = dataSize;
    nr->initializedSize = dataSize;

    const uint32_t runOff = static_cast<uint32_t>(
        offsetof(ATTR_NONRESIDENT, initializedSize) + sizeof(uint64_t));
    nr->dataRunOffset = static_cast<uint16_t>(runOff);
    uint8_t runs[8]{};
    const size_t runsLen = EncodeDataRun(runs, lcn, 1);
    std::memcpy(rec + off + runOff, runs, runsLen);
    ah->length = static_cast<uint32_t>(Align8(runOff + runsLen));
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
    WriteLe64(b + 56, kMftLcn + 1);
    b[64] = static_cast<uint8_t>(-10);       // 1<<10 = 1024 B / record
    b[510] = 0x55; b[511] = 0xAA;
}

static std::vector<uint8_t> MainPayload()
{
    std::vector<uint8_t> p(kMainSize);
    for (size_t i = 0; i < p.size(); ++i)
        p[i] = static_cast<uint8_t>((i * 37u + 11u) & 0xFF);
    return p;
}

static void PutRecord(std::vector<uint8_t>& image, uint32_t idx,
                      const uint8_t* rec)
{
    std::memcpy(image.data() + kMftLcn * kClusterBytes
                    + static_cast<uint64_t>(idx) * kRecordSize,
                rec, kRecordSize);
}

static std::vector<uint8_t> BuildImage(bool negativeGuard)
{
    std::vector<uint8_t> image(kTotalSectors * kSectorSize, 0);
    WriteBoot(image);

    // --- MFT record 0 ($MFT) : runlist covering LCN 4..35 ------------------
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
    PutRecord(image, 0, rec0);

    // --- Base record: $FILE_NAME + optional $ATTRIBUTE_LIST, NO $DATA -------
    uint8_t rec[kRecordSize]{};
    WriteMftHeader(rec, kTargetRec);
    uint32_t off = sizeof(MFT_RECORD_HEADER);

    uint8_t fnBuf[128]{};
    auto* fn = reinterpret_cast<FILE_NAME_ATTR*>(fnBuf);
    fn->parentDirRef   = 5;                       // root
    fn->allocatedSize  = 2 * kClusterBytes;
    fn->realSize       = kMainSize;              // deliberately "correct"
    fn->fileAttributes = 0x20;
    fn->nameLength     = 8;                      // L"ATTR.BIN"
    fn->nameType       = FN_NAMESPACE_WIN32;
    std::memcpy(fnBuf + sizeof(FILE_NAME_ATTR), kFileName,
                8 * sizeof(wchar_t));
    const uint32_t fnLen = static_cast<uint32_t>(
        sizeof(FILE_NAME_ATTR) + 8 * sizeof(wchar_t));
    off = AppendResident(rec, off, ATTR_FILE_NAME, fnBuf, fnLen);
    if (!negativeGuard)
        off = AppendAttrList(rec, off);
    FinalizeRecord(rec, off);
    PutRecord(image, kTargetRec, rec);

    // --- Extension records: each holds ONE unnamed non-resident $DATA ------
    uint8_t e0[kRecordSize]{};
    WriteMftHeader(e0, kExtVcn0);
    uint32_t eo = sizeof(MFT_RECORD_HEADER);
    eo = AppendExtensionData(e0, eo, 0, kDataLcn, kMainSize);
    FinalizeRecord(e0, eo);
    PutRecord(image, kExtVcn0, e0);

    uint8_t e1[kRecordSize]{};
    WriteMftHeader(e1, kExtVcn1);
    eo = sizeof(MFT_RECORD_HEADER);
    // dataSize=0 on the VCN1 segment: must NOT override the authoritative size.
    eo = AppendExtensionData(e1, eo, 1, kDataLcn + 1, 0);
    FinalizeRecord(e1, eo);
    PutRecord(image, kExtVcn1, e1);

    // --- Payload spans LCN 48..49 ------------------------------------------
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
    const bool negative = std::getenv("PHOENIX_NEGATIVE_ATTRLIST_GUARD") != nullptr;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() /
        ("phoenix_ntfs_attrlist_" + std::to_string(stamp));
    std::error_code ec;
    fs::create_directories(base, ec);

    const fs::path imagePath = base / "attrlist.dd";
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
    std::printf("parsed nodes=%zu (negative=%d)\n", files.size(),
                negative ? 1 : 0);

    const FileNode* primary = nullptr;
    int extNodes = 0;
    for (const auto& n : files) {
        std::printf("  node name=[%ls] id=%llu size=%llu clusters=%zu\n",
                    n.name.c_str(),
                    static_cast<unsigned long long>(n.id),
                    static_cast<unsigned long long>(n.size),
                    n.clusters.size());
        if (n.name == kFileName) primary = &n;
        else ++extNodes;
    }
    CHECK(primary != nullptr);
    // Gate 4: extension records have no $FILE_NAME and must not become nodes.
    CHECK(extNodes == 0);

    if (primary) {
        // Gate 1: authoritative size comes from the VCN0 segment only.
        CHECK(primary->size == kMainSize);
        // Gate 2: VCN order, NOT the (deliberately reversed) list order.
        CHECK(primary->clusters.size() == 2);
        if (primary->clusters.size() == 2) {
            CHECK(primary->clusters[0] == kDataLcn);
            CHECK(primary->clusters[1] == kDataLcn + 1);
        }
        CHECK(primary->residentData.empty());
        CHECK(!primary->isDirectory);

        // Gate 3: byte-level recovery through the production engine.
        const std::wstring outDir = (base / "out").wstring();
        RecoverEngine engine;
        RecoverBatchResult res = engine.RecoverBatch({*primary}, outDir,
                                                     false, reader, part);
        std::printf("recover: succeeded=%d failed=%d\n",
                    res.succeeded, res.failed);
        CHECK(res.succeeded == 1);
        CHECK(res.failed == 0);
        if (!res.details.empty() &&
            res.details[0].status == RecoverStatus::Success) {
            CHECK(ReadAll(res.details[0].targetPath) == MainPayload());
        } else {
            CHECK(false);   // no recovered file to verify
        }
    }

    fs::remove_all(base, ec);
    if (negative) {
        if (g_failures == 0) {
            std::fprintf(stderr, "FAIL: negative guard did not fail any "
                                 "check - gate is not falsifiable\n");
            return 1;
        }
        std::printf("NEGATIVE GUARD OK: %d checks failed as expected\n",
                    g_failures);
        return 0;
    }
    if (g_failures) {
        std::fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("OK: NtfsAttributeList (VCN-ordered runlist merge, "
                "authoritative VCN0 size, byte recovery verified)\n");
    return 0;
}
