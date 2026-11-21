#include "NtfsParser.h"
#include "../../common/Utils.h"
#include "UsnJournalParser.h"
#include "../../common/Utils.h"
#include "../../common/Constants.h"
#include "../../log/Logger.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <sstream>
#include <unordered_set>

using namespace Constants;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: format a hex value as wstring
// ─────────────────────────────────────────────────────────────────────────────
static std::wstring ToHex(uint64_t v)
{
    wchar_t buf[32];
    swprintf_s(buf, L"0x%llX", static_cast<unsigned long long>(v));
    return buf;
}
static std::wstring ToHex32(uint32_t v)
{
    wchar_t buf[16];
    swprintf_s(buf, L"0x%08X", v);
    return buf;
}
static std::wstring FmtPct(double v)
{
    wchar_t buf[16];
    swprintf_s(buf, L"%.1f%%", v * 100.0);
    return buf;
}

static std::wstring ReadAttrName(const uint8_t* attr, const ATTR_HEADER_COMMON* ah)
{
    if (!ah || ah->nameLength == 0)
        return L"";
    if (ah->nameOffset + ah->nameLength * 2u > ah->length)
        return L"";
    const wchar_t* namePtr =
        reinterpret_cast<const wchar_t*>(attr + ah->nameOffset);
    return std::wstring(namePtr, ah->nameLength);
}

