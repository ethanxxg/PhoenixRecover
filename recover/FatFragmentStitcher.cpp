#include "FatFragmentStitcher.h"

#include "../common/Utils.h"
#include "../fs/exfat/ExfatStructs.h"
#include "../fs/fat/FatStructs.h"
#include "../log/Logger.h"

#include <algorithm>
#include <cstring>

#include <chrono>

namespace {

static std::wstring LowerExt(const std::wstring& ext)
{
    std::wstring out = ext;
    for (auto& c : out) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return out;
}

static int ScoreToInt(FormatPayloadScore s)
{
    return static_cast<int>(s);
}

} // namespace

FatFragmentStitcher::FatFragmentStitcher(SectorReader& reader,
                                         const FatVolumeGeometry& geo,
                                         const std::vector<uint8_t>* fatTableBytes)
    : m_reader(reader)
    , m_geo(geo)
{
    if (fatTableBytes && !fatTableBytes->empty()) {
        m_fatTable = *fatTableBytes;
    }
}

bool FatFragmentStitcher::LoadFatTable(uint64_t fatStartSector, uint32_t fatSizeSectors)
{
    if (!m_fatTable.empty()) return true;

    const uint64_t fatBytes =
        static_cast<uint64_t>(fatSizeSectors) * m_geo.bytesPerSector;
    constexpr uint64_t kMaxFatRam = 128ULL * 1024 * 1024;
    if (fatBytes == 0 || fatBytes > kMaxFatRam) return false;

    m_fatTable.resize(static_cast<size_t>(fatBytes));
    const uint32_t ss = m_reader.GetSectorSize() ? m_reader.GetSectorSize() : 512;
    if (!m_reader.ReadBytes(fatStartSector * ss, m_fatTable.data(),
                            static_cast<uint32_t>(fatBytes))) {
        m_fatTable.clear();
        return false;
    }
    return true;
}

uint64_t FatFragmentStitcher::ClusterToAbsSector(uint32_t cluster) const
{
    return m_geo.dataStartSector +
           static_cast<uint64_t>(cluster - 2) * m_geo.sectorsPerCluster;
}

uint32_t FatFragmentStitcher::NextCluster(uint32_t cluster) const
{
    if (cluster < 2 || m_fatTable.empty()) return 0;

    if (m_geo.fatType == 32) {
        const uint64_t off = static_cast<uint64_t>(cluster) * 4;
        if (off + 4 > m_fatTable.size()) return 0;
        uint32_t val = Utils::ReadLE32(m_fatTable.data() + off) & FAT32_MASK;
        return (val >= (FAT32_EOC_MIN & FAT32_MASK)) ? 0 : val;
    }
    if (m_geo.fatType == 16) {
        const uint64_t off = static_cast<uint64_t>(cluster) * 2;
        if (off + 2 > m_fatTable.size()) return 0;
        uint16_t val = Utils::ReadLE16(m_fatTable.data() + off);
        return (val >= 0xFFF8) ? 0 : val;
    }
    // FAT12: rare for fragment stitch; treat as broken
    return 0;
}

bool FatFragmentStitcher::IsFreeCluster(uint32_t cluster) const
{
    if (cluster < 2 || m_fatTable.empty()) return false;

    if (m_geo.fatType == 32) {
        const uint64_t off = static_cast<uint64_t>(cluster) * 4;
        if (off + 4 > m_fatTable.size()) return false;
        return (Utils::ReadLE32(m_fatTable.data() + off) & FAT32_MASK) == 0;
    }
    if (m_geo.fatType == 16) {
        const uint64_t off = static_cast<uint64_t>(cluster) * 2;
        if (off + 2 > m_fatTable.size()) return false;
        return Utils::ReadLE16(m_fatTable.data() + off) == 0;
    }
    return false;
}

bool FatFragmentStitcher::ReadChainBytes(const std::vector<uint32_t>& clusterNums,
                                          uint64_t fileSize,
                                          std::vector<uint8_t>& out) const
{
    const uint64_t clusterBytes =
        static_cast<uint64_t>(m_geo.bytesPerSector) * m_geo.sectorsPerCluster;
    if (clusterBytes == 0 || clusterNums.empty()) return false;

    const uint64_t cap = clusterNums.size() * clusterBytes;
    out.resize(static_cast<size_t>(std::min(fileSize, cap)));
    if (out.empty()) return true;

    const uint32_t ss = m_reader.GetSectorSize() ? m_reader.GetSectorSize() : 512;
    uint64_t written = 0;
    std::vector<uint8_t> clusterBuf(static_cast<size_t>(clusterBytes));

    for (uint32_t c : clusterNums) {
        if (written >= fileSize) break;
        const uint64_t off = ClusterToAbsSector(c) * ss;
        if (!m_reader.ReadBytes(off, clusterBuf.data(),
                              static_cast<uint32_t>(clusterBytes))) {
            return false;
        }
        const uint64_t take = std::min(clusterBytes, fileSize - written);
        memcpy(out.data() + written, clusterBuf.data(), static_cast<size_t>(take));
        written += take;
    }
    return true;
}

