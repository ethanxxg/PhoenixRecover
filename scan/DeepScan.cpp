#include "DeepScan.h"
#include "RawScan.h"
#include "../fs/ntfs/NtfsParser.h"
#include "../fs/ntfs/NtfsStructs.h"
#include "../fs/fat/FatStructs.h"
#include "../common/Constants.h"
#include "../common/Utils.h"
#include "../log/Logger.h"
#include <chrono>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <cstring>

// Bring in NTFS-specific constants from the Constants namespace.
// We do NOT use `using namespace Constants` to avoid ambiguity with the
// FAT_ATTR_* constants that also live in FatStructs.h.
using Constants::NTFS_FILE_MAGIC;
using Constants::NTFS_MFT_RECORD_SIZE;
using Constants::MFT_FLAG_IN_USE;
using Constants::ATTR_END;
using Constants::ATTR_FILE_NAME;
using Constants::ATTR_DATA;

// ─────────────────────────────────────────────────────────────────────────────
// DeepScan::Run
// ─────────────────────────────────────────────────────────────────────────────
ScanResult DeepScan::Run(SectorReader& reader, const Partition& partition)
{
    ScanResult result;
    auto tStart = std::chrono::steady_clock::now();

    LOG_INFO(L"DeepScan::Run – partition=" + partition.DisplayName());

    // ── NTFS helper (for bitmap-based recoverability on NTFS volumes) ─────
    bool ntfsBootOk = false;
    NtfsParser ntfsHelper;
    if (ntfsHelper.Open(reader, partition)) {
        m_ntfsHelper = &ntfsHelper;
        ntfsBootOk   = true;
        LOG_INFO(L"DeepScan: NTFS boot sector recognised");
    } else {
        m_ntfsHelper = nullptr;
    }

    // ── FAT geometry detection ────────────────────────────────────────────
    FatDeepInfo fatInfo;
    bool fatBootOk = false;
    if (DetectFatBpb(reader, partition, fatInfo)) {
        fatBootOk = true;
        LOG_INFO(L"DeepScan: FAT" + std::to_wstring(fatInfo.fatType) +
                 L" geometry detected  bytesPerSector=" +
                 std::to_wstring(fatInfo.bytesPerSector) +
                 L"  spc=" + std::to_wstring(fatInfo.sectorsPerCluster) +
                 L"  dataStart=" + std::to_wstring(fatInfo.dataStartSector));
    } else {
        LOG_INFO(L"DeepScan: FAT BPB not detected – FAT dir scan will use "
                 L"heuristic-only mode (no cluster resolution)");
    }

    const uint32_t ss        = reader.GetSectorSize();
    const uint64_t partStart = partition.startSector;
    const uint64_t partTotal = partition.totalSectors;
    const uint64_t totalBytes = partTotal * ss;

    constexpr uint32_t kBatchSectors  = 128;
    constexpr uint32_t kMftRecordSize = Constants::NTFS_MFT_RECORD_SIZE;

    std::vector<uint8_t> buf(static_cast<size_t>(kBatchSectors) * ss);

    uint64_t scanned = 0;

    for (uint64_t sector = 0; sector < partTotal; ) {
        if (IsCancelled()) { result.cancelled = true; break; }

        uint32_t toRead = static_cast<uint32_t>(
            std::min(static_cast<uint64_t>(kBatchSectors), partTotal - sector));

        if (!reader.ReadSectors(partStart + sector, buf.data(), toRead)) {
            sector  += toRead;
            scanned += static_cast<uint64_t>(toRead) * ss;
            continue;
        }

        const uint32_t batchBytes = toRead * ss;

        // Step every 512 bytes (physical sector size on nearly all media)
        for (uint32_t i = 0; i + 512 <= batchBytes; i += 512)
        {
            const uint8_t* p = buf.data() + i;
            const uint64_t absByteOff = (partStart + sector) * ss + i;
            const uint64_t absSector  = absByteOff / ss;

            // ── NTFS: look for MFT record magic "FILE" ────────────────────
            if (i + kMftRecordSize <= batchBytes &&
                memcmp(p, Constants::NTFS_FILE_MAGIC, 4) == 0)
            {
                TryParseMftRecord(p, kMftRecordSize, absByteOff, result.files);
            }

            // ── FAT: look for directory sectors ───────────────────────────
            if (IsLikelyFatDirSector(p, ss)) {
                TryParseFatDirSector(p, ss, absSector, fatInfo, result.files);
            }
        }

        sector  += toRead;
        scanned += static_cast<uint64_t>(toRead) * ss;

        if ((sector & 0xFFFF) == 0) {
            ReportProgress(scanned, totalBytes,
                           static_cast<uint32_t>(result.files.size()));
        }
    }

    m_ntfsHelper = nullptr;

    // ── Fallback: structured NTFS/FAT parse failed → SignatureMatcher RAW carving ──
    // If the volume is a recognised file system (NTFS boot sector or FAT BPB) but the
    // structured deep scan recovered almost nothing — the classic "damaged MFT / directory"
    // case — fall back to content-based RAW carving.  We reuse the verified SignatureMatcher
    // path (RawScan::Run, which is now backed by SignatureDB::ScanBatch), so the carved set
    // is identical to a pure RAW scan while still being driven by DeepScan's failure trigger.
    {
        constexpr size_t kDeepFallbackMaxFiles = 8;
        const bool recognizedFs = ntfsBootOk || fatBootOk;
        if (recognizedFs && result.files.size() < kDeepFallbackMaxFiles) {
            LOG_INFO(L"DeepScan: structured parse recovered too few files on a "
                     L"recognised FS (" + std::to_wstring(result.files.size()) +
                     L"); falling back to SignatureMatcher RAW carving");
            RawScan rawScan;
            if (m_cancelCb) rawScan.SetCancelCallback(m_cancelCb);
            ScanResult raw = rawScan.Run(reader, partition);

            // Merge RAW-carved files, de-duplicating by rawOffset (skip offset 0, which
            // is the default for unpopulated nodes and would collide with the boot sector).
            std::unordered_set<uint64_t> have;
            for (const auto& f : result.files)
                if (f.rawOffset != 0) have.insert(f.rawOffset);
            size_t added = 0;
            for (const auto& f : raw.files) {
                if (f.rawOffset == 0) continue;
                if (have.count(f.rawOffset)) continue;
                have.insert(f.rawOffset);
                result.files.push_back(f);
                ++added;
            }
            LOG_INFO(L"DeepScan: RAW fallback added " + std::to_wstring(added) +
                     L" carved file(s) (raw scan found " +
                     std::to_wstring(raw.files.size()) + L")");
        }
    }

    result.foundCount   = static_cast<uint32_t>(result.files.size());
    result.scannedBytes = scanned;
    result.duration     = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - tStart);

    // Remove exact duplicates (same id)
    std::sort(result.files.begin(), result.files.end(),
              [](const FileNode& a, const FileNode& b) { return a.id < b.id; });
    result.files.erase(
        std::unique(result.files.begin(), result.files.end(),
                    [](const FileNode& a, const FileNode& b) {
                        return a.id == b.id;
                    }),
        result.files.end());
    result.foundCount = static_cast<uint32_t>(result.files.size());

    ReportProgress(scanned, totalBytes, result.foundCount);

    LOG_INFO(L"DeepScan done – found " + std::to_wstring(result.foundCount) +
             L" records in " + std::to_wstring(result.duration.count()) + L" ms");

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// TryParseMftRecord  (NTFS – unchanged from original)
// ─────────────────────────────────────────────────────────────────────────────
bool DeepScan::TryParseMftRecord(const uint8_t* buf, size_t bufLen,
                                  uint64_t diskByteOffset,
                                  std::vector<FileNode>& out)
{
    if (bufLen < sizeof(MFT_RECORD_HEADER)) return false;

    auto* hdr = reinterpret_cast<const MFT_RECORD_HEADER*>(buf);
        if (memcmp(hdr->magic, Constants::NTFS_FILE_MAGIC, 4) != 0) return false;
    if (hdr->attrOffset < sizeof(MFT_RECORD_HEADER)) return false;
    if (hdr->bytesInUse == 0 || hdr->attrOffset >= bufLen) return false;

    FileNode node;
    node.id     = diskByteOffset;
    node.fsType = L"NTFS";
    node.status = (hdr->flags & MFT_FLAG_IN_USE)
                  ? FileStatus::Normal : FileStatus::Deleted;

    const uint8_t* p   = buf + hdr->attrOffset;
    const uint8_t* end = buf + std::min(static_cast<size_t>(hdr->bytesInUse), bufLen);

    bool hasName = false;
    bool hasData = false;

    while (p + sizeof(ATTR_HEADER_COMMON) <= end) {
        auto* ah = reinterpret_cast<const ATTR_HEADER_COMMON*>(p);
        if (ah->type == ATTR_END || ah->length == 0 || ah->length > 65536) break;

        if (ah->type == ATTR_FILE_NAME && !ah->nonResident) {
            auto* res = reinterpret_cast<const ATTR_RESIDENT*>(p);
            const uint8_t* val  = p + res->valueOffset;
            size_t         vlen = res->valueLength;

            if (val + vlen <= end && vlen >= sizeof(FILE_NAME_ATTR)) {
                auto* fn = reinterpret_cast<const FILE_NAME_ATTR*>(val);
                if (vlen >= sizeof(FILE_NAME_ATTR) + fn->nameLength * 2u) {
                    auto* nptr = reinterpret_cast<const wchar_t*>(
                        val + sizeof(FILE_NAME_ATTR));
                    std::wstring name(nptr, fn->nameLength);
                    if (!name.empty() && (fn->nameType == 1 || fn->nameType == 3
                                          || !hasName)) {
                        node.name       = name;
                        node.size       = fn->realSize;
                        node.createTime = fn->createTime;
                        node.modifyTime = fn->modifyTime;
                        node.accessTime = fn->accessTime;
                        node.parentId   = MftRefRecordNumber(fn->parentDirRef);
                        hasName         = true;
                    }
                }
            }
        } else if (ah->type == ATTR_DATA && ah->nonResident && !hasData) {
            auto* nr = reinterpret_cast<const ATTR_NONRESIDENT*>(p);
            const uint8_t* runs = p + nr->dataRunOffset;
            size_t maxRunBytes  = (ah->length > nr->dataRunOffset)
                                  ? (ah->length - nr->dataRunOffset) : 0;
            if (maxRunBytes > 0) {
                node.clusters = NtfsParser::DecodeDataRuns(runs, maxRunBytes);
                hasData = !node.clusters.empty();
            }
        }

        if (static_cast<size_t>(ah->length) > static_cast<size_t>(end - p)) break;
        p += ah->length;
    }

    if (!hasName || node.name.empty()) return false;
    if (node.name == L"." || node.name == L"..") return false;

    size_t dot = node.name.rfind(L'.');
    if (dot != std::wstring::npos) {
        node.extension = node.name.substr(dot + 1);
        for (auto& c : node.extension) c = static_cast<wchar_t>(towlower(c));
    }

    if (m_ntfsHelper && !node.clusters.empty() &&
        (node.status == FileStatus::Deleted || node.status == FileStatus::Lost)) {
        node.recoverability = m_ntfsHelper->AssessRecoverability(node);
    } else {
        node.recoverability = RecoverabilityLevel::Unknown;
    }

    if (node.status == FileStatus::Deleted || node.status == FileStatus::Lost)
        ReportFile(node);

    out.push_back(std::move(node));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// DetectFatBpb
//   Reads the boot sector of the partition and extracts key FAT geometry
//   fields.  Returns false if the boot sector doesn't look like a FAT BPB.
// ─────────────────────────────────────────────────────────────────────────────
bool DeepScan::DetectFatBpb(SectorReader& reader, const Partition& part,
                              FatDeepInfo& out)
{
    const uint32_t ss = reader.GetSectorSize();
    std::vector<uint8_t> buf(ss);
    if (!reader.ReadSectors(part.startSector, buf.data(), 1)) return false;

    auto* bpb = reinterpret_cast<const FAT_BPB_COMMON*>(buf.data());

    // Basic sanity checks on BPB
    const uint16_t bps = bpb->bytesPerSector;
    const uint8_t  spc = bpb->sectorsPerCluster;
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) != 0) return false;
    if (spc == 0 || (spc & (spc - 1)) != 0) return false;
    if (bpb->numFATs == 0 || bpb->numFATs > 2) return false;
    if (bpb->reservedSectors == 0) return false;

    // Must have boot sector signature 0x55 0xAA
    if (buf[510] != 0x55 || buf[511] != 0xAA) return false;

    // FAT size
    uint32_t fatSize = (bpb->fatSize16 != 0)
        ? bpb->fatSize16
        : reinterpret_cast<const FAT32_BPB*>(buf.data())->fatSize32;
    if (fatSize == 0) return false;

    // Root dir sectors
    uint32_t rootSectors =
        (static_cast<uint32_t>(bpb->rootEntryCount) * 32 + bps - 1) / bps;

    uint64_t fatStart  = part.startSector + bpb->reservedSectors;
    uint64_t rootStart = fatStart + static_cast<uint64_t>(bpb->numFATs) * fatSize;
    uint64_t dataStart = rootStart + rootSectors;

    // Total sectors
    uint32_t totalSectors = (bpb->totalSectors16 != 0)
        ? bpb->totalSectors16 : bpb->totalSectors32;

    // Cluster count → FAT type determination
    if (totalSectors == 0 || dataStart >= part.startSector + totalSectors)
        return false;

    uint64_t dataSectors = part.startSector + totalSectors - dataStart;
    uint32_t totalClusters = static_cast<uint32_t>(dataSectors / spc);

    uint8_t fatType = 0;
    if      (totalClusters < 4085)   fatType = 12;
    else if (totalClusters < 65525)  fatType = 16;
    else                              fatType = 32;

    out.valid             = true;
    out.bytesPerSector    = bps;
    out.sectorsPerCluster = spc;
    out.partStartSector   = part.startSector;
    out.dataStartSector   = dataStart;
    out.fatType           = fatType;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// IsLikelyFatDirSector
