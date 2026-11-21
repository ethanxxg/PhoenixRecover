#include "FatParser.h"
#include "../../common/Utils.h"
#include "../../common/Constants.h"
#include "../../log/Logger.h"
#include "../../recover/FragmentStitchService.h"
#include <algorithm>
#include <cassert>
#include <unordered_set>

namespace {

static bool IsValidFat83Char(unsigned char c)
{
    return c >= 0x20 && c != 0x7F;
}

static bool IsValidFat83Components(const char* name8, const char* ext3)
{
    if (!name8 || !name8[0])
        return false;
    for (int i = 0; i < 8 && name8[i]; ++i) {
        if (name8[i] == ' ')
            continue;
        if (!IsValidFat83Char(static_cast<unsigned char>(name8[i])))
            return false;
    }
    for (int i = 0; i < 3 && ext3 && ext3[i]; ++i) {
        if (ext3[i] == ' ')
            continue;
        if (!IsValidFat83Char(static_cast<unsigned char>(ext3[i])))
            return false;
    }
    return true;
}

static bool IsValidDisplayName(const std::wstring& name)
{
    if (name.empty())
        return false;
    for (wchar_t c : name) {
        if (c < 32 || c == 0x7F)
            return false;
    }
    return true;
}

static bool IsPlausibleFatFileSize(uint64_t size, uint64_t volumeBytes,
                                   const std::vector<uint64_t>& clusters,
                                   uint32_t sectorsPerCluster, uint32_t bytesPerSector)
{
    if (size == 0)
        return true;
    if (volumeBytes > 0 && size > volumeBytes)
        return false;
    if (!clusters.empty() && sectorsPerCluster > 0 && bytesPerSector > 0) {
        const uint64_t chainBytes =
            clusters.size() * static_cast<uint64_t>(sectorsPerCluster) * bytesPerSector;
        if (size > chainBytes && volumeBytes > 0 && size > volumeBytes / 4)
            return false;
    }
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Open – read BPB, determine FAT type, load FAT into RAM
// ─────────────────────────────────────────────────────────────────────────────
bool FatParser::Open(SectorReader& reader, const Partition& partition)
{
    m_reader = &reader;
    m_part   = partition;
    m_valid  = false;
    m_files.clear();
    m_fatTable.clear();
    m_nextId = 1;

    if (!ReadBPB()) return false;

    // Load the first FAT into RAM (for fast cluster chain walking)
    uint64_t fatByteSize =
        static_cast<uint64_t>(m_fatSizeSectors) * m_bytesPerSector;

    constexpr uint64_t kMaxFatRam = 128ULL * 1024 * 1024; // 128 MB
    if (fatByteSize > kMaxFatRam) {
        LOG_WARN(L"FatParser: FAT too large to load fully (" +
                 std::to_wstring(fatByteSize) + L" B), using read-through");
        fatByteSize = 0; // Will do sector reads on demand
    }

    if (fatByteSize > 0) {
        m_fatTable.resize(static_cast<size_t>(fatByteSize));
        if (!reader.ReadBytes(m_fatStartSector * m_bytesPerSector,
                              m_fatTable.data(),
                              static_cast<uint32_t>(fatByteSize))) {
            LOG_WARN(L"FatParser: failed to read FAT table");
            m_fatTable.clear(); // proceed without cache
        }
    }

    m_valid = true;
    LOG_INFO(L"FatParser::Open ok – FAT" + std::to_wstring(m_fatType) +
             L"  clusters=" + std::to_wstring(m_totalClusters));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
bool FatParser::ReadBPB()
{
    uint32_t ss = m_reader->GetSectorSize();
    if (ss < 512) ss = 512;

    std::vector<uint8_t> buf(ss);
    if (!m_reader->ReadSectors(m_part.startSector, buf.data(), 1)) {
        LOG_ERROR(L"FatParser: cannot read boot sector");
        return false;
    }

    auto* common = reinterpret_cast<FAT_BPB_COMMON*>(buf.data());

    m_bytesPerSector    = common->bytesPerSector;
    m_sectorsPerCluster = common->sectorsPerCluster;
    m_reservedSectors   = common->reservedSectors;
    m_numFATs           = common->numFATs;

    if (m_bytesPerSector == 0 || m_sectorsPerCluster == 0) {
        LOG_ERROR(L"FatParser: invalid BPB fields");
        return false;
    }

    // FAT size
    m_fatSizeSectors = (common->fatSize16 != 0)
                       ? common->fatSize16
                       : reinterpret_cast<FAT32_BPB*>(buf.data())->fatSize32;

    if (m_fatSizeSectors == 0) return false;

    // Absolute start sectors (relative to physical disk)
    m_fatStartSector = m_part.startSector + m_reservedSectors;

    // Root directory
    uint32_t rootDirEntries = common->rootEntryCount;
    m_rootDirSectors =
        (rootDirEntries * 32 + m_bytesPerSector - 1) / m_bytesPerSector;
    m_rootDirStartSector =
        m_fatStartSector + static_cast<uint64_t>(m_numFATs) * m_fatSizeSectors;

    // Data area starts after root dir
    m_dataStartSector = m_rootDirStartSector + m_rootDirSectors;

    // Total sectors
    uint32_t totalSectors = (common->totalSectors16 != 0)
                            ? common->totalSectors16
                            : common->totalSectors32;
    uint64_t dataSectors  = (totalSectors > 0)
                            ? totalSectors - (m_dataStartSector - m_part.startSector)
                            : 0;
    m_totalClusters = dataSectors / m_sectorsPerCluster;

    // Determine FAT type from cluster count (per FAT spec)
    if (common->fatSize16 == 0) {
        // FAT32 extended BPB
        m_fatType     = 32;
        m_rootCluster = reinterpret_cast<FAT32_BPB*>(buf.data())->rootCluster;
        // Verify "FAT32   " signature
        if (memcmp(buf.data() + 82, "FAT32   ", 8) != 0) {
            LOG_WARN(L"FatParser: FAT32 signature missing");
        }
    } else if (m_totalClusters < 4085) {
        m_fatType = 12;
    } else if (m_totalClusters < 65525) {
        m_fatType = 16;
    } else {
        m_fatType = 32;
    }

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
std::wstring FatParser::GetFsTypeName() const
{
    switch (m_fatType) {
    case 12: return L"FAT12";
    case 16: return L"FAT16";
    default: return L"FAT32";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ScanMetadata – walk directory tree
// ─────────────────────────────────────────────────────────────────────────────
bool FatParser::ScanMetadata(std::function<void(int)> onProgress,
                              std::atomic<bool>*       cancelFlag)
{
    if (!m_valid) return false;

    if (m_fatType == 32) {
        ScanDirectory(m_rootCluster, L"\\", cancelFlag, 0);
    } else {
        // FAT12/16: root directory is at fixed sectors
        // We'll scan FAT16 root dir as a "cluster 0" special case
        // Use cluster 0 as sentinel meaning "fixed root dir"
        ScanDirectory(0, L"\\", cancelFlag, 0);
    }

    // Fallback: try to find orphaned directory entries in the data area and
    // attach them under "\LostDir\" so the user can inspect them.
    ScanOrphanDirEntries(cancelFlag);

    if (onProgress) onProgress(100);

    LOG_INFO(L"FatParser::ScanMetadata complete – found " +
             std::to_wstring(m_files.size()) + L" entries");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ScanDirectory – recursively scan a directory
// ─────────────────────────────────────────────────────────────────────────────
void FatParser::ScanDirectory(uint32_t startCluster, const std::wstring& path,
                               std::atomic<bool>* cancelFlag,
                               int depth)
{
    if (cancelFlag && cancelFlag->load()) return;
    if (depth > 16) return; // Safety: avoid deep / cyclic directory recursion

    // Read directory sectors
    uint64_t dirStartSector;
    uint64_t totalDirSectors;

    if (startCluster == 0 && m_fatType != 32) {
        // FAT16/12 fixed root directory
        dirStartSector  = m_rootDirStartSector;
        totalDirSectors = m_rootDirSectors;
    } else {
        dirStartSector  = ClusterToAbsoluteSector(startCluster);
        totalDirSectors = m_sectorsPerCluster;
    }

    std::vector<uint8_t> buf(m_bytesPerSector);

    // Pending LFN components (up to 20 × 13 = 260 chars)
    std::wstring pendingLfn;
    uint8_t      pendingChecksum = 0;
    bool         haveLfn         = false;

    // Helper: append LFN name part
    auto appendLfnPart = [](std::wstring& dst, const FAT_LFN_ENTRY* lfn) {
        for (auto c : lfn->name1) { if (c == 0xFFFF || c == 0) return; dst += static_cast<wchar_t>(c); }
        for (auto c : lfn->name2) { if (c == 0xFFFF || c == 0) return; dst += static_cast<wchar_t>(c); }
        for (auto c : lfn->name3) { if (c == 0xFFFF || c == 0) return; dst += static_cast<wchar_t>(c); }
    };

    // Build short name (8.3 → string, skip spaces)
    auto buildShortName = [](const FAT_DIR_ENTRY* e) -> std::wstring {
        // First character: 0xE5 means deleted, we'll handle that outside
        char n[9]{};
        char x[4]{};
        memcpy(n, e->name, 8); n[8] = 0;
        memcpy(x, e->ext,  3); x[3] = 0;
        // Trim trailing spaces
        for (int i = 7; i >= 0 && n[i] == ' '; --i) n[i] = 0;
        for (int i = 2; i >= 0 && x[i] == ' '; --i) x[i] = 0;
        std::wstring name = Utils::AnsiToWide(n);
        if (x[0]) { name += L'.'; name += Utils::AnsiToWide(x); }
        return name;
    };

    // Iterate: follow cluster chain for the directory
    uint32_t cluster = startCluster;
    bool     done    = false;

    while (!done) {
        for (uint64_t s = 0; s < totalDirSectors && !done; ++s) {
            if (!m_reader->ReadSectors(dirStartSector + s, buf.data(), 1)) break;

            uint32_t entCount = m_bytesPerSector / 32;
            for (uint32_t e = 0; e < entCount; ++e) {
                auto* raw = reinterpret_cast<FAT_DIR_ENTRY*>(buf.data() + e * 32);

                if (raw->name[0] == FAT_FREE_MARKER) {
                    done = true; // No more entries
                    break;
                }

                // Volume label or LFN entry
                if (raw->attr == FAT_ATTR_VOLUME_ID) { haveLfn = false; pendingLfn.clear(); continue; }

                if (raw->attr == FAT_ATTR_LFN) {
                    auto* lfn = reinterpret_cast<FAT_LFN_ENTRY*>(raw);
                    if (lfn->order & 0x40) {
                        // Last LFN entry (first in sequence on disk)
                        pendingLfn.clear();
                        pendingChecksum = lfn->checksum;
                        haveLfn = true;
                    }
                    std::wstring part;
                    appendLfnPart(part, lfn);
                    pendingLfn = part + pendingLfn; // Prepend (entries are reverse order)
                    continue;
                }

                bool deleted = (static_cast<uint8_t>(raw->name[0]) == FAT_DELETED_MARKER);

                // Skip "." and ".."
                if (!deleted && (raw->name[0] == '.' )) { haveLfn = false; pendingLfn.clear(); continue; }

                FileNode node;
                node.id          = m_nextId++;
                node.fsType      = GetFsTypeName();
                node.isDirectory = (raw->attr & FAT_ATTR_DIRECTORY) != 0;
                node.status      = deleted ? FileStatus::Deleted : FileStatus::Normal;

                // File name
                if (haveLfn && !pendingLfn.empty()) {
                    node.name = pendingLfn;
                } else {
                    // Reconstruct 8.3 name; for deleted files name[0] was 0xE5
                    char tmpName[9]{};
                    memcpy(tmpName, raw->name, 8);
                    if (deleted) tmpName[0] = '_'; // Replace deleted marker
                    tmpName[8] = 0;
                    for (int i = 7; i >= 0 && tmpName[i] == ' '; --i) tmpName[i] = 0;

                    char tmpExt[4]{};
                    memcpy(tmpExt, raw->ext, 3); tmpExt[3] = 0;
                    for (int i = 2; i >= 0 && tmpExt[i] == ' '; --i) tmpExt[i] = 0;

                    node.name = Utils::AnsiToWide(tmpName);
                    if (tmpExt[0]) { node.name += L'.'; node.name += Utils::AnsiToWide(tmpExt); }
                }

                haveLfn = false;
                pendingLfn.clear();

                if (node.name.empty() || node.name == L"." || node.name == L"..") continue;

                if (!haveLfn) {
                    char chkName[9]{}, chkExt[4]{};
                    memcpy(chkName, raw->name, 8);
                    if (deleted) chkName[0] = '_';
                    chkName[8] = 0;
                    for (int i = 7; i >= 0 && chkName[i] == ' '; --i) chkName[i] = 0;
                    memcpy(chkExt, raw->ext, 3);
                    chkExt[3] = 0;
                    for (int i = 2; i >= 0 && chkExt[i] == ' '; --i) chkExt[i] = 0;
                    if (!IsValidFat83Components(chkName, chkExt))
                        continue;
                } else if (!IsValidDisplayName(node.name)) {
                    continue;
                }

                node.originalPath = path + node.name;
                if (node.isDirectory) node.originalPath += L'\\';

                // File size
                node.size = raw->fileSize;

                // Timestamps
                node.createTime = FatDateTimeToFiletime(raw->createDate,
                                                        raw->createTime,
                                                        raw->createTimeTenth);
                node.modifyTime = FatDateTimeToFiletime(raw->lastWriteDate,
                                                        raw->lastWriteTime);
                node.accessTime = FatDateTimeToFiletime(raw->lastAccessDate, 0);

                // Extension
                size_t dot = node.name.rfind(L'.');
                if (dot != std::wstring::npos) {
                    node.extension = node.name.substr(dot + 1);
                    for (auto& c : node.extension) c = static_cast<wchar_t>(towlower(c));
                }

                // First cluster
                uint32_t firstCluster =
                    (static_cast<uint32_t>(raw->clusterHigh) << 16) | raw->clusterLow;

                if (!node.isDirectory && firstCluster >= 2) {
                    // Pass fileSize for deleted files so BuildClusterChain
                    // can extend the chain sequentially when FAT entries
                    // have been zeroed (chain broken after deletion).
                    node.clusters = BuildClusterChain(firstCluster,
                                        deleted ? node.size : 0,
                                        node.extension);
                }

                if (!node.isDirectory) {
                    const uint64_t volBytes = m_part.sizeBytes > 0
                        ? m_part.sizeBytes
                        : m_totalClusters * m_sectorsPerCluster * m_bytesPerSector;
                    if (!IsPlausibleFatFileSize(node.size, volBytes, node.clusters,
                                                m_sectorsPerCluster, m_bytesPerSector))
                        continue;
                }

                // Assess recoverability by sampling first cluster
                node.recoverability = RecoverabilityLevel::Unknown;
                if (deleted && firstCluster >= 2 && node.size > 0) {
                    uint64_t absSec = ClusterToAbsoluteSector(firstCluster);
                    uint8_t sample[512]{};
                    if (m_reader->ReadSectors(absSec, sample, 1)) {
                        bool allZero = true;
                        for (int i = 0; i < 512; ++i) {
                            if (sample[i] != 0) { allZero = false; break; }
                        }
                        node.recoverability = allZero ? RecoverabilityLevel::Lost
                                                      : RecoverabilityLevel::Good;
                    }
                }

                // Capture info needed for recursion BEFORE push_back
                // to avoid dangling references after potential vector reallocation.
                bool         isDir    = node.isDirectory;
                std::wstring dirPath  = node.originalPath;
                uint32_t     fc       = firstCluster;
                // For deleted directories, FAT chain is often broken, but the first
                // cluster may still contain valid entries (DFTT/CFReDS datasets).
                // Recurse with a depth limit to avoid runaway scans on garbage.
                bool         willRec  = isDir && fc >= 2 && depth < 8;

                FileNode emittedNode = node;
                m_files.push_back(std::move(node));
                if (m_onFileFound) {
                    m_onFileFound(emittedNode);
                }

                // Recurse into subdirectory (after push_back so the dir appears in results)
                if (willRec)
                    ScanDirectory(fc, dirPath, cancelFlag, depth + 1);
            }
        }

        if (startCluster == 0 && m_fatType != 32) break; // Fixed root, no chain

        // Follow cluster chain
        uint32_t next = NextCluster(cluster);
        if (next == 0 || next >= (FAT32_EOC_MIN & FAT32_MASK)) break;
        cluster         = next;
        dirStartSector  = ClusterToAbsoluteSector(cluster);
        totalDirSectors = m_sectorsPerCluster;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// ScanOrphanDirEntries
//   Linear scan of a limited portion of the data area to find directory
//   entries (deleted files) that are no longer reachable via the normal
//   directory tree.  Results are placed under "\LostDir\".
// ─────────────────────────────────────────────────────────────────────────────
void FatParser::ScanOrphanDirEntries(std::atomic<bool>* cancelFlag)
{
    if (!m_reader || m_totalClusters == 0) return;

    // Build a set of first clusters already accounted for via normal scan,
    // so we don't duplicate entries.
    std::unordered_set<uint32_t> usedFirstClusters;
    usedFirstClusters.reserve(m_files.size() * 2);
    for (const auto& f : m_files) {
        if (!f.clusters.empty()) {
            uint64_t sec = f.clusters.front();
            if (sec >= m_dataStartSector) {
                uint64_t rel = sec - m_dataStartSector;
                uint64_t perClus = m_sectorsPerCluster ? m_sectorsPerCluster : 1;
                uint64_t c = rel / perClus + 2;
                if (c <= 0xFFFFFFFFu)
                    usedFirstClusters.insert(static_cast<uint32_t>(c));
            }
        }
    }

    uint64_t bytesPerCluster =
        static_cast<uint64_t>(m_sectorsPerCluster) * m_bytesPerSector;
    if (bytesPerCluster == 0) return;

    // Limit scan to first ~64 MB of data area to keep runtime bounded.
    uint64_t maxScanBytes = std::min<uint64_t>(
        m_totalClusters * bytesPerCluster, 64ull * 1024 * 1024);
    if (maxScanBytes < bytesPerCluster) return;

    uint64_t maxScanSectors = maxScanBytes / m_bytesPerSector;
    uint64_t startSector    = m_dataStartSector;

    std::vector<uint8_t> buf(m_bytesPerSector);
    const std::wstring lostRoot = L"\\LostDir\\";

    for (uint64_t s = 0; s < maxScanSectors; ++s) {
        if (cancelFlag && cancelFlag->load()) break;

        if (!m_reader->ReadSectors(startSector + s, buf.data(), 1))
            continue;

        uint32_t entCount = m_bytesPerSector / 32;
        for (uint32_t e = 0; e < entCount; ++e) {
            auto* raw = reinterpret_cast<FAT_DIR_ENTRY*>(buf.data() + e * 32);

            if (raw->name[0] == FAT_FREE_MARKER) continue;
            if (raw->attr == FAT_ATTR_VOLUME_ID) continue;
            if (raw->attr == FAT_ATTR_LFN)       continue;

            bool deleted = (static_cast<uint8_t>(raw->name[0]) == FAT_DELETED_MARKER);
            if (!deleted) continue; // 只关心已删除但可能仍然存在的数据

            // Reconstruct short name; ignore "." / ".."
            char n[9]{}, x[4]{};
            memcpy(n, raw->name, 8); n[8] = 0;
            memcpy(x, raw->ext,  3); x[3] = 0;
            for (int i = 7; i >= 0 && n[i] == ' '; --i) n[i] = 0;
            for (int i = 2; i >= 0 && x[i] == ' '; --i) x[i] = 0;
            if (n[0] == 0 || (n[0] == '.' && (n[1] == 0 || n[1] == '.')))
                continue;

            std::wstring name = Utils::AnsiToWide(n);
            if (x[0]) { name += L'.'; name += Utils::AnsiToWide(x); }
            if (name.empty()) continue;
            {
                char vn[9]{}, vx[4]{};
                memcpy(vn, n, 9);
                memcpy(vx, x, 4);
                if (static_cast<unsigned char>(vn[0]) == FAT_DELETED_MARKER)
                    vn[0] = '_';
                if (!IsValidFat83Components(vn, vx))
                    continue;
            }

            uint32_t firstCluster =
                (static_cast<uint32_t>(raw->clusterHigh) << 16) | raw->clusterLow;
            if (firstCluster < 2 || firstCluster >= m_totalClusters + 2)
                continue;

            if (usedFirstClusters.find(firstCluster) != usedFirstClusters.end())
                continue;

            usedFirstClusters.insert(firstCluster);

            FileNode node;
            node.id          = m_nextId++;
            node.fsType      = GetFsTypeName();
            node.isDirectory = (raw->attr & FAT_ATTR_DIRECTORY) != 0;
            node.status      = FileStatus::Deleted;

            node.name         = name;
            node.originalPath = lostRoot + name;
            if (node.isDirectory) node.originalPath += L'\\';

            node.size = raw->fileSize;

            node.createTime = FatDateTimeToFiletime(raw->createDate,
                                                    raw->createTime,
                                                    raw->createTimeTenth);
            node.modifyTime = FatDateTimeToFiletime(raw->lastWriteDate,
                                                    raw->lastWriteTime);
            node.accessTime = FatDateTimeToFiletime(raw->lastAccessDate, 0);

            size_t dot = node.name.rfind(L'.');
            if (dot != std::wstring::npos) {
                node.extension = node.name.substr(dot + 1);
                for (auto& c : node.extension) c = static_cast<wchar_t>(towlower(c));
            }

            if (!node.isDirectory && firstCluster >= 2) {
                node.clusters = BuildClusterChain(firstCluster,
                                                  node.size > 0 ? node.size : 0,
                                                  node.extension);
            }

            if (!node.isDirectory) {
                const uint64_t volBytes = m_part.sizeBytes > 0
                    ? m_part.sizeBytes
                    : m_totalClusters * m_sectorsPerCluster * m_bytesPerSector;
                if (!IsPlausibleFatFileSize(node.size, volBytes, node.clusters,
                                            m_sectorsPerCluster, m_bytesPerSector))
                    continue;
            }

            node.recoverability = RecoverabilityLevel::Unknown;
            if (firstCluster >= 2 && node.size > 0) {
                uint64_t absSec = ClusterToAbsoluteSector(firstCluster);
                uint8_t sample[512]{};
                if (m_reader->ReadSectors(absSec, sample, 1)) {
                    bool allZero = true;
                    for (int i = 0; i < 512; ++i) {
                        if (sample[i] != 0) { allZero = false; break; }
                    }
                    node.recoverability = allZero ? RecoverabilityLevel::Lost
                                                  : RecoverabilityLevel::Good;
                }
            }

            FileNode emittedNode = node;
            m_files.push_back(std::move(node));
            if (m_onFileFound) {
                m_onFileFound(emittedNode);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
uint32_t FatParser::NextCluster(uint32_t cluster)
{
    if (cluster < 2) return 0;

    if (!m_fatTable.empty()) {
        if (m_fatType == 32) {
            uint64_t offset = static_cast<uint64_t>(cluster) * 4;
            if (offset + 4 > m_fatTable.size()) return 0;
            uint32_t val = Utils::ReadLE32(m_fatTable.data() + offset);
            val &= FAT32_MASK;
            return (val >= (FAT32_EOC_MIN & FAT32_MASK)) ? 0 : val;
        } else if (m_fatType == 16) {
            uint64_t offset = static_cast<uint64_t>(cluster) * 2;
            if (offset + 2 > m_fatTable.size()) return 0;
            uint16_t val = Utils::ReadLE16(m_fatTable.data() + offset);
            return (val >= 0xFFF8) ? 0 : val;
        } else { // FAT12
            uint64_t offset = static_cast<uint64_t>(cluster) * 3 / 2;
            if (offset + 2 > m_fatTable.size()) return 0;
            uint16_t val = Utils::ReadLE16(m_fatTable.data() + offset);
            if (cluster & 1) val >>= 4; else val &= 0x0FFF;
            return (val >= 0x0FF8) ? 0 : val;
        }
    }

    // Read-through fallback (slow)
    uint64_t byteOffset;
    if (m_fatType == 32)
        byteOffset = m_fatStartSector * m_bytesPerSector + cluster * 4ULL;
    else if (m_fatType == 16)
        byteOffset = m_fatStartSector * m_bytesPerSector + cluster * 2ULL;
    else
        byteOffset = m_fatStartSector * m_bytesPerSector + cluster * 3ULL / 2;

    uint8_t buf[4]{};
    if (!m_reader->ReadBytes(byteOffset, buf, 4)) return 0;

    if (m_fatType == 32) {
        uint32_t v = Utils::ReadLE32(buf) & FAT32_MASK;
        return (v >= (FAT32_EOC_MIN & FAT32_MASK)) ? 0 : v;
    } else if (m_fatType == 16) {
        uint16_t v = Utils::ReadLE16(buf);
        return (v >= 0xFFF8) ? 0 : v;
    } else {
        uint16_t v = Utils::ReadLE16(buf);
        if (cluster & 1) v >>= 4; else v &= 0x0FFF;
        return (v >= 0x0FF8) ? 0 : v;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
std::vector<uint64_t> FatParser::BuildClusterChain(uint32_t startCluster,
                                                     uint64_t fileSize,
                                                     const std::wstring& extension)
{
    std::vector<uint64_t> chain;
    uint32_t cur = startCluster;
    constexpr uint32_t kMaxChain = 1 << 24; // Safety limit

    // ── Phase 1: follow the actual FAT chain as far as it goes ────────────
    while (cur >= 2 && chain.size() < kMaxChain) {
        chain.push_back(ClusterToAbsoluteSector(cur));
        uint32_t next = NextCluster(cur);
        if (next == 0 || next == cur) {
            // EOC (next==0) or broken link. Only speculate sequential clusters
            // when the FAT chain is shorter than the directory entry size.
            if (fileSize > 0) {
                uint64_t bytesPerCluster =
                    static_cast<uint64_t>(m_sectorsPerCluster) * m_bytesPerSector;
                if (bytesPerCluster > 0) {
                    uint64_t clustersNeeded =
                        (fileSize + bytesPerCluster - 1) / bytesPerCluster;

                    if (chain.size() < clustersNeeded) {
                        // Phase 2: deleted file with zeroed FAT entries — assume
                        // contiguous run (fails on fragmented layouts).
                        uint32_t seqCur = cur + 1;
                        while (chain.size() < clustersNeeded &&
                               chain.size() < kMaxChain &&
                               seqCur >= 2 &&
                               seqCur < static_cast<uint32_t>(m_totalClusters + 2))
                        {
                            chain.push_back(ClusterToAbsoluteSector(seqCur));
                            ++seqCur;
                        }
                    }
                }
            }
            break;
        }
        cur = next;
    }

    if (fileSize > 0 && !chain.empty() && m_reader) {
        FatVolumeGeometry geo{};
        geo.bytesPerSector    = m_bytesPerSector;
        geo.sectorsPerCluster = m_sectorsPerCluster;
        geo.dataStartSector   = m_dataStartSector;
        geo.totalClusters     = static_cast<uint32_t>(m_totalClusters);
        geo.fatType           = m_fatType;

        FragmentStitchService::TryImproveFatClusterChain(
            *m_reader, geo, &m_fatTable, startCluster, fileSize, extension, chain,
            m_fragmentStitchPolicy);
    }

    return chain;
}

// ─────────────────────────────────────────────────────────────────────────────
uint64_t FatParser::ClusterToAbsoluteSector(uint32_t cluster) const
{
    return m_dataStartSector +
           static_cast<uint64_t>(cluster - 2) * m_sectorsPerCluster;
}