int FatFragmentStitcher::ScoreChain(const std::vector<uint32_t>& clusterNums,
                                      uint64_t fileSize,
                                      const std::wstring& extLower) const
{
    std::vector<uint8_t> buf;
    if (!ReadChainBytes(clusterNums, fileSize, buf)) return -1;
  return ScoreToInt(FormatPayloadScoreUtil::Score(extLower, buf.data(), buf.size()));
}

bool FatFragmentStitcher::SearchAndPickBest(
    uint32_t startCluster,
    uint64_t fileSize,
    const std::wstring& extLower,
    uint32_t clustersNeeded,
    const std::vector<uint32_t>& baseClusterNums,
    int baseScore,
    std::vector<uint32_t>& outBestClusters) const
{
    if (m_fatTable.empty() || clustersNeeded <= 1) return false;

    const uint32_t maxCluster = m_geo.totalClusters + 2;
    const uint32_t remaining = clustersNeeded - 1;

    // Bound search to keep recovery responsive on large volumes.
    // This is a heuristic best-effort stitcher, not an exhaustive solver.
    const uint32_t win = 4096;
    const uint32_t winStart = (startCluster > win) ? (startCluster - win) : 2;
    const uint32_t winEnd = std::min(maxCluster, startCluster + win);
    const auto budgetStart = std::chrono::steady_clock::now();
    constexpr int64_t kBudgetMs = 35;
    constexpr int kMaxTryChains = 1200;
    int tryChains = 0;

    auto isFreeRun = [&](uint32_t start, uint32_t len) -> bool {
        if (start < 2 || start + len - 1 > maxCluster) return false;
        for (uint32_t k = 0; k < len; ++k) {
            if (!IsFreeCluster(start + k)) return false;
        }
        return true;
    };

    int bestScore = baseScore;
    std::vector<uint32_t> best = baseClusterNums;
    bool improved = false;

    auto tryChain = [&](const std::vector<uint32_t>& chain) {
        if (chain.size() != clustersNeeded) return;
        if (tryChains++ >= kMaxTryChains) return;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - budgetStart).count() >= kBudgetMs) return;
        const int sc = ScoreChain(chain, fileSize, extLower);
        if (sc > bestScore) {
            bestScore = sc;
            best = chain;
            improved = true;
        }
    };

    // Phase A: [start] + free run
    for (uint32_t runStart = winStart; runStart + remaining - 1 <= winEnd; ++runStart) {
        if (runStart == startCluster) continue;
        if (!isFreeRun(runStart, remaining)) continue;
        std::vector<uint32_t> chain;
        chain.reserve(clustersNeeded);
        chain.push_back(startCluster);
        for (uint32_t k = 0; k < remaining; ++k) chain.push_back(runStart + k);
        tryChain(chain);
        if (bestScore >= ScoreToInt(FormatPayloadScore::Complete)) break;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - budgetStart).count() >= kBudgetMs) break;
    }

    // Phase B: [start] + contiguous (start+1..) + free run
    for (uint32_t a = 1; a < remaining; ++a) {
        const uint32_t run1Start = startCluster + 1;
        const uint32_t run1Len = a;
        const uint32_t run2Len = remaining - a;
        if (!isFreeRun(run1Start, run1Len)) continue;

        for (uint32_t run2Start = winStart; run2Start + run2Len - 1 <= winEnd; ++run2Start) {
            if (run2Start == startCluster) continue;
            if (!isFreeRun(run2Start, run2Len)) continue;
            std::vector<uint32_t> chain;
            chain.reserve(clustersNeeded);
            chain.push_back(startCluster);
            for (uint32_t k = 0; k < run1Len; ++k) chain.push_back(run1Start + k);
            for (uint32_t k = 0; k < run2Len; ++k) chain.push_back(run2Start + k);
            tryChain(chain);
            if (bestScore >= ScoreToInt(FormatPayloadScore::Complete)) break;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - budgetStart).count() >= kBudgetMs) break;
        }
        if (bestScore >= ScoreToInt(FormatPayloadScore::Complete)) break;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - budgetStart).count() >= kBudgetMs) break;
    }

    // Phase C: bounded 3-run search near start cluster (same window)
    for (uint32_t a = 1; a < remaining; ++a) {
        const uint32_t run1Len = a;
        const uint32_t run2Len = remaining - a;

        for (uint32_t run1Start = winStart; run1Start + run1Len - 1 <= winEnd; ++run1Start) {
            if (run1Start == startCluster) continue;
            if (!isFreeRun(run1Start, run1Len)) continue;

            for (uint32_t run2Start = winStart; run2Start + run2Len - 1 <= winEnd; ++run2Start) {
                if (run2Start == startCluster) continue;
                if (!isFreeRun(run2Start, run2Len)) continue;

                std::vector<uint32_t> chain;
                chain.reserve(clustersNeeded);
                chain.push_back(startCluster);
                for (uint32_t k = 0; k < run1Len; ++k) chain.push_back(run1Start + k);
                for (uint32_t k = 0; k < run2Len; ++k) chain.push_back(run2Start + k);
                tryChain(chain);
                if (bestScore >= ScoreToInt(FormatPayloadScore::Complete)) break;
                if (std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - budgetStart).count() >= kBudgetMs) break;
            }
            if (bestScore >= ScoreToInt(FormatPayloadScore::Complete)) break;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - budgetStart).count() >= kBudgetMs) break;
        }
        if (bestScore >= ScoreToInt(FormatPayloadScore::Complete)) break;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - budgetStart).count() >= kBudgetMs) break;
    }

    if (!improved) return false;
    outBestClusters = std::move(best);
    return true;
}