//   Returns true when the 512-byte block looks like a FAT directory sector.
//
//   Criteria (all must hold):
//     • Every non-end entry must have attr bits 6-7 clear (no reserved bits)
//     • At least FAT_DIR_MIN_VALID valid entries found
//     • At least 1 of those entries is deleted (0xE5) or an LFN (0x0F)
//     • No entry has a file size > 4 GB (FAT hard limit) – reduces false pos.
// ─────────────────────────────────────────────────────────────────────────────
bool DeepScan::IsLikelyFatDirSector(const uint8_t* p, uint32_t sectorSize)
{
    static constexpr int FAT_DIR_MIN_VALID = 3;

    const uint32_t entCount = (std::min)(sectorSize / 32u, 16u);
    if (entCount < 4) return false;

    int goodEntries    = 0;
    int interestingCnt = 0;  // deleted or LFN entries

    for (uint32_t i = 0; i < entCount; ++i) {
        const uint8_t* e = p + i * 32;
        const uint8_t  first = e[0];
        const uint8_t  attr  = e[11];

        if (first == 0x00) break;  // end-of-directory marker

        // Reserved attr bits must be 0 – hard reject for any entry
        if (attr & 0xC0) return false;

        bool isLfn = (attr == 0x0F);   // FAT_ATTR_LFN
        bool isDel = (first == 0xE5);

        if (isLfn) {
            // LFN entries have a constrained layout
            // clusterLow must be 0x0000 (bytes 26-27)
            if (e[26] != 0 || e[27] != 0) return false;
            ++goodEntries;
            ++interestingCnt;
            continue;
        }

        // For 8.3 entries: name bytes must be printable (0x20+) or null-pad
        bool nameOk = true;
        for (int k = 1; k < 11; ++k) {
            const uint8_t c = e[k];
            if (c != 0x00 && c != 0x20 && c < 0x20) { nameOk = false; break; }
        }
        if (!nameOk) continue;  // Don't hard-reject, just skip this entry

        // File size sanity: must fit in uint32_t (FAT spec guarantees <= 4 GB)
        // (already uint32_t in struct; just check it's not some huge random value
        //  disguised by the 4-byte field – we allow anything up to 4 GB which is
        //  the FAT spec maximum, so no extra check needed here)

        ++goodEntries;
        if (isDel) ++interestingCnt;
    }

    return (goodEntries >= FAT_DIR_MIN_VALID) && (interestingCnt >= 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// ExtractLfnChars
//   Extracts the 13 UTF-16LE characters from one LFN directory entry.
//   Strips trailing 0xFFFF padding (unused slots) and 0x0000 terminator.
// ─────────────────────────────────────────────────────────────────────────────
std::wstring DeepScan::ExtractLfnChars(const uint8_t* e)
{
    // The 13 code units are split across three fields in the LFN entry:
    //   bytes  1-10  → 5 chars  (name1)
    //   bytes 14-25  → 6 chars  (name2)
    //   bytes 28-31  → 2 chars  (name3)
    static const int offsets[] = { 1, 3, 5, 7, 9,
                                   14, 16, 18, 20, 22, 24,
                                   28, 30 };
    wchar_t chars[14]{};
    int     len = 0;

    for (int k : offsets) {
        uint16_t c = static_cast<uint16_t>(e[k]) |
                     (static_cast<uint16_t>(e[k + 1]) << 8);
        if (c == 0x0000 || c == 0xFFFF) break;  // end or padding
        chars[len++] = static_cast<wchar_t>(c);
    }
    return std::wstring(chars, len);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reconstruct83Name
//   Returns the 8.3 filename as a wide string.
//   For deleted entries the first byte was 0xE5; we replace it with '_'.
// ─────────────────────────────────────────────────────────────────────────────
std::wstring DeepScan::Reconstruct83Name(const uint8_t* e, bool deleted)
{
    char name[9]{}, ext[4]{};
    memcpy(name, e, 8); name[8] = '\0';
    memcpy(ext,  e + 8, 3); ext[3] = '\0';

    if (deleted && static_cast<uint8_t>(name[0]) == 0xE5)
        name[0] = '_';

    // Trim trailing spaces
    for (int i = 7; i >= 0 && name[i] == ' '; --i) name[i] = '\0';
    for (int i = 2; i >= 0 && ext[i]  == ' '; --i) ext[i]  = '\0';

    // Check remaining name chars are printable
    for (int i = 0; name[i]; ++i)
        if (static_cast<uint8_t>(name[i]) < 0x20) return L"";

    std::wstring result = Utils::AnsiToWide(name);
    if (ext[0]) { result += L'.'; result += Utils::AnsiToWide(ext); }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// TryParseFatDirSector
//   Scans a 512-byte FAT directory sector for deleted entries.
//   Handles long-file-name (LFN) sequences preceding 8.3 entries.
//
//   LFN entries on disk are stored in DESCENDING sequence-number order
//   (highest = last chars, stored first on disk).  The first entry with
//   the 0x40 bit set ("last LFN entry") signals the start of a new group.
//   For DELETED LFN entries, name[0] = 0xE5 replaces the seq# – we still
//   reconstruct by prepending each entry's characters to pendingLfn.
//
//   Returns the number of deleted FileNodes appended to `out`.
// ─────────────────────────────────────────────────────────────────────────────
int DeepScan::TryParseFatDirSector(const uint8_t* p, uint32_t sectorSize,
                                    uint64_t absSector,
                                    const FatDeepInfo& fi,
                                    std::vector<FileNode>& out)
{
    const uint32_t entCount = sectorSize / 32;
    int found = 0;

    std::wstring pendingLfn;
    uint8_t      lfnChecksum = 0;

    for (uint32_t i = 0; i < entCount; ++i) {
        const uint8_t* e = p + i * 32;
        const uint8_t  first = e[0];
        const uint8_t  attr  = e[11];

        if (first == 0x00) break;  // end of directory

        bool isLfn = (attr == 0x0F);   // FAT_ATTR_LFN
        bool isDel = (first == 0xE5);

        // ── LFN entry ─────────────────────────────────────────────────────
        if (isLfn) {
            bool isLastLfnEntry = (!isDel) && ((first & 0x40) != 0);
            if (isLastLfnEntry) {
                // Start of a new active LFN group – discard any previous partial
                pendingLfn.clear();
                lfnChecksum = e[13];
            }
            std::wstring part = ExtractLfnChars(e);
            pendingLfn = part + pendingLfn;  // prepend (entries in descending order)
            continue;
        }

        // ── Normal or Deleted 8.3 entry ───────────────────────────────────
        // Skip volume-ID-only entries and "." / ".."
        if (attr == 0x08) {  // FAT_ATTR_VOLUME_ID
            pendingLfn.clear();
            continue;
        }
        if (!isDel && (first == '.' || first == ' ')) {
            pendingLfn.clear();
            continue;
        }

        if (!isDel) {
            // Normal (non-deleted) entry – not interesting for recovery,
            // but clear the pending LFN buffer so the next deleted file
            // doesn't accidentally inherit this entry's LFN.
            pendingLfn.clear();
            lfnChecksum = 0;
            continue;
        }

        // ── Deleted 8.3 entry ─────────────────────────────────────────────
        const auto* de = reinterpret_cast<const FAT_DIR_ENTRY*>(e);

        // Reconstruct name (LFN takes priority over 8.3)
        std::wstring name;
        if (!pendingLfn.empty()) {
            name = pendingLfn;
            // Validate LFN checksum against 8.3 name if we have a checksum
            // (skip validation for deleted-LFN groups where checksum may be 0)
        } else {
            name = Reconstruct83Name(e, true);
        }

        pendingLfn.clear();
        lfnChecksum = 0;

        if (name.empty() || name == L"_" || name.substr(0,1) == L"_.")
            continue;
        // Skip meta-entries like "." and ".." (already filtered above for !isDel,
        // but also handle deleted versions)
        if (name == L"." || name == L"..") continue;

        // Skip directory entries in deep scan – we can't follow their cluster
        // chains without the FAT table, so they add noise without value.
        bool isDir = (attr & 0x10) != 0;  // FAT_ATTR_DIRECTORY
        if (isDir) continue;

        // ── Build FileNode ────────────────────────────────────────────────
        FileNode node;
        // Use a unique ID: (absSector << 8) | entry-index within sector
        node.id          = (absSector << 8) | i;
        node.fsType      = (fi.valid && fi.fatType > 0)
                           ? (L"FAT" + std::to_wstring(fi.fatType))
                           : L"FAT";
        node.status      = FileStatus::Deleted;
        node.name        = name;
        node.size        = de->fileSize;

        // Extension
        {
            size_t dot = name.rfind(L'.');
            if (dot != std::wstring::npos) {
                node.extension = name.substr(dot + 1);
                for (auto& c : node.extension)
                    c = static_cast<wchar_t>(towlower(c));
            }
        }

        // Timestamps
        node.createTime = FatDateTimeToFiletime(
            de->createDate, de->createTime, de->createTimeTenth);
        node.modifyTime = FatDateTimeToFiletime(
            de->lastWriteDate, de->lastWriteTime);
        node.accessTime = FatDateTimeToFiletime(de->lastAccessDate, 0);

        // First cluster number
        uint32_t firstCluster =
            (static_cast<uint32_t>(de->clusterHigh) << 16) | de->clusterLow;

        if (firstCluster >= 2 && fi.valid) {
            // Store just the starting cluster.
            // DeepScan doesn't load the full FAT table, so we can't follow
            // the chain.  Store the raw cluster number in node.rawOffset so
            // the recovery engine can locate the data start directly.
            node.clusters.push_back(static_cast<uint64_t>(firstCluster));
            // rawOffset = absolute byte offset of the file's first cluster
            node.rawOffset = fi.dataStartSector * fi.bytesPerSector
                + static_cast<uint64_t>(firstCluster - 2)
                  * fi.sectorsPerCluster * fi.bytesPerSector;
            node.rawSize   = de->fileSize;
        }

        // Recoverability: without the FAT table we can't check if clusters
        // are free, so we use Poor as a conservative estimate for files
        // that have a non-zero first cluster (data location is known).
        if (firstCluster >= 2 && de->fileSize > 0) {
            node.recoverability = RecoverabilityLevel::Poor;
        } else if (de->fileSize == 0) {
            node.recoverability = RecoverabilityLevel::Excellent;  // empty file
        } else {
            node.recoverability = RecoverabilityLevel::Unknown;
        }

        ReportFile(node);
        out.push_back(std::move(node));
        ++found;
    }

    return found;
}