static bool MftRecordByteOffset(uint64_t recordIndex,
                                uint64_t mftByteOffset,
                                uint64_t partStartBytes,
                                uint64_t bytesPerCluster,
                                uint32_t recordSize,
                                const std::vector<uint64_t>& mftClusters,
                                uint64_t& outOffset)
{
    if (recordSize == 0)
        return false;

    if (!mftClusters.empty()) {
        const uint64_t recordsPerCluster = bytesPerCluster / recordSize;
        if (recordsPerCluster > 0) {
            const uint64_t clusterIndex = recordIndex / recordsPerCluster;
            const uint64_t recordInCluster = recordIndex % recordsPerCluster;
            if (clusterIndex >= mftClusters.size())
                return false;
            const uint64_t lcn = mftClusters[clusterIndex];
            outOffset = partStartBytes + lcn * bytesPerCluster +
                        recordInCluster * recordSize;
            return true;
        }
    }

    outOffset = mftByteOffset + recordIndex * recordSize;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// $ATTRIBUTE_LIST (0x20) support (P0-7).
//
// NTFS moves attributes into EXTENSION MFT records when a base record runs out
// of room (typically a heavily fragmented file whose $DATA has many runs).
// The base record then holds a resident $ATTRIBUTE_LIST whose entries point at
// those records by MFT reference + lowestVCN. Before P0-7 the parser walked
// only the base record, so such files showed a declared size (stale
// $FILE_NAME.realSize) but ZERO clusters, and RecoverEngine correctly refused
// them with "No recoverable data". Found on the real LDM disk: MFT record 78,
// 56,725,784 bytes spread over extension records 10902/11004/10737.
// ─────────────────────────────────────────────────────────────────────────────
struct AttrListDataRef
{
    uint64_t lowestVcn;
    uint64_t recordIndex;
    uint16_t sequence;
};

template <typename T>
static T ReadUnaligned(const uint8_t* p)
{
    T v{};
    std::memcpy(&v, p, sizeof(T));
    return v;
}

static bool CollectAttrListDataRefs(const uint8_t* value, size_t len,
                                    std::vector<AttrListDataRef>& refs)
{
    constexpr size_t kEntryHeader = 26;   // type..instance (before the name)
    size_t off = 0;
    while (off + kEntryHeader <= len) {
        const uint32_t type   = ReadUnaligned<uint32_t>(value + off);
        const uint16_t recLen = ReadUnaligned<uint16_t>(value + off + 4);
        const uint8_t  nameLen = value[off + 6];
        if (recLen < kEntryHeader || off + recLen > len) break;
        // Only the DEFAULT (unnamed) $DATA stream is the file payload.
        // Named $DATA entries are alternate data streams, handled elsewhere.
        if (type == ATTR_DATA && nameLen == 0) {
            AttrListDataRef r{};
            r.lowestVcn   = ReadUnaligned<uint64_t>(value + off + 8);
            const uint64_t ref = ReadUnaligned<uint64_t>(value + off + 16);
            r.recordIndex = ref & 0x0000FFFFFFFFFFFFULL;
            r.sequence    = static_cast<uint16_t>(ref >> 48);
            refs.push_back(r);
        }
        off += recLen;
    }
    std::sort(refs.begin(), refs.end(),
              [](const AttrListDataRef& a, const AttrListDataRef& b) {
                  return a.lowestVcn < b.lowestVcn;
              });
    return !refs.empty();
}

static bool NtfsPerNodeLogEnabled()
{
    static int cached = -1;
    if (cached >= 0) {
        return cached != 0;
    }
    const std::wstring value = Utils::GetEnvironmentVariableW(L"PHOENIX_LOG_NTFS_NODES");
    if (value.empty()) {
        cached = 0;
        return false;
    }
    cached = (value == L"1" || value == L"true" || value == L"TRUE" || value == L"on" || value == L"ON") ? 1 : 0;
    return cached != 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Open – validate boot sector
// ─────────────────────────────────────────────────────────────────────────────
bool NtfsParser::Open(SectorReader& reader, const Partition& partition)
{
    m_reader          = &reader;
    m_part            = partition;
    m_valid           = false;
    m_files.clear();
    m_recordMap.clear();
    m_mftClusters.clear();
    m_bitmap.clear();
    m_bitmapLoaded    = false;
    m_totalRecords    = 0;
    m_inUseCount      = 0;

    LOG_INFO(L"NtfsParser::Open"
             L"  startSector="  + std::to_wstring(partition.startSector) +
             L"  size="         + std::to_wstring(partition.sizeBytes) + L" bytes"
             L"  sectorSize="   + std::to_wstring(reader.GetSectorSize()));

    if (!ReadBootSector()) return false;

    m_bytesPerCluster = m_boot.bytesPerSector;
    m_bytesPerCluster *= m_boot.sectorsPerCluster;

    if (m_boot.clustersPerRecord < 0)
        m_recordSize = 1u << static_cast<uint32_t>(-m_boot.clustersPerRecord);
    else
        m_recordSize = static_cast<uint32_t>(m_boot.clustersPerRecord)
                       * static_cast<uint32_t>(m_bytesPerCluster);

    if (m_recordSize == 0 || m_recordSize > 65536) {
        LOG_ERROR(L"NtfsParser: invalid MFT record size " +
                  std::to_wstring(m_recordSize));
        return false;
    }

    m_partStartBytes = partition.startSector * reader.GetSectorSize();
    m_mftByteOffset  = m_partStartBytes +
                       m_boot.mftStartCluster * m_bytesPerCluster;

    LOG_INFO(L"NtfsParser geometry:"
             L"  bytesPerSector="    + std::to_wstring(m_boot.bytesPerSector) +
             L"  sectorsPerCluster=" + std::to_wstring(m_boot.sectorsPerCluster) +
             L"  bytesPerCluster="   + std::to_wstring(m_bytesPerCluster) +
             L"  recordSize="        + std::to_wstring(m_recordSize) + L" bytes");

    LOG_INFO(L"NtfsParser MFT location:"
             L"  mftStartCluster=" + std::to_wstring(m_boot.mftStartCluster) +
             L"  mftByteOffset="   + std::to_wstring(m_mftByteOffset) +
             L"  partStartBytes="  + std::to_wstring(m_partStartBytes));

    LOG_DEBUG(L"NtfsParser BPB raw:"
              L"  totalSectors="     + std::to_wstring(m_boot.totalSectors) +
              L"  mftMirrorCluster=" + std::to_wstring(m_boot.mftMirrorCluster) +
              L"  volumeSerial="     + ToHex(m_boot.volumeSerialNumber));

    m_valid = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool NtfsParser::ReadBootSector()
{
    if (!m_reader || !m_reader->IsOpen()) {
        LOG_ERROR(L"NtfsParser::ReadBootSector – reader not open");
        return false;
    }

    uint32_t ss = m_reader->GetSectorSize();
    std::vector<uint8_t> buf(ss);

    LOG_DEBUG(L"NtfsParser: reading boot sector at startSector=" +
              std::to_wstring(m_part.startSector));

    if (!m_reader->ReadSectors(m_part.startSector, buf.data(), 1)) {
        LOG_ERROR(L"NtfsParser: I/O error reading boot sector (sector=" +
                  std::to_wstring(m_part.startSector) + L")");
        return false;
    }

    memcpy(&m_boot, buf.data(), sizeof(m_boot));

    // Log raw OEM ID
    wchar_t woemId[9] = {};
    for (int i = 0; i < 8; ++i) woemId[i] = static_cast<wchar_t>(
        static_cast<unsigned char>(m_boot.oemId[i]));
    LOG_DEBUG(L"NtfsParser: OEM ID = [" + std::wstring(woemId) + L"]");

    if (memcmp(m_boot.oemId, "NTFS    ", 8) != 0) {
        LOG_WARN(L"NtfsParser: OEM ID is not 'NTFS    ' – not an NTFS partition");
        return false;
    }
    if (m_boot.bytesPerSector == 0 || m_boot.sectorsPerCluster == 0) {
        LOG_ERROR(L"NtfsParser: invalid BPB"
                  L"  bytesPerSector=" + std::to_wstring(m_boot.bytesPerSector) +
                  L"  sectorsPerCluster=" + std::to_wstring(m_boot.sectorsPerCluster));
        return false;
    }

    LOG_DEBUG(L"NtfsParser: boot sector OK");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// TryReadMftRecord0 – attempt to read and validate MFT record 0 from
// the given byte offset.  Returns true and fills recBuf on success.
// ─────────────────────────────────────────────────────────────────────────────
bool NtfsParser::TryReadMftRecord0(uint64_t byteOffset, std::vector<uint8_t>& recBuf)
{
    if (!m_reader->ReadBytes(byteOffset, recBuf.data(), m_recordSize)) {
        LOG_DEBUG(L"NtfsParser::TryReadMftRecord0 – I/O error at offset="
                  + std::to_wstring(byteOffset));
        return false;
    }

    if (memcmp(recBuf.data(), NTFS_FILE_MAGIC, 4) != 0) {
        LOG_DEBUG(L"NtfsParser::TryReadMftRecord0 – bad magic at offset="
                  + std::to_wstring(byteOffset));
        return false;
    }

    if (!ApplyUSA(recBuf.data(), m_recordSize)) {
        LOG_WARN(L"NtfsParser::TryReadMftRecord0 – USA fix-up failed at offset="
                 + std::to_wstring(byteOffset) + L", continuing anyway");
    }

    auto* hdr = reinterpret_cast<const MFT_RECORD_HEADER*>(recBuf.data());
    return (hdr->bytesInUse > 0 && hdr->attrOffset >= sizeof(MFT_RECORD_HEADER));
}

// ─────────────────────────────────────────────────────────────────────────────
bool NtfsParser::ReadMftDataRuns()
{
    std::vector<uint8_t> recBuf(m_recordSize);
    bool usedMirror = false;

    LOG_DEBUG(L"NtfsParser: reading MFT record 0 at offset="
              + std::to_wstring(m_mftByteOffset));

    // ── Try primary $MFT location ─────────────────────────────────────────
    if (!TryReadMftRecord0(m_mftByteOffset, recBuf)) {
        // ── Fallback: $MFTMirr (contains a copy of records 0-3) ──────────
        uint64_t mirrorOffset = m_partStartBytes
                              + m_boot.mftMirrorCluster * m_bytesPerCluster;
        LOG_WARN(L"NtfsParser: primary MFT record 0 unreadable – trying $MFTMirr"
                 L"  mirrorOffset=" + std::to_wstring(mirrorOffset));

        if (!TryReadMftRecord0(mirrorOffset, recBuf)) {
            LOG_ERROR(L"NtfsParser: both primary MFT and $MFTMirr failed");
            return false;
        }
        usedMirror = true;
        LOG_INFO(L"NtfsParser: successfully read MFT record 0 from $MFTMirr");
    }

    auto* hdr = reinterpret_cast<MFT_RECORD_HEADER*>(recBuf.data());
    (void)usedMirror;

    LOG_DEBUG(L"NtfsParser: MFT record 0 header:"
              L"  flags="      + ToHex32(hdr->flags) +
              L"  attrOffset=" + std::to_wstring(hdr->attrOffset) +
              L"  bytesInUse=" + std::to_wstring(hdr->bytesInUse));

    const uint8_t* p   = recBuf.data() + hdr->attrOffset;
    const uint8_t* end = recBuf.data() + m_recordSize;
    int attrCount = 0;

    while (p + sizeof(ATTR_HEADER_COMMON) <= end) {
        auto* ah = reinterpret_cast<const ATTR_HEADER_COMMON*>(p);
        if (ah->type == ATTR_END || ah->length == 0) break;
        ++attrCount;

        LOG_DEBUG(L"NtfsParser: MFT[0] attr #" + std::to_wstring(attrCount) +
                  L"  type=" + ToHex32(ah->type) +
                  L"  len="  + std::to_wstring(ah->length) +
                  L"  nonRes=" + std::to_wstring(ah->nonResident));

        if (ah->type == ATTR_DATA && ah->nonResident) {
            auto* nr = reinterpret_cast<const ATTR_NONRESIDENT*>(p);
            const uint8_t* runs = p + nr->dataRunOffset;
            m_mftClusters = DecodeDataRuns(runs, static_cast<size_t>(end - runs));

            LOG_INFO(L"NtfsParser: $MFT $DATA decoded"
                     L"  clusters="   + std::to_wstring(m_mftClusters.size()) +
                     L"  dataSize="   + std::to_wstring(nr->dataSize) +
                     L"  allocSize="  + std::to_wstring(nr->allocatedSize));

            if (!m_mftClusters.empty()) {
                LOG_DEBUG(L"NtfsParser: MFT first LCN=" +
                          std::to_wstring(m_mftClusters.front()) +
                          L"  last LCN=" +
                          std::to_wstring(m_mftClusters.back()));
            }
            break;
        }
        if (ah->length > static_cast<uint32_t>(end - p)) break;
        p += ah->length;
    }

    if (m_mftClusters.empty()) {
        LOG_ERROR(L"NtfsParser: could not find non-resident $DATA in MFT record 0");
        return false;
    }

    uint64_t recordsPerCluster = (m_recordSize > 0) ? m_bytesPerCluster / m_recordSize : 0;
    uint64_t estimatedRecords  = m_mftClusters.size() * recordsPerCluster;
    LOG_INFO(L"NtfsParser: estimated MFT records=" + std::to_wstring(estimatedRecords) +
             L"  (clusters=" + std::to_wstring(m_mftClusters.size()) +
             L"  recordsPerCluster=" + std::to_wstring(recordsPerCluster) + L")");

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ScanMetadata  –  OPTIMISED: bulk MFT reads (4 MB per I/O call)
//
// Old approach: ReadBytes(1 KB) per record → up to 3M kernel calls for 1M recs
// New approach: batch-read contiguous MFT data-runs (≤4 MB each) → ~1 K calls
// ─────────────────────────────────────────────────────────────────────────────
bool NtfsParser::ScanMetadata(std::function<void(int)> onProgress,
                               std::atomic<bool>*       cancelFlag)
{
    if (!m_valid) {
        LOG_ERROR(L"NtfsParser::ScanMetadata – parser not valid (Open failed)");
        return false;
    }

    LOG_INFO(L"NtfsParser::ScanMetadata – starting MFT scan (bulk I/O)");

    if (!ReadMftDataRuns()) {
        LOG_WARN(L"NtfsParser: could not decode MFT data runs, "
                 L"falling back to sequential read");
    }

    if (m_bytesPerCluster == 0 || m_recordSize == 0) {
        LOG_ERROR(L"NtfsParser::ScanMetadata – invalid geometry");
        return false;
    }

    const uint32_t sectorSize = m_reader->GetSectorSize();
    if (sectorSize == 0) {
        LOG_ERROR(L"NtfsParser::ScanMetadata – sector size is zero");
        return false;
    }

    const uint64_t totalClusters    = m_mftClusters.size();
    const uint64_t recordsPerCluster =
        (totalClusters > 0 && m_recordSize > 0 && m_bytesPerCluster >= m_recordSize)
            ? (m_bytesPerCluster / m_recordSize) : 1;
    const uint64_t totalRecords =
        (totalClusters > 0)
            ? totalClusters * recordsPerCluster
            : (m_part.sizeBytes / m_recordSize);

    LOG_INFO(L"NtfsParser: scan plan"
             L"  mode=" + std::wstring(m_mftClusters.empty() ? L"sequential" : L"bulk-datarun") +
             L"  expectedRecords=" + std::to_wstring(totalRecords) +
             L"  recordsPerCluster=" + std::to_wstring(recordsPerCluster));

    // ── Pre-reserve record map (avoids repeated rehash) ──────────────────
    m_recordMap.reserve(static_cast<size_t>(totalRecords));

    uint64_t processed = 0;
    uint64_t magicFail = 0;
    uint64_t usaFail   = 0;
    int      lastPct   = -1;

    auto reportProgress = [&]() {
        if (!onProgress) return;
        int pct = (totalRecords > 0)
                  ? static_cast<int>(processed * 100 / totalRecords)
                  : 0;
        pct = std::min(pct, 99);
        if (pct != lastPct) { onProgress(pct); lastPct = pct; }
    };

    // ── Single reusable buffer for USA fix-up (avoids per-record allocation)
    std::vector<uint8_t> recCopy(m_recordSize);

    // Inline record processor – takes a pointer into already-loaded memory
    auto processFromMem = [&](const uint8_t* rawRec, uint64_t recordIndex)
    {
        if (memcmp(rawRec, NTFS_FILE_MAGIC, 4) != 0) { ++magicFail; return; }
        memcpy(recCopy.data(), rawRec, m_recordSize);    // copy for in-place USA patch
        if (!ApplyUSA(recCopy.data(), m_recordSize))    { ++usaFail;   return; }
        ParseRecord(recCopy.data(), recordIndex);
    };

    if (!m_mftClusters.empty())
    {
        // ── Bulk-read mode ────────────────────────────────────────────────
        // Read up to BATCH_BYTES of contiguous MFT clusters per ReadSectors call.
        // MFT data-runs are usually highly contiguous, so most batches will be
        // large and sequential – ideal for both HDDs and SSDs.
        constexpr size_t BATCH_BYTES      = 4 * 1024 * 1024; // 4 MB
        const     size_t maxBatchClusters = BATCH_BYTES / static_cast<size_t>(m_bytesPerCluster);
        const     size_t sectorSize       = m_reader->GetSectorSize();
        if (maxBatchClusters == 0) {
            LOG_ERROR(L"NtfsParser: bytesPerCluster too large for bulk scan");
            return false;
        }

        std::vector<uint8_t> batchBuf;
        batchBuf.reserve(BATCH_BYTES);

        uint64_t recordIndex = 0;
        size_t   ci          = 0;                   // index into m_mftClusters
        const size_t nClusters = m_mftClusters.size();

        while (ci < nClusters)
        {
            if (cancelFlag && cancelFlag->load()) break;

            // ── Find the longest contiguous run starting at ci ────────────
            size_t runLen = 1;
            while (runLen < nClusters - ci &&
                   runLen < maxBatchClusters &&
                   m_mftClusters[ci + runLen] == m_mftClusters[ci + runLen - 1] + 1)
            {
                ++runLen;
            }

            // ── Issue a single bulk ReadSectors call for this run ─────────
            const size_t batchBytes   = runLen * static_cast<size_t>(m_bytesPerCluster);
            const uint64_t startLcn   = m_mftClusters[ci];
            const uint64_t lba        = (m_partStartBytes / sectorSize)
                                      + startLcn * (m_bytesPerCluster / sectorSize);
            const uint32_t sectors    = static_cast<uint32_t>(batchBytes / sectorSize);

            batchBuf.resize(batchBytes);

            if (m_reader->ReadSectors(lba, batchBuf.data(), sectors))
            {
                // Parse all records from the in-memory buffer (no more I/O)
                for (size_t off = 0; off + m_recordSize <= batchBytes; off += m_recordSize)
                {
                    processFromMem(batchBuf.data() + off, recordIndex);
                    ++recordIndex;
                    ++processed;
                }
            }
            else
            {
                // Bulk read failed (rare on healthy disk) – skip this batch
                LOG_WARN(L"NtfsParser: bulk read failed at LCN="
                         + std::to_wstring(startLcn)
                         + L"  runLen=" + std::to_wstring(runLen)
                         + L"  skipping "
                         + std::to_wstring(batchBytes / m_recordSize) + L" records");
                recordIndex += batchBytes / m_recordSize;
                processed   += batchBytes / m_recordSize;
            }

            ci += runLen;
            reportProgress();
        }
    }
    else
    {
        // ── Sequential fallback (no MFT data-runs available) ──────────────
        // Still reads 4 MB at a time for efficiency.
        LOG_WARN(L"NtfsParser: no MFT data-runs – using sequential bulk scan");

        constexpr size_t BATCH_BYTES = 4 * 1024 * 1024;
        const     size_t sectorSize  = m_reader->GetSectorSize();
        const     size_t sectors_per_batch = BATCH_BYTES / sectorSize;

        std::vector<uint8_t> batchBuf(BATCH_BYTES);

        const uint64_t startLba   = m_mftByteOffset / sectorSize;
        const uint64_t partEndLba = (m_partStartBytes + m_part.sizeBytes) / sectorSize;
        uint64_t       curLba     = startLba;
        uint64_t       recordIndex = 0;

        while (curLba < partEndLba)
        {
            if (cancelFlag && cancelFlag->load()) break;

            uint32_t toRead = static_cast<uint32_t>(
                std::min<uint64_t>(sectors_per_batch, partEndLba - curLba));

            if (!m_reader->ReadSectors(curLba, batchBuf.data(), toRead))
                break;

            const size_t batchBytes = static_cast<size_t>(toRead) * sectorSize;
            for (size_t off = 0; off + m_recordSize <= batchBytes; off += m_recordSize)
            {
                processFromMem(batchBuf.data() + off, recordIndex);
                ++recordIndex;
                ++processed;
            }

            curLba += toRead;
            reportProgress();
        }
    }

    BuildFullPaths();

    // Parse $Recycle.Bin metadata ($I files) to enhance deleted file information
    uint32_t recycleBinUpdated = ParseRecycleBinMetadata();
    if (recycleBinUpdated > 0) {
        LOG_INFO(L"NtfsParser: $Recycle.Bin metadata parsed"
                 L"  files updated=" + std::to_wstring(recycleBinUpdated));
    }

    if (onProgress) onProgress(100);

    LOG_INFO(L"NtfsParser::ScanMetadata complete:");
    LOG_INFO(L"  Records processed : " + std::to_wstring(processed));
    LOG_INFO(L"  Magic mismatch    : " + std::to_wstring(magicFail));
    LOG_INFO(L"  USA fix-up fail   : " + std::to_wstring(usaFail));
    LOG_INFO(L"  Valid records(≥16): " + std::to_wstring(m_totalRecords));
    LOG_INFO(L"  In-use files      : " + std::to_wstring(m_inUseCount));
    LOG_INFO(L"  Total nodes       : " + std::to_wstring(m_files.size()));
    LOG_INFO(L"  Path map entries  : " + std::to_wstring(m_recordMap.size()));

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool NtfsParser::ApplyUSA(uint8_t* recBuf, size_t recSize)
{
    auto* hdr = reinterpret_cast<MFT_RECORD_HEADER*>(recBuf);
    if (hdr->usaOffset == 0 || hdr->usaCount < 2) return true;

    const uint8_t* usaBase  = recBuf + hdr->usaOffset;
    uint16_t usaValue        = Utils::ReadLE16(usaBase);
    uint32_t sectorSize      = 512;
    uint32_t numSectors      = static_cast<uint32_t>(recSize) / sectorSize;

    if (static_cast<uint32_t>(hdr->usaCount - 1) != numSectors) return false;

    for (uint32_t i = 0; i < numSectors; ++i) {
        uint8_t* sectorEnd = recBuf + (i + 1) * sectorSize - 2;
        if (Utils::ReadLE16(sectorEnd) != usaValue) return false;
        sectorEnd[0] = usaBase[(i + 1) * 2];
        sectorEnd[1] = usaBase[(i + 1) * 2 + 1];
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool NtfsParser::ParseRecord(const uint8_t* recBuf, uint64_t recordIndex)
{
    auto* hdr = reinterpret_cast<const MFT_RECORD_HEADER*>(recBuf);

    if (hdr->bytesInUse == 0 || hdr->bytesAllocated == 0) return false;
    if (hdr->attrOffset < sizeof(MFT_RECORD_HEADER))       return false;

    // Skip NTFS system meta-files 0-15
    if (recordIndex < 16) return false;

    bool isDeleted = !(hdr->flags & MFT_FLAG_IN_USE);
    bool isDir     = (hdr->flags & MFT_FLAG_DIRECTORY) != 0;

    ++m_totalRecords;
    if (!isDeleted) ++m_inUseCount;

    FileNode node;
    node.id          = recordIndex;
    node.isDirectory = isDir;
    node.status      = isDeleted ? FileStatus::Deleted : FileStatus::Normal;
    node.fsType      = L"NTFS";

    const uint8_t* p   = recBuf + hdr->attrOffset;
    const uint8_t* end = recBuf + hdr->bytesInUse;

    bool hasFileName = false;
    bool hasData     = false;
    std::vector<FileNode> adsNodes;
    // P0-7: resident $ATTRIBUTE_LIST value, resolved after $FILE_NAME parsing.
    std::vector<uint8_t> attrListValue;

    while (p + sizeof(ATTR_HEADER_COMMON) <= end) {
        auto* ah = reinterpret_cast<const ATTR_HEADER_COMMON*>(p);
        if (ah->type == ATTR_END) break;
        if (ah->length < sizeof(ATTR_HEADER_COMMON) || ah->length > 65536) break;

        if (ah->type == ATTR_STANDARD_INFORMATION && !ah->nonResident) {
            // $STANDARD_INFORMATION carries the most accurate timestamps
            auto* res = reinterpret_cast<const ATTR_RESIDENT*>(p);
            if (res->valueLength >= sizeof(STANDARD_INFORMATION) &&
                res->valueOffset + res->valueLength <= ah->length)
            {
                auto* si = reinterpret_cast<const STANDARD_INFORMATION*>(
                               p + res->valueOffset);
                node.createTime = si->createTime;
                node.modifyTime = si->modifyTime;
                node.accessTime = si->accessTime;
            }
        } else if (ah->type == ATTR_FILE_NAME && !ah->nonResident) {
            auto* res  = reinterpret_cast<const ATTR_RESIDENT*>(p);
            const uint8_t* val = p + res->valueOffset;
            size_t         len = res->valueLength;
            if (ParseFileName(val, len, node))
                hasFileName = true;
        } else if (ah->type == ATTR_ATTRIBUTE_LIST && !ah->nonResident) {
            // P0-7: remember the list; it is only followed when the base record
            // itself has NO unnamed $DATA (the usual NTFS extension layout).
            auto* res = reinterpret_cast<const ATTR_RESIDENT*>(p);
            const size_t voff = res->valueOffset;
            const size_t vlen = res->valueLength;
            if (vlen > 0 && voff >= sizeof(ATTR_RESIDENT) &&
                voff + vlen <= ah->length && p + voff + vlen <= end)
            {
                attrListValue.assign(p + voff, p + voff + vlen);
            }
        } else if (ah->type == ATTR_DATA) {
            const std::wstring streamName = ReadAttrName(p, ah);
            if (streamName.empty()) {
                if (!hasData && ParseDataAttr(ah, recordIndex, node))
                    hasData = true;
            } else if (hasFileName) {
                FileNode adsNode = node;
                adsNode.clusters.clear();
                adsNode.residentData.clear();
                adsNode.size             = 0;
                adsNode.rawOffset        = 0;
                adsNode.rawSize          = 0;
                adsNode.isNtfsCompressed = false;
                adsNode.compressionUnit  = 0;
                if (ParseDataAttr(ah, recordIndex, adsNode)) {
                    adsNode.name = node.name + L":" + streamName;
                    adsNodes.push_back(std::move(adsNode));
                }
            }
        }

        p += ah->length;
    }

    if (!hasFileName) {
        LOG_DEBUG(L"NtfsParser: rec=" + std::to_wstring(recordIndex) +
                  L"  no $FILE_NAME → skip"
                  L"  flags=" + ToHex32(hdr->flags));
        return false;
    }

    // ── P0-7: resolve $DATA from $ATTRIBUTE_LIST extension records ────────
    // Only when the base record has no default $DATA of its own. Entries are
    // sorted by lowestVCN, so out-of-order lists still reconstruct the correct
    // cluster order; gaps between segments become sparse holes (LCN 0), which
    // RecoverEngine already understands.
    if (!hasData && !attrListValue.empty()) {
        std::vector<AttrListDataRef> refs;
        if (CollectAttrListDataRefs(attrListValue.data(),
                                    attrListValue.size(), refs))
        {
            constexpr uint64_t kMaxClusters = 1ULL << 22;  // 4M (~16 GiB @4K)
            std::vector<uint64_t> merged;
            bool gotAuthoritativeSize = false;
            for (const auto& ref : refs) {
                if (ref.lowestVcn > kMaxClusters) break;
                uint64_t extOff = 0;
                if (!MftRecordByteOffset(ref.recordIndex, m_mftByteOffset,
                                         m_partStartBytes, m_bytesPerCluster,
                                         m_recordSize, m_mftClusters, extOff))
                    continue;
                std::vector<uint8_t> extRec(m_recordSize, 0);
                if (!m_reader->ReadBytes(extOff, extRec.data(), m_recordSize))
                    continue;
                if (memcmp(extRec.data(), NTFS_FILE_MAGIC, 4) != 0) continue;
                if (!ApplyUSA(extRec.data(), m_recordSize)) continue;

                auto* eh = reinterpret_cast<const MFT_RECORD_HEADER*>(extRec.data());
                if (eh->attrOffset < sizeof(MFT_RECORD_HEADER) ||
                    eh->attrOffset >= m_recordSize) continue;
                const uint8_t* ep   = extRec.data() + eh->attrOffset;
                const size_t   eInUse = eh->bytesInUse
                                      ? std::min<size_t>(eh->bytesInUse,
                                                         m_recordSize)
                                      : static_cast<size_t>(m_recordSize);
                const uint8_t* eEnd = extRec.data() + eInUse;
                while (ep + sizeof(ATTR_HEADER_COMMON) <= eEnd) {
                    auto* eah = reinterpret_cast<const ATTR_HEADER_COMMON*>(ep);
                    if (eah->type == ATTR_END) break;
                    if (eah->length < sizeof(ATTR_HEADER_COMMON) ||
                        ep + eah->length > eEnd) break;
                    if (eah->type == ATTR_DATA && eah->nameLength == 0) {
                        FileNode segment;
                        if (ParseDataAttr(eah, ref.recordIndex, segment)) {
                            if (!segment.clusters.empty()) {
                                const size_t need = static_cast<size_t>(
                                    ref.lowestVcn + segment.clusters.size());
                                if (need <= kMaxClusters) {
                                    if (merged.size() < need)
                                        merged.resize(need, 0);  // sparse gap
                                    for (size_t i = 0;
                                         i < segment.clusters.size(); ++i)
                                        merged[static_cast<size_t>(ref.lowestVcn)
                                               + i] = segment.clusters[i];
                                }
                            }
                            // realSize/dataSize is only authoritative on the
                            // VCN0 segment; later segments commonly repeat 0.
                            if (!gotAuthoritativeSize && ref.lowestVcn == 0 &&
                                segment.size > 0)
                            {
                                node.size = segment.size;
                                gotAuthoritativeSize = true;
                            }
                            if (segment.isNtfsCompressed) {
                                node.isNtfsCompressed = true;
                                node.compressionUnit  = segment.compressionUnit;
                            }
                            if (!segment.residentData.empty() &&
                                ref.lowestVcn == 0)
                                node.residentData = segment.residentData;
                        }
                        break;   // one unnamed $DATA per extension record
                    }
                    ep += eah->length;
                }
            }
            if (!merged.empty()) {
                node.clusters = std::move(merged);
                hasData = true;
                if (NtfsPerNodeLogEnabled())
                    LOG_INFO(L"NtfsParser: rec=" +
                             std::to_wstring(recordIndex) +
                             L" $ATTRIBUTE_LIST resolved segments=" +
                             std::to_wstring(refs.size()) +
                             L" clusters=" +
                             std::to_wstring(node.clusters.size()));
            }
        }
    }

    // Build extension
    size_t dot = node.name.rfind(L'.');
    if (dot != std::wstring::npos) {
        node.extension = node.name.substr(dot + 1);
        for (auto& c : node.extension) c = static_cast<wchar_t>(towlower(c));
    }

    // Register in lookup map for path resolution (all valid non-system records)
    RecordInfo ri;
    ri.name     = node.name;
    ri.parentId = node.parentId;
    ri.isDir    = isDir;
    m_recordMap.emplace(recordIndex, std::move(ri));

    // Real-time path resolution: try to resolve path immediately
    // If parent chain is not yet available, we'll resolve it later in BuildFullPaths
    node.originalPath = ResolveFullPath(node.parentId, node.name);
    
    m_files.push_back(node);

    // Real-time callback: report node immediately when found (even if path is partial)
    if (m_onFileFound) {
        if (NtfsPerNodeLogEnabled()) {
            LOG_INFO(L"NtfsParser node"
                     L" state=" + std::wstring(isDeleted ? L"deleted" : L"normal") +
                     L" kind=" + std::wstring(isDir ? L"dir" : L"file") +
                     L" rec=" + std::to_wstring(recordIndex) +
                     L" id=" + std::to_wstring(static_cast<uint64_t>(node.id)) +
                     L" parentId=" + std::to_wstring(node.parentId) +
                     L" size=" + std::to_wstring(node.size) +
                     L" clusters=" + std::to_wstring(node.clusters.size()) +
                     L" name=[" + node.name + L"]"
                     L" path=[" + node.originalPath + L"]");
        }
        m_onFileFound(node);
    }

    for (size_t adsIdx = 0; adsIdx < adsNodes.size(); ++adsIdx) {
        FileNode& adsNode = adsNodes[adsIdx];
        adsNode.id          = (recordIndex << 4) | static_cast<uint64_t>(adsIdx + 1);
        adsNode.originalPath = ResolveFullPath(adsNode.parentId, adsNode.name);
        m_files.push_back(adsNode);
        if (m_onFileFound)
            m_onFileFound(adsNode);
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool NtfsParser::ParseFileName(const uint8_t* attrValue, size_t valueLen,
                               FileNode& out)
{
    if (valueLen < sizeof(FILE_NAME_ATTR)) return false;
    auto* fn = reinterpret_cast<const FILE_NAME_ATTR*>(attrValue);
    uint8_t nameLen = fn->nameLength;
    if (sizeof(FILE_NAME_ATTR) + nameLen * 2u > valueLen) return false;

    bool nameAlreadySet = !out.name.empty();
    if (nameAlreadySet && (fn->nameType == 0 || fn->nameType == 2))
        return true;

    const wchar_t* namePtr =
        reinterpret_cast<const wchar_t*>(attrValue + sizeof(FILE_NAME_ATTR));
    out.name = std::wstring(namePtr, nameLen);

    if (out.createTime == 0) {
        out.createTime = fn->createTime;
        out.modifyTime = fn->modifyTime;
        out.accessTime = fn->accessTime;
    }
    if (out.size == 0) out.size = fn->realSize;
    out.parentId = MftRefRecordNumber(fn->parentDirRef);

    return !out.name.empty();
}

// ─────────────────────────────────────────────────────────────────────────────
bool NtfsParser::ParseDataAttr(const ATTR_HEADER_COMMON* hdr,
                               uint64_t recordIndex, FileNode& out)
{
    auto* p = reinterpret_cast<const uint8_t*>(hdr);

    if (!hdr->nonResident) {
        auto* res = reinterpret_cast<const ATTR_RESIDENT*>(p);
        out.size  = res->valueLength;

        // Copy resident data bytes so RecoverEngine can write them directly
        // without any further disk I/O.  Bounds-check: value must fit inside
        // the attribute (hdr->length covers the whole attribute record).
        if (res->valueLength > 0 &&
            res->valueOffset >= sizeof(ATTR_RESIDENT) &&
            static_cast<uint32_t>(res->valueOffset) + res->valueLength <= hdr->length)
        {
            const uint8_t* data = p + res->valueOffset;
            out.residentData.assign(data, data + res->valueLength);
        }

        return true;
    }

    auto* nr = reinterpret_cast<const ATTR_NONRESIDENT*>(p);
    // $DATA.dataSize is the authoritative file size; always overwrite whatever
    // $FILE_NAME.realSize may have set (it can be stale or zero for deleted files).
    if (nr->dataSize > 0) out.size = nr->dataSize;

    // ── Detect NTFS transparent compression (ATTR_FLAG_COMPRESSED = 0x0001) ─
    // When set the DataRun list interleaves real LCN runs with sparse "holes"
    // at compression unit boundaries.  RecoverEngine will call LZNT1 to
    // decompress each compression unit.
    constexpr uint16_t ATTR_FLAG_COMPRESSED = 0x0001;
    if (hdr->flags & ATTR_FLAG_COMPRESSED) {
        out.isNtfsCompressed = true;
        // compressionUnit field in the non-resident header encodes log2(clusters).
        // 0 in the header → Windows default = 2^4 = 16 clusters.
        out.compressionUnit = (nr->compressionUnit > 0)
                              ? (1u << nr->compressionUnit)
                              : 16u;
    }

    const uint8_t* runs = p + nr->dataRunOffset;
    size_t maxRunBytes = (hdr->length > nr->dataRunOffset)
                         ? hdr->length - nr->dataRunOffset : 0;

    out.clusters = DecodeDataRuns(runs, maxRunBytes);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
void NtfsParser::BuildFullPaths()
{
    LOG_INFO(L"NtfsParser::BuildFullPaths – re-resolving paths for " +
             std::to_wstring(m_files.size()) + L" nodes");

    uint32_t resolved = 0, partial = 0, updated = 0;
    for (auto& node : m_files) {
        // Re-resolve path now that all records are in m_recordMap
        // This may improve paths that were partial during real-time scanning
        std::wstring newPath = ResolveFullPath(node.parentId, node.name);
        
        bool wasPartial = node.originalPath.find(L"<") != std::wstring::npos;
        bool isPartial = newPath.find(L"<") != std::wstring::npos;
        bool pathImproved = wasPartial && !isPartial;  // improved from partial to full
        bool pathChanged = (newPath != node.originalPath);
        
        if (pathChanged) {
            // Path changed (improved or different) – update
            node.originalPath = std::move(newPath);
            if (pathImproved) {
                ++updated;
                // Notify callback of improved path (if callback is set)
                if (m_onFileFound) {
                    m_onFileFound(node);
                }
            }
        }

        if (isPartial) ++partial; else ++resolved;

        LOG_DEBUG(L"NtfsParser:   rec=" + std::to_wstring(node.id) +
                  L"  path=[" + node.originalPath + L"]" +
                  (isPartial ? L"  [PARTIAL]" : L"  [FULL]") +
                  (pathImproved ? L"  [IMPROVED]" : L""));
    }

    LOG_INFO(L"NtfsParser::BuildFullPaths done"
             L"  full=" + std::to_wstring(resolved) +
             L"  partial=" + std::to_wstring(partial) +
             L"  updated=" + std::to_wstring(updated));
}

std::wstring NtfsParser::ResolveFullPath(uint64_t parentId,
                                          const std::wstring& fileName) const
{
    std::vector<std::wstring> parts;
    uint64_t current = parentId;

    for (int depth = 0; depth < 32; ++depth) {
        if (current <= 5) break;  // 0-5 are NTFS root / meta-files

        auto it = m_recordMap.find(current);
        if (it == m_recordMap.end()) {
            // 常见：父 MFT 尚未收录或孤儿引用；勿逐条 DEBUG，否则单次扫描可写日志逾十万行。
            parts.push_back(L"<" + std::to_wstring(current) + L"?>");
            break;
        }

        parts.push_back(it->second.name);
        current = it->second.parentId;
    }

    // Prefix with drive letter (e.g. "C:\") when available, else just "\"
    std::wstring path = m_part.driveLetter.empty()
                        ? L"\\"
                        : (m_part.driveLetter + L"\\");

    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        path += *it;
        path += L"\\";
    }
    path += fileName;
    return path;
}

// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint64_t> NtfsParser::DecodeDataRuns(const uint8_t* p,
                                                  size_t maxBytes)
{
    std::vector<uint64_t> result;
    if (!p || maxBytes == 0) return result;

    const uint8_t* end = p + maxBytes;
    int64_t currentLcn = 0;

    while (p < end) {
        uint8_t header = *p++;
        if (header == 0x00) break;

        int lenFieldSize = header & 0x0F;
        int offFieldSize = (header >> 4) & 0x0F;
        if (lenFieldSize == 0) break;
        if (p + lenFieldSize + offFieldSize > end) break;

        uint64_t runLen = 0;
        for (int i = 0; i < lenFieldSize; ++i)
            runLen |= static_cast<uint64_t>(p[i]) << (i * 8);
        p += lenFieldSize;

        if (offFieldSize == 0) {
            for (uint64_t k = 0; k < runLen; ++k)
                result.push_back(0);
            continue;
        }

        int64_t delta = Utils::ReadSignedLE(p, offFieldSize);
        p += offFieldSize;
        currentLcn += delta;
        if (currentLcn < 0) break;

        for (uint64_t k = 0; k < runLen; ++k)
            result.push_back(static_cast<uint64_t>(currentLcn + k));
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
RecoverabilityLevel NtfsParser::AssessRecoverability(const FileNode& node)
{
    // Resident file: data lives inside the MFT record – always fully intact.
    if (!node.residentData.empty())
        return RecoverabilityLevel::Excellent;

    // Zero-byte files or no location info at all
    if (node.clusters.empty()) {
        // USN-only record with no cluster map – could still be recoverable via
        // RAW carving, but we mark Unknown so the user sees it in the list.
        LOG_DEBUG(L"NtfsParser::Assess rec=" + std::to_wstring(node.id) +
                  L"  no clusters → Unknown");
        return RecoverabilityLevel::Unknown;
    }

    if (!m_bitmapLoaded) {
        m_bitmapLoaded = true;
        
        // 计算 MFT 记录 6 ($Bitmap) 的正确位置
        uint64_t bitmapOff = 0;
        if (!MftRecordByteOffset(6, m_mftByteOffset, m_partStartBytes, m_bytesPerCluster,
                                 m_recordSize, m_mftClusters, bitmapOff)) {
            bitmapOff = m_mftByteOffset + 6 * m_recordSize;
        }
        
        LOG_DEBUG(L"NtfsParser: loading $Bitmap from MFT record 6  offset=" +
                  std::to_wstring(bitmapOff));

        std::vector<uint8_t> recBuf(m_recordSize);
        if (!m_reader->ReadBytes(bitmapOff, recBuf.data(), m_recordSize)) {
            LOG_WARN(L"NtfsParser: I/O error reading $Bitmap MFT record");
            return RecoverabilityLevel::Unknown;
        }
        if (!ApplyUSA(recBuf.data(), m_recordSize)) {
            LOG_WARN(L"NtfsParser: USA fail on $Bitmap record");
            return RecoverabilityLevel::Unknown;
        }
        auto* hdr = reinterpret_cast<const MFT_RECORD_HEADER*>(recBuf.data());
        if (memcmp(hdr->magic, NTFS_FILE_MAGIC, 4) != 0) {
            LOG_WARN(L"NtfsParser: $Bitmap MFT record magic mismatch");
            return RecoverabilityLevel::Unknown;
        }

        const uint8_t* p   = recBuf.data() + hdr->attrOffset;
        const uint8_t* end = recBuf.data() + m_recordSize;
        while (p + sizeof(ATTR_HEADER_COMMON) <= end) {
            auto* ah = reinterpret_cast<const ATTR_HEADER_COMMON*>(p);
            if (ah->type == ATTR_END || ah->length == 0) break;
            if (ah->type == ATTR_DATA && ah->nonResident) {
                auto* nr = reinterpret_cast<const ATTR_NONRESIDENT*>(p);
                const uint8_t* runs = p + nr->dataRunOffset;
                size_t maxR = ah->length - nr->dataRunOffset;
                auto bitmapClusters = DecodeDataRuns(runs, maxR);
                for (uint64_t lcn : bitmapClusters) {
                    uint64_t off  = m_partStartBytes + lcn * m_bytesPerCluster;
                    size_t   prev = m_bitmap.size();
                    m_bitmap.resize(prev + static_cast<size_t>(m_bytesPerCluster));
                    if (!m_reader->ReadBytes(off, m_bitmap.data() + prev,
                                            static_cast<uint32_t>(m_bytesPerCluster)))
                        m_bitmap.resize(prev);
                }
                LOG_INFO(L"NtfsParser: $Bitmap loaded"
                         L"  bytes=" + std::to_wstring(m_bitmap.size()) +
                         L"  clusters=" + std::to_wstring(m_bitmap.size() * 8));
                break;
            }
            p += ah->length;
        }
    }

    if (m_bitmap.empty()) return RecoverabilityLevel::Unknown;

    uint64_t total = node.clusters.size();
    if (total == 0) return RecoverabilityLevel::Unknown;

    uint64_t allocated = 0;
    for (uint64_t lcn : node.clusters) {
        if (lcn == 0) continue;
        uint64_t byte = lcn / 8;
        uint8_t  bit  = static_cast<uint8_t>(lcn % 8);
        if (byte < m_bitmap.size() && (m_bitmap[byte] & (1u << bit)))
            ++allocated;
    }

    double ratio = static_cast<double>(allocated) / static_cast<double>(total);
    RecoverabilityLevel level =
        (ratio == 0.0)   ? RecoverabilityLevel::Excellent :
        (ratio <= 0.10)  ? RecoverabilityLevel::Good      :
        (ratio <= 0.50)  ? RecoverabilityLevel::Poor      :
                           RecoverabilityLevel::Lost;

    // If initially rated Excellent or Good, sample first cluster to check for zeroed data
    if (level == RecoverabilityLevel::Excellent || level == RecoverabilityLevel::Good) {
        uint64_t firstLcn = 0;
        for (uint64_t lcn : node.clusters) {
            if (lcn != 0) { firstLcn = lcn; break; }
        }
        if (firstLcn > 0) {
            uint64_t sampleOff = m_partStartBytes + firstLcn * m_bytesPerCluster;
            // Sample first 512 bytes of the cluster
            uint8_t sample[512]{};
            uint32_t sampleSize = (std::min)(512u, static_cast<uint32_t>(m_bytesPerCluster));
            if (m_reader->ReadBytes(sampleOff, sample, sampleSize)) {
                // Check if data is all zeros
                bool allZero = true;
                for (uint32_t i = 0; i < sampleSize; ++i) {
                    if (sample[i] != 0) { allZero = false; break; }
                }
                if (allZero) {
                    // Data has been zeroed out - mark as Lost
                    level = RecoverabilityLevel::Lost;
                    LOG_DEBUG(L"NtfsParser: Assess rec=" + std::to_wstring(node.id) +
                              L"  name=[" + node.name + L"]  data is zeroed → Lost");
                }
            }
        }
    }

    LOG_DEBUG(L"NtfsParser: Assess rec=" + std::to_wstring(node.id) +
              L"  name=[" + node.name + L"]"
              L"  clusters=" + std::to_wstring(total) +
              L"  overwritten=" + std::to_wstring(allocated) +
              L"  ratio=" + FmtPct(ratio) +
              L"  level=" + Utils::Utf8ToWide(RecoverabilityStr(level)));

    return level;
}

// ─────────────────────────────────────────────────────────────────────────────
// ScanUsnJournal  – Phase-2: read $UsnJrnl via FSCTL, find deleted nodes that
//                   are no longer visible in the MFT.
// ─────────────────────────────────────────────────────────────────────────────
uint32_t NtfsParser::ScanUsnJournal(std::function<void(int)> onProgress,
                                     const std::atomic<bool>* cancelFlag)
{
    if (m_part.driveLetter.empty())
    {
        LOG_DEBUG(L"NtfsParser::ScanUsnJournal – no drive letter, skipping");
        return 0;
    }

    // Build volume path, e.g.  "\\.\C:"
    std::wstring volPath = L"\\\\.\\" + m_part.driveLetter;
    LOG_INFO(L"NtfsParser::ScanUsnJournal ─────────────────────────────────");
    LOG_INFO(L"  Volume : " + volPath);

    // ── Collect FRNs already found by the MFT scan ────────────────────────
    std::unordered_set<uint64_t> knownFrns;
    knownFrns.reserve(m_files.size() * 2);
    for (const auto& f : m_files)
        knownFrns.insert(f.id);

    LOG_INFO(L"  Existing MFT nodes already found : " + std::to_wstring(knownFrns.size()));

    // ── Invoke UsnJournalParser ───────────────────────────────────────────
    UsnJournalParser usn;
    std::vector<UsnRecord> records;

    auto progressFn = [&](uint64_t done, uint64_t total)
    {
        if (onProgress && total > 0)
            onProgress(static_cast<int>(done * 100 / total));
    };

    if (!usn.Scan(volPath, records, progressFn, cancelFlag))
    {
        LOG_WARN(L"NtfsParser::ScanUsnJournal – " + usn.GetLastError());
        return 0;
    }

    LOG_INFO(L"  USN raw entries  : " + std::to_wstring(usn.GetRawRecordsRead()));
    LOG_INFO(L"  Unique FRNs      : " + std::to_wstring(usn.GetUniqueFrns()));

    // ── Filter: keep only entries whose last state is "deleted" ──────────
    // Decision logic:
    //   • allReasons must include FILE_DELETE  (was deleted at least once)
    //   • lastReason must NOT include FILE_CREATE or RENAME_NEW_NAME
    //     (would mean re-created/renamed-back after the delete)
    constexpr uint32_t kAliveReasons =
        UsnReason::FILE_CREATE_REASON | UsnReason::RENAME_NEW_NAME;

    uint32_t added = 0;

    for (const auto& rec : records)
    {
        // Must have been deleted at some point
        if (!(rec.allReasons & UsnReason::FILE_DELETE_REASON))
            continue;

        // Latest event indicates the file is still alive → skip
        if (rec.lastReason & kAliveReasons)
            continue;

        // Skip system files (FRN 0-15 are NTFS metadata)
        if (rec.fileFrn < 16)
            continue;

        // Skip if MFT scan already found this file
        if (knownFrns.count(rec.fileFrn))
            continue;

        if (rec.name.empty())
            continue;

        // ── Build FileNode ────────────────────────────────────────────────
        FileNode node;
        node.id        = rec.fileFrn;
        node.name      = rec.name;
        node.parentId  = rec.parentFrn;
        node.status    = FileStatus::Deleted;
        node.recoverability = RecoverabilityLevel::Unknown;
        node.fsType    = L"NTFS";

        // Windows FILETIME → stored directly (display layer converts)
        node.modifyTime  = static_cast<uint64_t>(rec.timestamp);
        node.createTime  = node.modifyTime;

        // Directory flag (WIN32 FILE_ATTRIBUTE_DIRECTORY = 0x10)
        node.isDirectory = (rec.fileAttributes & 0x10) != 0;

        // Extension
        auto dot = rec.name.rfind(L'.');
        if (dot != std::wstring::npos && dot + 1 < rec.name.size())
            node.extension = rec.name.substr(dot + 1);

        // Full path – try to resolve via the MFT record map built during
        // ScanMetadata().  Parent directories that still exist will be
        // found; deleted parents get a placeholder "<FRN?>" segment.
        node.originalPath = ResolveFullPath(rec.parentFrn, rec.name);

        // No cluster information available from USN journal alone.
        // RecoverabilityLevel stays Unknown.

        m_files.push_back(node);
        knownFrns.insert(rec.fileFrn); // prevent duplicates within journal
        ++added;

        LOG_DEBUG(L"NtfsParser::ScanUsnJournal + FRN=" + std::to_wstring(rec.fileFrn)
                + L"  path=[" + node.originalPath + L"]"
                + L"  allReasons=0x" + ToHex32(rec.allReasons));

        // Real-time callback: report file immediately when found via USN Journal
        if (m_onFileFound) {
            m_onFileFound(node);
        }
    }

    LOG_INFO(L"  New files from $UsnJrnl : " + std::to_wstring(added));
    LOG_INFO(L"  Total nodes now : " + std::to_wstring(m_files.size()));
    LOG_INFO(L"NtfsParser::ScanUsnJournal done ─────────────────────────────");

    return added;
}

// ─────────────────────────────────────────────────────────────────────────────
// ParseRecycleBinMetadata – parse $Ixxxxx.xxx files to extract original paths
// and deletion times, then update corresponding $R files.
//
// $I file format (Windows Vista+):
//   Offset 0x00: Version (2 bytes) = 0x01
//   Offset 0x02: Header size (2 bytes) = 0x0000
//   Offset 0x04: File size (4 bytes) = low 32 bits of original size
//   Offset 0x08: Original path (Unicode, null-terminated, 2-byte aligned)
//   After path: Deletion FILETIME (8 bytes, little-endian)
//   After FILETIME: Original file size (8 bytes, little-endian)
// ─────────────────────────────────────────────────────────────────────────────
uint32_t NtfsParser::ParseRecycleBinMetadata()
{
    if (!m_valid || m_files.empty()) return 0;

    LOG_INFO(L"NtfsParser::ParseRecycleBinMetadata – scanning for $I files");

    // Helper: read $DATA attribute content from a MFT record.
    // For $Ixxxxx.xxx files this is always a small resident attribute,
    // so we ONLY support the resident case here to keep things safe/simple.
    auto ReadFileData = [this](uint64_t recordIndex, std::vector<uint8_t>& outData) -> bool
    {
        if (recordIndex >= 0xFFFFFFFFFFFFULL) return false;

        // Calculate byte offset of this MFT record
        uint64_t recordOffset = 0;
        if (!MftRecordByteOffset(recordIndex, m_mftByteOffset, m_partStartBytes,
                                 m_bytesPerCluster, m_recordSize, m_mftClusters,
                                 recordOffset)) {
            return false;
        }

        std::vector<uint8_t> recBuf(m_recordSize);
        if (!m_reader->ReadBytes(recordOffset, recBuf.data(), m_recordSize)) {
            return false;
        }

        if (memcmp(recBuf.data(), NTFS_FILE_MAGIC, 4) != 0) return false;
        if (!ApplyUSA(recBuf.data(), m_recordSize)) return false;

        auto* hdr = reinterpret_cast<const MFT_RECORD_HEADER*>(recBuf.data());
        const uint8_t* p = recBuf.data() + hdr->attrOffset;
        const uint8_t* end = recBuf.data() + hdr->bytesInUse;

        while (p + sizeof(ATTR_HEADER_COMMON) <= end) {
            auto* ah = reinterpret_cast<const ATTR_HEADER_COMMON*>(p);
            if (ah->type == ATTR_END || ah->length == 0) break;
            
            if (ah->type == ATTR_DATA && !ah->nonResident) {
                // Resident $DATA – copy directly
                auto* res = reinterpret_cast<const ATTR_RESIDENT*>(p);
                if (res->valueLength > 0 && 
                    res->valueOffset + res->valueLength <= ah->length) {
                    const uint8_t* data = p + res->valueOffset;
                    outData.assign(data, data + res->valueLength);
                    return true;
                }
            }
            
            p += ah->length;
        }
        return false;
    };

    // Collect all $I files (indexed by prefix)
    struct IFileInfo {
        uint64_t recordIndex;
        std::wstring fileName;
        std::vector<uint8_t> data;
    };
    std::unordered_map<std::wstring, IFileInfo> iFiles; // key = prefix (e.g., "I12345")

    // First pass: find all $I files
    for (const auto& file : m_files) {
        // Check if path contains $RECYCLE.BIN or $Recycle.Bin (case-insensitive)
        std::wstring pathLower = file.originalPath;
        for (auto& c : pathLower) c = static_cast<wchar_t>(towlower(c));
        
        if (pathLower.find(L"$recycle.bin") == std::wstring::npos &&
            pathLower.find(L"\\recycler\\") == std::wstring::npos) {
            continue;
        }

        // Check if filename starts with $I
        if (file.name.length() < 2 || file.name[0] != L'$' || 
            (file.name[1] != L'I' && file.name[1] != L'i')) {
            continue;
        }

        // Extract prefix (everything before the dot, excluding $I)
        size_t dotPos = file.name.find(L'.');
        if (dotPos == std::wstring::npos || dotPos < 2) continue;
        
        std::wstring prefix = file.name.substr(2, dotPos - 2); // Skip "$I"

        // Read $I file data
        std::vector<uint8_t> iData;
        if (!ReadFileData(file.id, iData) || iData.size() < 24) {
            LOG_DEBUG(L"NtfsParser: $I file read failed rec=" + 
                     std::to_wstring(file.id) + L"  name=[" + file.name + L"]");
            continue;
        }

        iFiles[prefix] = { file.id, file.name, std::move(iData) };
        LOG_DEBUG(L"NtfsParser: found $I file rec=" + std::to_wstring(file.id) +
                 L"  name=[" + file.name + L"]  prefix=[" + prefix + L"]");
    }

    if (iFiles.empty()) {
        LOG_INFO(L"NtfsParser::ParseRecycleBinMetadata – no $I files found");
        return 0;
    }

    LOG_INFO(L"NtfsParser::ParseRecycleBinMetadata – found " +
             std::to_wstring(iFiles.size()) + L" $I files");

    // Second pass: match $I files with $R files and update metadata
    uint32_t updated = 0;

    for (auto& fileRef : m_files) {
        FileNode& file = fileRef; // Non-const reference for modification

        // Check if this is a $R file
        if (file.name.length() < 2 || file.name[0] != L'$' ||
            (file.name[1] != L'R' && file.name[1] != L'r')) {
            continue;
        }

        // Extract prefix
        size_t dotPos = file.name.find(L'.');
        if (dotPos == std::wstring::npos || dotPos < 2) continue;
        
        std::wstring prefix = file.name.substr(2, dotPos - 2); // Skip "$R"

        auto it = iFiles.find(prefix);
        if (it == iFiles.end()) continue;

        // Parse $I file content
        const std::vector<uint8_t>& iData = it->second.data;
        if (iData.size() < 24) continue;

        // Check version (should be 0x01)
        uint16_t version = Utils::ReadLE16(iData.data());
        if (version != 0x01) {
            LOG_DEBUG(L"NtfsParser: $I file version mismatch rec=" +
                     std::to_wstring(file.id) + L"  version=0x" + ToHex32(version));
            continue;
        }

        // Skip header (8 bytes: version + headerSize + fileSize)
        // Windows Vista+ $I format:
        //   Offset 0: Version (2 bytes) = 0x01
        //   Offset 2: Header size (2 bytes) = 0x0000
        //   Offset 4: File size low 32 bits (4 bytes)
        //   Offset 8: Original path length in bytes (4 bytes, excluding null)
        //   Offset 12: Original path (Unicode, no null terminator)
        //   After path: Deletion FILETIME (8 bytes)
        //   After FILETIME: Original file size (8 bytes)
        size_t offset = 8;

        // Read original path length (4 bytes)
        if (offset + 4 > iData.size()) continue;
        uint32_t pathLengthBytes = Utils::ReadLE32(iData.data() + offset);
        offset += 4;

        if (pathLengthBytes == 0 || pathLengthBytes > (iData.size() - offset) || 
            pathLengthBytes % 2 != 0) { // Must be even (Unicode)
            continue;
        }

        size_t pathLengthChars = pathLengthBytes / 2;
        if (offset + pathLengthBytes > iData.size()) continue;
        
        const wchar_t* pathPtr = reinterpret_cast<const wchar_t*>(iData.data() + offset);
        std::wstring originalPath(pathPtr, pathLengthChars);
        offset += pathLengthBytes;

        // Read deletion FILETIME (8 bytes)
        if (offset + 8 > iData.size()) continue;
        uint64_t deletionTime = Utils::ReadLE64(iData.data() + offset);
        offset += 8;

        // Read original file size (8 bytes)
        if (offset + 8 > iData.size()) continue;
        uint64_t originalSize = Utils::ReadLE64(iData.data() + offset);

        // Update FileNode
        bool changed = false;
        if (!originalPath.empty() && originalPath != file.originalPath) {
            file.originalPath = std::move(originalPath);
            changed = true;
        }
        if (deletionTime > 0 && deletionTime != file.modifyTime) {
            file.modifyTime = deletionTime;
            changed = true;
        }
        if (originalSize > 0 && originalSize != file.size) {
            file.size = originalSize;
            changed = true;
        }

        if (changed) {
            ++updated;
            LOG_DEBUG(L"NtfsParser: updated $R file rec=" + std::to_wstring(file.id) +
                     L"  $I=[" + it->second.fileName + L"]"
                     L"  originalPath=[" + file.originalPath + L"]"
                     L"  deletionTime=" + std::to_wstring(deletionTime) +
                     L"  originalSize=" + std::to_wstring(originalSize));

            // Notify callback of updated information
            if (m_onFileFound) {
                m_onFileFound(file);
            }
        }
    }

    LOG_INFO(L"NtfsParser::ParseRecycleBinMetadata done"
             L"  $I files found=" + std::to_wstring(iFiles.size()) +
             L"  $R files updated=" + std::to_wstring(updated));

    return updated;
}