bool FatFragmentStitcher::TryImprove(uint32_t startCluster,
                                      uint64_t fileSize,
                                      const std::wstring& extension,
                                      std::vector<uint64_t>& absSectorChain)
{
    if (fileSize == 0 || absSectorChain.empty() || m_fatTable.empty()) return false;

    std::wstring extLower = LowerExt(extension);
    if (extLower.empty()) {
        std::vector<uint8_t> probe(512);
        const uint32_t ss = m_reader.GetSectorSize() ? m_reader.GetSectorSize() : 512;
        if (m_reader.ReadBytes(absSectorChain[0] * ss, probe.data(), 512)) {
            extLower = FormatPayloadScoreUtil::ProbeExtension(probe.data(), 512);
        }
    }
    if (!FormatPayloadScoreUtil::SupportsFragmentStitch(extLower)) return false;

    const uint64_t clusterBytes =
        static_cast<uint64_t>(m_geo.bytesPerSector) * m_geo.sectorsPerCluster;
    if (clusterBytes == 0) return false;

    const uint32_t clustersNeeded =
        static_cast<uint32_t>((fileSize + clusterBytes - 1) / clusterBytes);
    if (clustersNeeded <= 1) return false;

    std::vector<uint32_t> baseClusters;
    baseClusters.reserve(absSectorChain.size());
    for (uint64_t absSec : absSectorChain) {
        if (absSec < m_geo.dataStartSector) continue;
        const uint64_t rel = absSec - m_geo.dataStartSector;
        baseClusters.push_back(
            static_cast<uint32_t>(rel / m_geo.sectorsPerCluster + 2));
    }
    if (baseClusters.empty()) baseClusters.push_back(startCluster);

    while (baseClusters.size() < clustersNeeded) {
        uint32_t next = baseClusters.back() + 1;
        if (next >= m_geo.totalClusters + 2) break;
        baseClusters.push_back(next);
    }
    if (baseClusters.size() > clustersNeeded) {
        baseClusters.resize(clustersNeeded);
    }

    const int baseScore = ScoreChain(baseClusters, fileSize, extLower);
    if (baseScore >= ScoreToInt(FormatPayloadScore::Complete)) return false;

    std::vector<uint32_t> bestClusters;
    if (!SearchAndPickBest(startCluster, fileSize, extLower, clustersNeeded,
                           baseClusters, baseScore, bestClusters)) {
        return false;
    }

    absSectorChain.clear();
    absSectorChain.reserve(bestClusters.size());
    for (uint32_t c : bestClusters) {
        absSectorChain.push_back(ClusterToAbsSector(c));
    }

    LOG_INFO(L"FatFragmentStitcher: improved " + extension +
             L" chain to score " + std::to_wstring(ScoreChain(bestClusters, fileSize, extLower)));
    return true;
}

