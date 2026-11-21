#include "fragment_ntfs_structured_fixture.h"

#include "../../common/Constants.h"
#include "../../fs/ntfs/NtfsStructs.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

using namespace Constants;

namespace {

constexpr uint32_t kSectorSize     = 512;
constexpr uint32_t kSpc          = 2;
constexpr uint32_t kClusterBytes = kSectorSize * kSpc;
constexpr uint32_t kRecordSize   = 1024;
constexpr uint64_t kMftStartLcn  = 4;
constexpr uint64_t kBitmapLcn    = 16;
constexpr uint64_t kDataLcn0     = 24;
constexpr uint64_t kDecoyLcn     = 26;
constexpr uint32_t kDeletedRec   = 40;
constexpr uint32_t kMftRunClusters = 64;
constexpr uint32_t kTotalSectors = 8192;

static void WriteLe16(uint8_t* p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

static void WriteLe32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

static void WriteLe64(uint8_t* p, uint64_t v)
{
    for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((v >> (i * 8)) & 0xFF);
}

static uint64_t ClusterOffset(uint64_t lcn)
{
    return lcn * kClusterBytes;
}

static void WriteJpegClusters(std::vector<uint8_t>& image, uint64_t lcn0, uint64_t lcn1)
{
    uint8_t* c0 = image.data() + ClusterOffset(lcn0);
    uint8_t* c1 = image.data() + ClusterOffset(lcn1);
    memset(c0, 0xCC, kClusterBytes);
    memset(c1, 0xCC, kClusterBytes);
    c0[0] = 0xFF;
    c0[1] = 0xD8;
    const uint32_t eoiOff =
        FragmentNtfsStructuredFixture::kStructuredFileSize - kClusterBytes - 2;
    c1[eoiOff]     = 0xFF;
    c1[eoiOff + 1] = 0xD9;
}

static size_t EncodeDataRun(uint8_t* out, uint64_t lcn, uint64_t length)
{
    out[0] = 0x11;
    out[1] = static_cast<uint8_t>(length & 0xFF);
    out[2] = static_cast<uint8_t>(lcn & 0xFF);
    out[3] = 0x00;
    return 4;
}

static void SetBitmapBit(std::vector<uint8_t>& bitmap, uint64_t lcn, bool allocated)
{
    const uint64_t byte = lcn / 8;
    const uint8_t  bit  = static_cast<uint8_t>(lcn % 8);
    if (byte >= bitmap.size()) bitmap.resize(static_cast<size_t>(byte + 1), 0);
    if (allocated) bitmap[static_cast<size_t>(byte)] |= static_cast<uint8_t>(1u << bit);
    else bitmap[static_cast<size_t>(byte)] &= static_cast<uint8_t>(~(1u << bit));
}

static void WriteMftHeader(uint8_t* rec,
                           uint16_t flags,
                           uint32_t bytesInUse,
                           uint32_t recordNumber)
{
    memset(rec, 0, kRecordSize);
    memcpy(rec, NTFS_FILE_MAGIC, 4);
    auto* hdr = reinterpret_cast<MFT_RECORD_HEADER*>(rec);
    hdr->usaOffset      = 0;
    hdr->usaCount       = 0;
    hdr->attrOffset     = sizeof(MFT_RECORD_HEADER);
    hdr->flags          = flags;
    hdr->bytesInUse     = bytesInUse;
    hdr->bytesAllocated = kRecordSize;
    hdr->mftRecordNumber = recordNumber;
}

static uint32_t AppendResidentAttr(uint8_t* rec,
                                 uint32_t off,
                                 uint32_t type,
                                 const void* value,
                                 uint32_t valueLen)
{
    auto* ah = reinterpret_cast<ATTR_HEADER_COMMON*>(rec + off);
    ah->type        = type;
    ah->nonResident = 0;
    ah->nameLength  = 0;
    ah->flags       = 0;
    ah->attrId      = 0;

    auto* res = reinterpret_cast<ATTR_RESIDENT*>(rec + off);
    res->valueLength = valueLen;
    res->valueOffset = sizeof(ATTR_RESIDENT);
    res->indexedFlag = 0;

    memcpy(rec + off + res->valueOffset, value, valueLen);
    ah->length = res->valueOffset + valueLen;
    return off + ah->length;
}

static uint32_t AppendDataAttr(uint8_t* rec,
                             uint32_t off,
                             uint64_t dataSize,
                             const uint8_t* runs,
                             size_t runsLen)
{
    auto* ah = reinterpret_cast<ATTR_HEADER_COMMON*>(rec + off);
    ah->type        = ATTR_DATA;
    ah->nonResident = 1;
    ah->nameLength  = 0;
    ah->flags       = 0;
    ah->attrId      = 0;

    auto* nr = reinterpret_cast<ATTR_NONRESIDENT*>(rec + off);
    nr->startVcn        = 0;
    nr->lastVcn         = (dataSize + kClusterBytes - 1) / kClusterBytes - 1;
    nr->compressionUnit = 0;
    nr->allocatedSize   = ((dataSize + kClusterBytes - 1) / kClusterBytes) * kClusterBytes;
    nr->dataSize        = dataSize;
    nr->initializedSize = dataSize;

    const uint32_t runOff =
        static_cast<uint32_t>(offsetof(ATTR_NONRESIDENT, initializedSize) + sizeof(uint64_t));
    nr->dataRunOffset = static_cast<uint16_t>(runOff);
    memcpy(rec + off + runOff, runs, runsLen);
    ah->length = runOff + static_cast<uint32_t>(runsLen);
    return off + ah->length;
}

static void FinalizeMftRecord(uint8_t* rec, uint32_t endOff)
{
    auto* hdr = reinterpret_cast<MFT_RECORD_HEADER*>(rec);
    hdr->bytesInUse = endOff;
    auto* endAttr = reinterpret_cast<ATTR_HEADER_COMMON*>(rec + endOff);
    endAttr->type   = ATTR_END;
    endAttr->length = 0;
}

static void WriteFileNameValue(uint8_t* out, const wchar_t* name, uint8_t nameLen)
{
    auto* fn = reinterpret_cast<FILE_NAME_ATTR*>(out);
    memset(fn, 0, sizeof(FILE_NAME_ATTR));
    fn->parentDirRef   = 5;
    fn->realSize       = FragmentNtfsStructuredFixture::kStructuredFileSize;
    fn->allocatedSize  = 2 * kClusterBytes;
    fn->fileAttributes = 0x20;
    fn->nameLength     = nameLen;
    fn->nameType       = FN_NAMESPACE_WIN32;
    const uint32_t nameOff = static_cast<uint32_t>(sizeof(FILE_NAME_ATTR));
    memcpy(out + nameOff, name, nameLen * sizeof(wchar_t));
}

static void WriteMft0Record(std::vector<uint8_t>& image, uint64_t mftByteOffset)
{
    uint8_t rec[kRecordSize]{};
    WriteMftHeader(rec, MFT_FLAG_IN_USE, sizeof(MFT_RECORD_HEADER), 0);

    uint32_t off = sizeof(MFT_RECORD_HEADER);
    uint8_t runs[8]{};
    const size_t runsLen = EncodeDataRun(runs, kMftStartLcn, kMftRunClusters);
    off = AppendDataAttr(rec, off, kMftRunClusters * kClusterBytes, runs, runsLen);
    FinalizeMftRecord(rec, off);
    memcpy(image.data() + mftByteOffset, rec, kRecordSize);
}

static void WriteDeletedFileRecord(std::vector<uint8_t>& image,
                                   uint64_t mftByteOffset,
                                   FragmentNtfsStructuredFixture::NtfsChainMode mode)
{
    uint8_t rec[kRecordSize]{};
    WriteMftHeader(rec, 0, sizeof(MFT_RECORD_HEADER), kDeletedRec);

    uint32_t off = sizeof(MFT_RECORD_HEADER);

    uint8_t fnBuf[128]{};
    const wchar_t kName[] = L"PHOENIX.JPG";
    constexpr uint8_t kNameLen = 10;
    WriteFileNameValue(fnBuf, kName, kNameLen);
    const uint32_t fnTotal =
        static_cast<uint32_t>(sizeof(FILE_NAME_ATTR) + kNameLen * sizeof(wchar_t));
    off = AppendResidentAttr(rec, off, ATTR_FILE_NAME, fnBuf, fnTotal);

    uint8_t runs[8]{};
    size_t runsLen = 0;
    if (mode == FragmentNtfsStructuredFixture::NtfsChainMode::Intact) {
        runsLen = EncodeDataRun(runs, kDataLcn0, 2);
    } else {
        runsLen = EncodeDataRun(runs, kDataLcn0, 1);
    }

    off = AppendDataAttr(rec, off, FragmentNtfsStructuredFixture::kStructuredFileSize, runs, runsLen);
    FinalizeMftRecord(rec, off);

    memcpy(image.data() + mftByteOffset + static_cast<uint64_t>(kDeletedRec) * kRecordSize,
           rec,
           kRecordSize);
}

static void WriteBitmapRecord(std::vector<uint8_t>& image,
                              uint64_t mftByteOffset,
                              const std::vector<uint8_t>& bitmap)
{
    uint8_t rec[kRecordSize]{};
    WriteMftHeader(rec, MFT_FLAG_IN_USE, sizeof(MFT_RECORD_HEADER), 6);

    uint32_t off = sizeof(MFT_RECORD_HEADER);

    uint8_t runs[8]{};
    const size_t runsLen = EncodeDataRun(runs, kBitmapLcn, 1);
    off = AppendDataAttr(rec, off, static_cast<uint64_t>(bitmap.size()), runs, runsLen);
    FinalizeMftRecord(rec, off);

    memcpy(image.data() + mftByteOffset + 6 * kRecordSize, rec, kRecordSize);
    memcpy(image.data() + ClusterOffset(kBitmapLcn), bitmap.data(), bitmap.size());
}

static void WriteBootSector(std::vector<uint8_t>& image)
{
    uint8_t* boot = image.data();
    memcpy(boot + 3, NTFS_OEM_ID, 8);
    WriteLe16(boot + 11, kSectorSize);
    boot[13] = static_cast<uint8_t>(kSpc);
    WriteLe64(boot + 40, kTotalSectors);
    WriteLe64(boot + 48, kMftStartLcn);
    WriteLe64(boot + 56, kMftStartLcn + 1);
    boot[64] = static_cast<uint8_t>(-10);
    boot[510] = 0x55;
    boot[511] = 0xAA;
}

} // namespace

namespace FragmentNtfsStructuredFixture {

const wchar_t* FormatExtension(StructuredFormat format)
{
    switch (format) {
    case StructuredFormat::Jpeg:
        return L"jpg";
    }
    return L"";
}

void BuildNtfsStructuredVolume(std::vector<uint8_t>& image,
                               StructuredFormat format,
                               NtfsChainMode mode)
{
    (void)format;
    image.assign(static_cast<size_t>(kTotalSectors) * kSectorSize, 0);

    WriteBootSector(image);

    const uint64_t mftByteOffset = ClusterOffset(kMftStartLcn);

    std::vector<uint8_t> bitmap(static_cast<size_t>(kDecoyLcn / 8 + 8), 0);
    SetBitmapBit(bitmap, kDataLcn0, true);
    if (mode == FragmentNtfsStructuredFixture::NtfsChainMode::Intact) {
        SetBitmapBit(bitmap, kDataLcn0 + 1, true);
    }
    SetBitmapBit(bitmap, kBitmapLcn, true);

    // No record 0 $MFT datarun: NtfsParser uses sequential MFT scan (covers record 40).
    WriteMft0Record(image, mftByteOffset);
    WriteBitmapRecord(image, mftByteOffset, bitmap);
    WriteDeletedFileRecord(image, mftByteOffset, mode);

    WriteJpegClusters(image, kDataLcn0, kDataLcn0 + 1);
    if (mode == FragmentNtfsStructuredFixture::NtfsChainMode::BrokenBraid) {
        memset(image.data() + ClusterOffset(kDecoyLcn), 0, kClusterBytes);
    }
}

} // namespace FragmentNtfsStructuredFixture
