#include "ExfatParser.h"
#include "../../common/Utils.h"
#include "../../log/Logger.h"
#include "../../recover/FragmentStitchService.h"
#include "../../recover/FragmentStitchPolicy.h"
#include <algorithm>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// ExfatTimeToFiletime
//   Converts an exFAT packed timestamp to a Windows FILETIME (100-ns ticks
//   since 1601-01-01 UTC).
//
//   exFAT time word layout (little-endian 32-bit):
//     bits [31:25] = year   – 1980   (0..127  → 1980..2107)
//     bits [24:21] = month            (1..12)
//     bits [20:16] = day              (1..31)
//     bits [15:11] = hour             (0..23)
//     bits [10: 5] = minute           (0..59)
//     bits [ 4: 0] = 2-second units   (0..29 → 0..58 seconds)
//
//   utcOff: signed 7-bit UTC offset in 15-minute increments;
//           0x80 (= –128 as int8_t) means "no offset info, treat as local".
//   tenMsIncrement: 10-ms increment added to the seconds (0..199).
// ─────────────────────────────────────────────────────────────────────────────
uint64_t ExfatParser::ExfatTimeToFiletime(uint32_t t, uint8_t utcOff,
                                           uint8_t tenMsIncrement)
{
    if (t == 0) return 0;

    SYSTEMTIME st{};
    st.wYear         = static_cast<WORD>(1980 + ((t >> 25) & 0x7F));
    st.wMonth        = static_cast<WORD>((t >> 21) & 0x0F);
    st.wDay          = static_cast<WORD>((t >> 16) & 0x1F);
    st.wHour         = static_cast<WORD>((t >> 11) & 0x1F);
    st.wMinute       = static_cast<WORD>((t >> 5)  & 0x3F);
    st.wSecond       = static_cast<WORD>((t & 0x1F) * 2);
    st.wMilliseconds = 0;

    // 10-ms increment: values 0..99 add 0..990 ms, values 100..199 add 1 s + 0..990 ms
    if (tenMsIncrement >= 100) {
        ++st.wSecond;
        tenMsIncrement -= 100;
    }
    st.wMilliseconds = static_cast<WORD>(tenMsIncrement * 10);

    if (st.wMonth < 1 || st.wMonth > 12 || st.wDay < 1) return 0;

    FILETIME ft{};
    if (!::SystemTimeToFileTime(&st, &ft)) return 0;

    uint64_t result =
        (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;

    // Apply UTC offset if present.  utcOff is stored as a signed 7-bit field
    // in a uint8_t; values 0x00..0x7F are positive, 0x80 means "no info".
    if (utcOff != 0x80) {
        int8_t  signedOff   = static_cast<int8_t>(utcOff);
        // Each unit = 15 minutes = 15 * 60 * 10 000 000 = 9 000 000 000 ticks
        int64_t offsetTicks = static_cast<int64_t>(signedOff) * 9000000000LL;
        result = static_cast<uint64_t>(static_cast<int64_t>(result) - offsetTicks);
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Open
// ─────────────────────────────────────────────────────────────────────────────
bool ExfatParser::Open(SectorReader& reader, const Partition& partition)
{
    m_reader          = &reader;
    m_part            = partition;
    m_valid           = false;
    m_bitmapLoaded    = false;
    m_files.clear();
    m_fatTable.clear();
    m_allocBitmap.clear();
    m_nextId          = 1;
    m_totalRecords    = 0;
    m_inUseCount      = 0;
    m_partStartSector = partition.startSector;

    // Read boot sector
    uint32_t ss = reader.GetSectorSize();
    if (ss < 512) ss = 512;
    std::vector<uint8_t> buf(ss);

    if (!reader.ReadSectors(partition.startSector, buf.data(), 1)) {
        LOG_ERROR(L"ExfatParser: cannot read boot sector");
        return false;
    }

    memcpy(&m_boot, buf.data(), sizeof(m_boot));

    if (memcmp(m_boot.oemName, "EXFAT   ", 8) != 0) {
        LOG_WARN(L"ExfatParser: OEM name not 'EXFAT   '");
        return false;
    }

    // Sanity-check shift values
    if (m_boot.bytesPerSectorShift < 9 || m_boot.bytesPerSectorShift > 12) {
        LOG_ERROR(L"ExfatParser: invalid bytesPerSectorShift " +
                  std::to_wstring(m_boot.bytesPerSectorShift));
        return false;
    }
    if (m_boot.sectorsPerClusterShift > 25) {
        LOG_ERROR(L"ExfatParser: invalid sectorsPerClusterShift");
        return false;
    }

    m_bytesPerSector         = 1u << m_boot.bytesPerSectorShift;
    m_sectorsPerCluster      = 1u << m_boot.sectorsPerClusterShift;
    m_fatStartSector         = partition.startSector + m_boot.fatOffset;
    m_clusterHeapStartSector = partition.startSector + m_boot.clusterHeapOffset;
    m_totalClusters          = m_boot.clusterCount;

    // Load FAT into RAM (capped at 128 MB)
    uint64_t fatBytes =
        static_cast<uint64_t>(m_boot.fatLength) * m_bytesPerSector;
    constexpr uint64_t kMaxFat = 128ULL * 1024 * 1024;
    if (fatBytes > 0 && fatBytes <= kMaxFat) {
        m_fatTable.resize(static_cast<size_t>(fatBytes));
        if (!reader.ReadBytes(m_fatStartSector * m_bytesPerSector,
                              m_fatTable.data(),
                              static_cast<uint32_t>(fatBytes)))
            m_fatTable.clear();
    }

    m_valid = true;
    LOG_INFO(L"ExfatParser::Open ok"
             L"  bps="   + std::to_wstring(m_bytesPerSector) +
             L"  spc="   + std::to_wstring(m_sectorsPerCluster) +
             L"  clusters=" + std::to_wstring(m_totalClusters));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ScanMetadata
// ─────────────────────────────────────────────────────────────────────────────
bool ExfatParser::ScanMetadata(std::function<void(int)> onProgress,
                                std::atomic<bool>*       cancelFlag)
{
    if (!m_valid) return false;

    // Load allocation bitmap on first scan (finds it by traversing root dir
    // for the 0x81 entry type).
    LoadAllocationBitmap();

    ScanDirectory(m_boot.rootDirectoryCluster, L"\\", cancelFlag);

    if (onProgress) onProgress(100);

    LOG_INFO(L"ExfatParser::ScanMetadata – total=" +
             std::to_wstring(m_totalRecords) +
             L"  inUse=" + std::to_wstring(m_inUseCount) +
             L"  deleted=" + std::to_wstring(m_files.size()));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// LoadAllocationBitmap
//   Reads the root directory just far enough to find the allocation-bitmap
//   entry (type 0x81), then loads the bitmap into m_allocBitmap.
//   Safe to call multiple times (no-op if already loaded).
// ─────────────────────────────────────────────────────────────────────────────
bool ExfatParser::LoadAllocationBitmap()
{
    if (m_bitmapLoaded) return !m_allocBitmap.empty();

    m_bitmapLoaded = true;  // Don't retry on failure

    uint32_t cluster = m_boot.rootDirectoryCluster;
    const size_t clusterBytes =
        static_cast<size_t>(m_sectorsPerCluster) * m_bytesPerSector;
    std::vector<uint8_t> cbuf(clusterBytes);

    constexpr int kMaxClusters = 256;  // Limit root scan for bitmap search
    int clustersSearched = 0;

    while (cluster >= 2 && cluster < 0xFFFFFFF7U
           && clustersSearched++ < kMaxClusters)
    {
        uint64_t sec = ClusterToSector(cluster);
        if (!m_reader->ReadSectors(sec, cbuf.data(), m_sectorsPerCluster))
            break;

        size_t off = 0;
        while (off + 32 <= cbuf.size()) {
            uint8_t type = cbuf[off];

            if (type == EXFAT_ENTRY_EOD) goto bitmapDone;

            // Allocation bitmap entry (type 0x81)
            if (type == EXFAT_ENTRY_ALLOC_BMP) {
                // Bytes 20-23: first cluster (LE32)
                // Bytes 24-31: data length in bytes (LE64)
                uint32_t bmpCluster = Utils::ReadLE32(cbuf.data() + off + 20);
                uint64_t bmpBytes   = Utils::ReadLE64(cbuf.data() + off + 24);

                constexpr uint64_t kMaxBmp = 4ULL * 1024 * 1024; // 4 MB cap
                if (bmpBytes > 0 && bmpBytes <= kMaxBmp && bmpCluster >= 2) {
                    m_allocBitmap.resize(static_cast<size_t>(bmpBytes));
                    uint64_t byteOff = ClusterToSector(bmpCluster)
                                       * m_bytesPerSector;
                    if (!m_reader->ReadBytes(byteOff,
                                             m_allocBitmap.data(),
                                             static_cast<uint32_t>(bmpBytes)))
                        m_allocBitmap.clear();
                    else
                        LOG_INFO(L"ExfatParser: allocation bitmap loaded "
                                 + std::to_wstring(bmpBytes) + L" bytes");
                }
                goto bitmapDone;
            }

            off += 32;
        }

        cluster = NextCluster(cluster);
    }

bitmapDone:
    return !m_allocBitmap.empty();
}

// ─────────────────────────────────────────────────────────────────────────────
// IsClusterFree
//   Returns true when the given cluster's bit in the allocation bitmap is 0
//   (meaning it is not allocated).  Returns true (optimistic) if the bitmap
//   is not available or the cluster is out of range.
// ─────────────────────────────────────────────────────────────────────────────
bool ExfatParser::IsClusterFree(uint32_t cluster) const
{
    if (m_allocBitmap.empty()) return true;  // Unknown → optimistic
    if (cluster < 2) return false;

    uint32_t idx = cluster - 2;  // Bitmap bit 0 = cluster 2
    size_t  byte = idx / 8;
    uint8_t bit  = static_cast<uint8_t>(1u << (idx % 8));

    if (byte >= m_allocBitmap.size()) return true;
    return (m_allocBitmap[byte] & bit) == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// AssessRecoverability
//   Checks `clusterCount` consecutive clusters starting at `firstCluster`
//   against the allocation bitmap.  Deleted file clusters should ideally be
//   free (bit = 0 in the bitmap after deletion).
// ─────────────────────────────────────────────────────────────────────────────
RecoverabilityLevel ExfatParser::AssessRecoverability(uint32_t firstCluster,
                                                       uint32_t clusterCount)
{
    if (m_allocBitmap.empty() || firstCluster < 2 || clusterCount == 0)
        return RecoverabilityLevel::Unknown;

    uint32_t allocated = 0;
    for (uint32_t i = 0; i < clusterCount; ++i) {
        if (!IsClusterFree(firstCluster + i)) ++allocated;
    }

    if (clusterCount == 0) return RecoverabilityLevel::Unknown;
    unsigned pct = allocated * 100 / clusterCount;

    RecoverabilityLevel level;
    if (pct == 0)        level = RecoverabilityLevel::Excellent;
    else if (pct <= 10)  level = RecoverabilityLevel::Good;
    else if (pct <= 50)  level = RecoverabilityLevel::Poor;
    else                 level = RecoverabilityLevel::Lost;

    // If rated Excellent or Good, sample first cluster to check for zeroed data
    if (level == RecoverabilityLevel::Excellent || level == RecoverabilityLevel::Good) {
        uint64_t sec = ClusterToSector(firstCluster);
        uint8_t sample[512]{};
        if (m_reader->ReadSectors(sec, sample, 1)) {
            bool allZero = true;
            for (int i = 0; i < 512; ++i) {
                if (sample[i] != 0) { allZero = false; break; }
            }
            if (allZero) {
                level = RecoverabilityLevel::Lost;
            }
        }
    }

    return level;
}

// ─────────────────────────────────────────────────────────────────────────────
// ScanDirectory
// ─────────────────────────────────────────────────────────────────────────────
void ExfatParser::ScanDirectory(uint32_t startCluster,
                                 const std::wstring& path,
                                 std::atomic<bool>* cancelFlag,
                                 int depth,
                                 bool ancestorsDeleted)
{
    if (cancelFlag && cancelFlag->load()) return;

    uint32_t cluster = startCluster;
    const size_t clusterBytes =
        static_cast<size_t>(m_sectorsPerCluster) * m_bytesPerSector;
    std::vector<uint8_t> cbuf(clusterBytes);

    while (cluster >= 2 && cluster < 0xFFFFFFF7U) {
        if (cancelFlag && cancelFlag->load()) break;

        uint64_t sec = ClusterToSector(cluster);
        if (!m_reader->ReadSectors(sec, cbuf.data(), m_sectorsPerCluster))
            break;

        size_t offset = 0;
        while (offset + 32 <= cbuf.size()) {
            uint8_t entType = cbuf[offset];

            if (entType == EXFAT_ENTRY_EOD) return;

            // Allocation bitmap and up-case table are metadata – skip
            if (entType == EXFAT_ENTRY_ALLOC_BMP ||
                entType == EXFAT_ENTRY_UPCASE    ||
                entType == EXFAT_ENTRY_VOLUME_LABEL) {
                offset += 32;
                continue;
            }

            // Active file entry (0x85) or deleted file entry (0x05)
            bool activeFile  = (entType == EXFAT_ENTRY_FILE);   // 0x85
            bool deletedFile = (entType == 0x05);               // deleted

            if (!activeFile && !deletedFile) {
                offset += 32;
                continue;
            }

            auto* fe = reinterpret_cast<const EXFAT_FILE_ENTRY*>(
                cbuf.data() + offset);
            uint8_t secCount = fe->secondaryCount;

            // Validate that the full entry set fits in the buffer
            if (offset + 32ULL * (1 + secCount) > cbuf.size()) {
                offset += 32;
                continue;
            }

            ++m_totalRecords;
            if (activeFile) ++m_inUseCount;

            // ── Stream Extension (first secondary) ────────────────────────
            const uint8_t* seBytes = cbuf.data() + offset + 32;
            // Active entry: type 0xC0; deleted: type 0x40
            bool seOk = (seBytes[0] == EXFAT_ENTRY_STREAM_EXT || seBytes[0] == 0x40);
            if (!seOk) {
                offset += 32ULL * (1 + secCount);
                continue;
            }
            auto* se = reinterpret_cast<const EXFAT_STREAM_EXT*>(seBytes);

            // ── File name extensions ──────────────────────────────────────
            std::wstring name;
            for (uint8_t i = 2; i <= secCount && i <= 20; ++i) {
                const uint8_t* entry = cbuf.data() + offset + 32ULL * i;
                // Active: 0xC1; deleted: 0x41
                if (entry[0] != EXFAT_ENTRY_FILE_NAME && entry[0] != 0x41)
                    break;
                auto* fne = reinterpret_cast<const EXFAT_FILENAME_EXT*>(entry);
                for (int j = 0; j < 15; ++j) {
                    if (fne->fileName[j] == 0) goto nameDone;
                    name += static_cast<wchar_t>(fne->fileName[j]);
                }
            }
            nameDone:;

            if (name.empty()) {
                offset += 32ULL * (1 + secCount);
                continue;
            }

            // Mac HFS+ metadata on CFReDS exFAT images — not part of DFR counts.
            if (!name.empty() && name[0] == L'.') {
                offset += 32ULL * (1 + secCount);
                continue;
            }

            const bool inDeletedTree = ancestorsDeleted || deletedFile;
            const bool isDirectory = (fe->fileAttributes & 0x10) != 0;

            // ── Build FileNode ─────────────────────────────────────────────
            FileNode node;
            node.id           = m_nextId++;
            node.fsType       = L"exFAT";
            node.name         = name;
            node.originalPath = path + name;
            node.size         = se->dataLength;
            node.isDirectory  = isDirectory;
            node.status       = (inDeletedTree && !isDirectory)
                                    ? FileStatus::Deleted
                                    : (deletedFile ? FileStatus::Deleted
                                                   : FileStatus::Normal);

            // ── Timestamps ────────────────────────────────────────────────
            node.createTime = ExfatTimeToFiletime(
                fe->createTime,
                fe->createUtcOffset,
                fe->create10msIncrement);
            node.modifyTime = ExfatTimeToFiletime(
                fe->lastModifiedTime,
                fe->lastModifiedUtcOffset,
                fe->lastModified10msIncrement);
            node.accessTime = ExfatTimeToFiletime(
                fe->lastAccessedTime,
                fe->lastAccessedUtcOffset);

            // ── Extension ─────────────────────────────────────────────────
            {
                size_t dot = name.rfind(L'.');
                if (dot != std::wstring::npos) {
                    node.extension = name.substr(dot + 1);
                    for (auto& c : node.extension)
                        c = static_cast<wchar_t>(towlower(c));
                }
            }

            // ── Cluster chain ─────────────────────────────────────────────
            // generalSecondaryFlags bit 1 (0x02) = AllocationPossible
            // generalSecondaryFlags bit 0 (0x01) = NoFatChain (contiguous)
            bool allocPossible = (se->generalSecondaryFlags & 0x02) != 0;
            bool noFatChain    = (se->generalSecondaryFlags & 0x01) != 0;

            uint32_t firstCluster  = se->firstCluster;
            uint32_t clusterCount  = 0;
            if (se->dataLength > 0 && m_sectorsPerCluster > 0 && m_bytesPerSector > 0) {
                uint64_t bytesPerCluster =
                    static_cast<uint64_t>(m_sectorsPerCluster) * m_bytesPerSector;
                clusterCount = static_cast<uint32_t>(
                    (se->dataLength + bytesPerCluster - 1) / bytesPerCluster);
            }

            if ((allocPossible || inDeletedTree) && firstCluster >= 2) {
                // NoFatChain is an on-disk property of the stream (contiguous
                // allocation, no FAT chain to follow).  Deletion only clears
                // the high bit of the directory-entry TYPE; the stream flags
                // remain intact.  Forcing this to false for deleted entries
                // made us follow FAT entries that deletion had zeroed and then
                // let the FAT heuristic stitcher rewrite a perfectly good
                // contiguous chain (see ExfatRecoverBytes / CAR-003).
                const bool chainNoFat = noFatChain;
                if (!node.isDirectory || activeFile) {
                    node.clusters = BuildClusterChain(firstCluster,
                                                      clusterCount,
                                                      chainNoFat,
                                                      se->dataLength,
                                                      node.extension);
                }
                // For contiguous files (NoFatChain) also set rawOffset/rawSize
                // so the RecoverEngine can use direct byte-range reads
                // (which is faster and more reliable for deleted contiguous files).
                if (chainNoFat && !node.isDirectory) {
                    node.rawOffset =
                        ClusterToSector(firstCluster) * m_bytesPerSector;
                    node.rawSize = se->dataLength;
                }
            }

            if (node.isDirectory) node.originalPath += L'\\';

            // ── Recoverability ─────────────────────────────────────────────
            if (inDeletedTree && !isDirectory) {
                if (node.size == 0) {
                    node.recoverability = RecoverabilityLevel::Excellent;
                } else if (noFatChain && firstCluster >= 2 && clusterCount > 0) {
                    // Contiguous – we know exactly which clusters to check
                    node.recoverability = AssessRecoverability(firstCluster,
                                                               clusterCount);
                } else if (firstCluster >= 2 && clusterCount > 0
                           && !m_allocBitmap.empty()) {
                    // FAT chain may be gone, but check at least the first cluster
                    node.recoverability = AssessRecoverability(firstCluster, 1);
                } else {
                    node.recoverability = RecoverabilityLevel::Unknown;
                }

                // Real-time callback
                if (m_onFileFound) m_onFileFound(node);
            } else {
                node.recoverability = RecoverabilityLevel::Excellent;
            }

            bool doRecurse = isDirectory && firstCluster >= 2 && depth < 8;

            m_files.push_back(node);

            if (doRecurse)
                ScanDirectory(firstCluster, node.originalPath, cancelFlag,
                              depth + 1, inDeletedTree || deletedFile);

            offset += 32ULL * (1 + secCount);
        }

        cluster = NextCluster(cluster);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// NextCluster
// ─────────────────────────────────────────────────────────────────────────────
uint32_t ExfatParser::NextCluster(uint32_t cluster)
{
    if (cluster < 2) return 0xFFFFFFFEU;

    uint64_t offset = static_cast<uint64_t>(cluster) * 4;
    if (!m_fatTable.empty()) {
        if (offset + 4 > m_fatTable.size()) return 0xFFFFFFFEU;
        return Utils::ReadLE32(m_fatTable.data() + offset);
    }
    uint8_t buf[4]{};
    if (!m_reader->ReadBytes(m_fatStartSector * m_bytesPerSector + offset, buf, 4))
        return 0xFFFFFFFEU;
    return Utils::ReadLE32(buf);
}

// ─────────────────────────────────────────────────────────────────────────────
// BuildClusterChain
//   Builds the list of absolute sector numbers for the cluster chain.
//
//   If noFatChain is true (contiguous file) and clusterCount > 0, we extend
//   the chain sequentially without reading the FAT – important for deleted
//   contiguous files whose FAT entries have been zeroed.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint64_t> ExfatParser::BuildClusterChain(uint32_t startCluster,
                                                       uint32_t clusterCount,
                                                       bool     noFatChain,
                                                       uint64_t fileSize,
                                                       const std::wstring& extension)
{
    std::vector<uint64_t> chain;
    if (startCluster < 2) return chain;

    constexpr uint32_t kMaxClusters = 1u << 24;  // 16M cluster cap
    uint32_t cur = startCluster;
    const uint32_t limit = (clusterCount > 0)
                           ? std::min(clusterCount, kMaxClusters)
                           : kMaxClusters;

    if (noFatChain && clusterCount > 0) {
        // Contiguous allocation: clusters are consecutive
        uint32_t n = std::min(clusterCount, kMaxClusters);
        chain.reserve(n);
        for (uint32_t i = 0; i < n; ++i)
            chain.push_back(ClusterToSector(cur + i));
    } else {
        // Follow FAT chain; extend sequentially when entries are zeroed after delete.
        while (cur >= 2 && cur < 0xFFFFFFF7U && chain.size() < limit) {
            chain.push_back(ClusterToSector(cur));
            uint32_t next = NextCluster(cur);
            if (next == 0 || next >= 0xFFFFFFF8U || next == cur) {
                if (clusterCount > chain.size()) {
                    uint32_t seqCur = cur;
                    while (chain.size() < clusterCount && chain.size() < kMaxClusters) {
                        ++seqCur;
                        if (seqCur >= 2 && seqCur <= m_totalClusters + 1)
                            chain.push_back(ClusterToSector(seqCur));
                        else
                            break;
                    }
                }
                break;
            }
            cur = next;
        }
    }

    // A NoFatChain stream is contiguous by definition: there is no fragment
    // to reassemble.  Running the FAT12/16/32 greedy scorer here is not only
    // pointless, it is actively harmful for deleted exFAT files: deletion
    // zeroes the FAT entries, so the scorer treats live data clusters as
    // "free" candidates and can replace a correct cluster with an unrelated
    // one that merely scores better as a payload.  Fragmented (FAT-chained)
    // exFAT streams keep the existing behaviour; exFAT also has its dedicated
    // recovery-time path FatFragmentStitcher::TryImproveExfatNode().
    if (!noFatChain && fileSize > 0 && !chain.empty() && m_reader) {
        FatVolumeGeometry geo{};
        geo.bytesPerSector    = m_bytesPerSector;
        geo.sectorsPerCluster = m_sectorsPerCluster;
        geo.dataStartSector   = m_clusterHeapStartSector;
        geo.totalClusters     = m_totalClusters;
        geo.fatType           = 32;

        FragmentStitchPolicy policy;
        FragmentStitchService::TryImproveFatClusterChain(
            *m_reader, geo, &m_fatTable, startCluster, fileSize, extension, chain,
            policy);
    }

    return chain;
}

// ─────────────────────────────────────────────────────────────────────────────
// ClusterToSector
// ─────────────────────────────────────────────────────────────────────────────
uint64_t ExfatParser::ClusterToSector(uint32_t cluster) const
{
    return m_clusterHeapStartSector
           + static_cast<uint64_t>(cluster - 2) * m_sectorsPerCluster;
}