bool FatFragmentStitcher::TryImproveExfatNode(SectorReader& reader,
                                               const Partition& partition,
                                               FileNode& node)
{
    if (node.fsType.find(L"exFAT") == std::wstring::npos) return false;
    if (node.isDirectory || node.size == 0 || node.clusters.empty()) return false;

    const uint32_t ss = reader.GetSectorSize() ? reader.GetSectorSize() : 512;
    std::vector<uint8_t> boot(ss);
    if (!reader.ReadSectors(partition.startSector, boot.data(), 1)) return false;
    if (memcmp(boot.data() + 3, "EXFAT   ", 8) != 0) return false;

    EXFAT_BOOT_SECTOR bs{};
    memcpy(&bs, boot.data(), sizeof(bs));

    FatVolumeGeometry geo{};
    geo.bytesPerSector    = 1u << bs.bytesPerSectorShift;
    geo.sectorsPerCluster = 1u << bs.sectorsPerClusterShift;
    geo.dataStartSector   = partition.startSector + bs.clusterHeapOffset;
    geo.totalClusters     = bs.clusterCount;
    geo.fatType           = 32;

    const uint64_t fatStart       = partition.startSector + bs.fatOffset;
    const uint32_t fatSizeSectors = bs.fatLength;
    if (geo.bytesPerSector == 0 || geo.sectorsPerCluster == 0 ||
        fatSizeSectors == 0 || geo.totalClusters == 0)
        return false;

    const uint64_t firstAbs = node.clusters[0];
    if (firstAbs < geo.dataStartSector) return false;
    const uint32_t startCluster = static_cast<uint32_t>(
        (firstAbs - geo.dataStartSector) / geo.sectorsPerCluster + 2);

    FatFragmentStitcher stitcher(reader, geo);
    if (!stitcher.LoadFatTable(fatStart, fatSizeSectors)) return false;

    return stitcher.TryImprove(startCluster, node.size, node.extension, node.clusters);
}

bool FatFragmentStitcher::TryImproveNode(SectorReader& reader,
                                          const Partition& partition,
                                          FileNode& node)
{
    if (node.isDirectory || node.size == 0 || node.clusters.empty()) return false;
    if (node.fsType.find(L"FAT") == std::wstring::npos &&
        node.fsType.find(L"fat") == std::wstring::npos)
        return false;

    const uint32_t ss = reader.GetSectorSize() ? reader.GetSectorSize() : 512;
    std::vector<uint8_t> boot(ss);
    if (!reader.ReadSectors(partition.startSector, boot.data(), 1)) return false;

    FatVolumeGeometry geo{};
    geo.bytesPerSector    = Utils::ReadLE16(boot.data() + 11);
    geo.sectorsPerCluster = boot[13];
    const uint16_t reserved = Utils::ReadLE16(boot.data() + 14);
    const uint8_t  numFats  = boot[16];
    const uint16_t rootEnt  = Utils::ReadLE16(boot.data() + 17);
    const uint16_t fatSz16  = Utils::ReadLE16(boot.data() + 22);
    const uint32_t fatSz32  = Utils::ReadLE32(boot.data() + 36);
    const uint32_t totSec   = Utils::ReadLE16(boot.data() + 19)
                              ? Utils::ReadLE16(boot.data() + 19)
                              : Utils::ReadLE32(boot.data() + 32);

    const uint32_t fatSizeSectors = fatSz16 ? fatSz16 : fatSz32;
    if (geo.bytesPerSector == 0 || geo.sectorsPerCluster == 0 || fatSizeSectors == 0 ||
        totSec == 0)
        return false;

    const uint64_t fatStart = partition.startSector + reserved;
    const uint64_t rootDirSectors =
        (static_cast<uint64_t>(rootEnt) * 32 + geo.bytesPerSector - 1) / geo.bytesPerSector;
    geo.dataStartSector =
        fatStart + static_cast<uint64_t>(numFats) * fatSizeSectors + rootDirSectors;

    const uint64_t dataSectors =
        totSec > (geo.dataStartSector - partition.startSector)
            ? static_cast<uint64_t>(totSec) - (geo.dataStartSector - partition.startSector)
            : 0;
    geo.totalClusters = static_cast<uint32_t>(dataSectors / geo.sectorsPerCluster);

    if (fatSz16 == 0) geo.fatType = 32;
    else if (geo.totalClusters < 4085) geo.fatType = 12;
    else if (geo.totalClusters < 65525) geo.fatType = 16;
    else geo.fatType = 32;

    const uint64_t firstAbs = node.clusters[0];
    if (firstAbs < geo.dataStartSector) return false;
    const uint32_t startCluster = static_cast<uint32_t>(
        (firstAbs - geo.dataStartSector) / geo.sectorsPerCluster + 2);

    FatFragmentStitcher stitcher(reader, geo);
    if (!stitcher.LoadFatTable(fatStart, fatSizeSectors)) return false;

    return stitcher.TryImprove(startCluster, node.size, node.extension, node.clusters);
}
