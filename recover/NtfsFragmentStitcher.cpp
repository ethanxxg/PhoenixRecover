#include "NtfsFragmentStitcher.h"

#include "../common/Constants.h"
#include "../common/Utils.h"
#include "../fs/ntfs/NtfsStructs.h"
#include "../log/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>

using namespace Constants;

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

static std::vector<uint64_t> DecodeDataRuns(const uint8_t* p, size_t maxBytes)
{
    std::vector<uint64_t> result;
    if (!p || maxBytes == 0) return result;

    const uint8_t* end = p + maxBytes;
    int64_t currentLcn = 0;

    while (p < end) {
        const uint8_t header = *p++;
        if (header == 0x00) break;

        const int lenFieldSize = header & 0x0F;
        const int offFieldSize = (header >> 4) & 0x0F;
        if (lenFieldSize == 0) break;
        if (p + lenFieldSize + offFieldSize > end) break;

        uint64_t runLen = 0;
        for (int i = 0; i < lenFieldSize; ++i)
            runLen |= static_cast<uint64_t>(p[i]) << (i * 8);
        p += lenFieldSize;

        if (offFieldSize == 0) {
            for (uint64_t k = 0; k < runLen; ++k) result.push_back(0);
            continue;
        }

        const int64_t delta = Utils::ReadSignedLE(p, offFieldSize);
        p += offFieldSize;
        currentLcn += delta;
        if (currentLcn < 0) break;

        for (uint64_t k = 0; k < runLen; ++k)
            result.push_back(static_cast<uint64_t>(currentLcn + static_cast<int64_t>(k)));
    }
    return result;
}

static bool ApplyUsaFixup(uint8_t* recBuf, size_t recSize)
{
    auto* hdr = reinterpret_cast<MFT_RECORD_HEADER*>(recBuf);
    if (hdr->usaOffset == 0 || hdr->usaCount < 2) return true;

    const uint8_t* usaBase = recBuf + hdr->usaOffset;
    const uint16_t usaValue = Utils::ReadLE16(usaBase);
    const uint32_t sectorSize = 512;
    const uint32_t numSectors = static_cast<uint32_t>(recSize) / sectorSize;

    if (static_cast<uint32_t>(hdr->usaCount - 1) != numSectors) return false;

    for (uint32_t i = 0; i < numSectors; ++i) {
        uint8_t* sectorEnd = recBuf + (i + 1) * sectorSize - 2;
        if (Utils::ReadLE16(sectorEnd) != usaValue) return false;
        sectorEnd[0] = usaBase[(i + 1) * 2];
        sectorEnd[1] = usaBase[(i + 1) * 2 + 1];
    }
    return true;
}

static bool ReadMftRecord(SectorReader& reader,
                          uint64_t byteOffset,
                          uint32_t recordSize,
                          std::vector<uint8_t>& recBuf)
{
    recBuf.resize(recordSize);
    if (!reader.ReadBytes(byteOffset, recBuf.data(), recordSize)) return false;
    if (memcmp(recBuf.data(), NTFS_FILE_MAGIC, 4) != 0) return false;
    ApplyUsaFixup(recBuf.data(), recordSize);
    const auto* hdr = reinterpret_cast<const MFT_RECORD_HEADER*>(recBuf.data());
    return hdr->bytesInUse > 0 && hdr->attrOffset >= sizeof(MFT_RECORD_HEADER);
}

static bool ParseMftDataRuns(const std::vector<uint8_t>& recBuf,
                             uint32_t recordSize,
                             std::vector<uint64_t>& outClusters)
{
    const auto* hdr = reinterpret_cast<const MFT_RECORD_HEADER*>(recBuf.data());
    const uint8_t* p   = recBuf.data() + hdr->attrOffset;
    const uint8_t* end = recBuf.data() + recordSize;

    while (p + sizeof(ATTR_HEADER_COMMON) <= end) {
        const auto* ah = reinterpret_cast<const ATTR_HEADER_COMMON*>(p);
        if (ah->type == ATTR_END || ah->length == 0) break;
        if (ah->type == ATTR_DATA && ah->nonResident) {
            const auto* nr = reinterpret_cast<const ATTR_NONRESIDENT*>(p);
            const uint8_t* runs = p + nr->dataRunOffset;
            const size_t maxR = (ah->length > nr->dataRunOffset) ? ah->length - nr->dataRunOffset : 0;
            outClusters = DecodeDataRuns(runs, maxR);
            return !outClusters.empty();
        }
        p += ah->length;
    }
    return false;
}

static bool LoadGeometry(SectorReader& reader,
                         const Partition& partition,
                         NtfsVolumeGeometry& geo,
                         std::vector<uint64_t>& mftClusters)
{
    const uint32_t ss = reader.GetSectorSize() ? reader.GetSectorSize() : 512;
    std::vector<uint8_t> boot(ss);
    if (!reader.ReadSectors(partition.startSector, boot.data(), 1)) return false;
    if (memcmp(boot.data() + 3, "NTFS    ", 8) != 0) return false;

    NTFS_BOOT_SECTOR bpb{};
    memcpy(&bpb, boot.data(), sizeof(bpb));

    geo.bytesPerSector    = bpb.bytesPerSector;
    geo.sectorsPerCluster = bpb.sectorsPerCluster;
    geo.bytesPerCluster   = bpb.bytesPerSector * bpb.sectorsPerCluster;
    geo.partStartBytes    = partition.startSector * ss;

    if (bpb.clustersPerRecord < 0)
        geo.recordSize = 1u << static_cast<uint32_t>(-bpb.clustersPerRecord);
    else
        geo.recordSize = static_cast<uint32_t>(bpb.clustersPerRecord) * geo.bytesPerCluster;

    if (geo.bytesPerCluster == 0 || geo.recordSize == 0) return false;

    geo.mftByteOffset = geo.partStartBytes + bpb.mftStartCluster * geo.bytesPerCluster;

    const uint64_t volBytes = (bpb.totalSectors > 0)
                                  ? bpb.totalSectors * ss
                                  : partition.sizeBytes;
    if (volBytes > geo.partStartBytes) {
        geo.totalClusters = static_cast<uint64_t>((volBytes - geo.partStartBytes) / geo.bytesPerCluster);
    }

    std::vector<uint8_t> recBuf;
    if (!ReadMftRecord(reader, geo.mftByteOffset, geo.recordSize, recBuf)) {
        const uint64_t mirrorOff =
            geo.partStartBytes + bpb.mftMirrorCluster * geo.bytesPerCluster;
        if (!ReadMftRecord(reader, mirrorOff, geo.recordSize, recBuf)) return false;
    }
    return ParseMftDataRuns(recBuf, geo.recordSize, mftClusters);
}

static uint64_t BitmapRecordOffset(const NtfsVolumeGeometry& geo,
                                   const std::vector<uint64_t>& mftClusters)
{
    constexpr uint64_t kBitmapRecord = 6;
    if (!mftClusters.empty() && geo.bytesPerCluster > 0 && geo.recordSize > 0) {
        const uint64_t recordsPerCluster = geo.bytesPerCluster / geo.recordSize;
        if (recordsPerCluster > 0) {
            const uint64_t clusterIndex = kBitmapRecord / recordsPerCluster;
            const uint64_t recordInCluster = kBitmapRecord % recordsPerCluster;
            if (clusterIndex < mftClusters.size()) {
                return geo.partStartBytes + mftClusters[clusterIndex] * geo.bytesPerCluster +
                       recordInCluster * geo.recordSize;
            }
        }
    }
    return geo.mftByteOffset + kBitmapRecord * geo.recordSize;
}

static std::vector<uint64_t> CompactLcns(const std::vector<uint64_t>& chain)
{
    std::vector<uint64_t> out;
    out.reserve(chain.size());
    for (uint64_t lcn : chain) {
        if (lcn != 0) out.push_back(lcn);
    }
    return out;
}

} // namespace

NtfsFragmentStitcher::NtfsFragmentStitcher(SectorReader& reader, const NtfsVolumeGeometry& geo)
    : m_reader(reader)
    , m_geo(geo)
{
}

bool NtfsFragmentStitcher::LoadVolumeBitmap(const std::vector<uint64_t>& mftClusters)
{
    m_bitmap.clear();
    if (m_geo.bytesPerCluster == 0 || m_geo.recordSize == 0) return false;

    std::vector<uint8_t> recBuf;
    const uint64_t bitmapOff = BitmapRecordOffset(m_geo, mftClusters);
    if (!ReadMftRecord(m_reader, bitmapOff, m_geo.recordSize, recBuf)) return false;

    const auto* hdr = reinterpret_cast<const MFT_RECORD_HEADER*>(recBuf.data());
    const uint8_t* p   = recBuf.data() + hdr->attrOffset;
    const uint8_t* end = recBuf.data() + m_geo.recordSize;

    while (p + sizeof(ATTR_HEADER_COMMON) <= end) {
        const auto* ah = reinterpret_cast<const ATTR_HEADER_COMMON*>(p);
        if (ah->type == ATTR_END || ah->length == 0) break;
        if (ah->type == ATTR_DATA && ah->nonResident) {
            const auto* nr = reinterpret_cast<const ATTR_NONRESIDENT*>(p);
            const uint8_t* runs = p + nr->dataRunOffset;
            const size_t maxR = (ah->length > nr->dataRunOffset) ? ah->length - nr->dataRunOffset : 0;
            const auto bitmapClusters = DecodeDataRuns(runs, maxR);
            for (uint64_t lcn : bitmapClusters) {
                if (lcn == 0) continue;
                const uint64_t off = m_geo.partStartBytes + lcn * m_geo.bytesPerCluster;
                const size_t prev = m_bitmap.size();
                m_bitmap.resize(prev + m_geo.bytesPerCluster);
                if (!m_reader.ReadBytes(off, m_bitmap.data() + prev, m_geo.bytesPerCluster)) {
                    m_bitmap.resize(prev);
                }
            }
            return !m_bitmap.empty();
        }
        p += ah->length;
    }
    return false;
}

bool NtfsFragmentStitcher::IsFreeLcn(uint64_t lcn) const
{
    if (lcn == 0 || m_bitmap.empty()) return false;
    const uint64_t byte = lcn / 8;
    const uint8_t  bit  = static_cast<uint8_t>(lcn % 8);
    if (byte >= m_bitmap.size()) return false;
    return (m_bitmap[byte] & (1u << bit)) == 0;
}

bool NtfsFragmentStitcher::ReadChainBytes(const std::vector<uint64_t>& lcns,
                                           uint64_t fileSize,
                                           std::vector<uint8_t>& out) const
{
    if (lcns.empty() || m_geo.bytesPerCluster == 0) return false;

    const uint64_t cap = lcns.size() * m_geo.bytesPerCluster;
    out.resize(static_cast<size_t>(std::min(fileSize, cap)));
    if (out.empty()) return true;

    uint64_t written = 0;
    std::vector<uint8_t> clusterBuf(m_geo.bytesPerCluster);

    for (uint64_t lcn : lcns) {
        if (written >= fileSize) break;
        const uint64_t take = std::min<uint64_t>(m_geo.bytesPerCluster, fileSize - written);
        if (lcn == 0) {
            memset(out.data() + written, 0, static_cast<size_t>(take));
        } else {
            const uint64_t off = m_geo.partStartBytes + lcn * m_geo.bytesPerCluster;
            if (!m_reader.ReadBytes(off, clusterBuf.data(), m_geo.bytesPerCluster)) {
                return false;
            }
            memcpy(out.data() + written, clusterBuf.data(), static_cast<size_t>(take));
        }
        written += take;
    }
    return true;
}

int NtfsFragmentStitcher::ScoreChain(const std::vector<uint64_t>& lcns,
                                     uint64_t fileSize,
                                     const std::wstring& extLower) const
{
    std::vector<uint8_t> buf;
    if (!ReadChainBytes(lcns, fileSize, buf)) return -1;
    return ScoreToInt(FormatPayloadScoreUtil::Score(extLower, buf.data(), buf.size()));
}

bool NtfsFragmentStitcher::SearchAndPickBest(uint64_t startLcn,
                                             uint64_t fileSize,
                                             const std::wstring& extLower,
                                             uint32_t clustersNeeded,
                                             const std::vector<uint64_t>& baseLcns,
                                             int baseScore,
                                             std::vector<uint64_t>& outBest) const
{
    if (m_bitmap.empty() || clustersNeeded <= 1 || startLcn < 2) return false;

    const uint64_t maxLcn = std::min<uint64_t>(
        m_geo.totalClusters > 0 ? m_geo.totalClusters : (m_bitmap.size() * 8),
        m_bitmap.size() * 8);
    const uint32_t remaining = clustersNeeded - 1;

    const uint32_t win = 4096;
    const uint64_t winStart = (startLcn > win) ? (startLcn - win) : 2;
    const uint64_t winEnd = std::min(maxLcn, startLcn + win);

    const auto budgetStart = std::chrono::steady_clock::now();
    constexpr int64_t kBudgetMs = 35;
    constexpr int kMaxTryChains = 1200;
    int tryChains = 0;

    auto isFreeRun = [&](uint64_t runStart, uint32_t len) -> bool {
        if (runStart < 2 || runStart + len > maxLcn) return false;
        for (uint32_t k = 0; k < len; ++k) {
            if (!IsFreeLcn(runStart + k)) return false;
        }
        return true;
    };

    int bestScore = baseScore;
    std::vector<uint64_t> best = baseLcns;
    bool improved = false;

    auto tryChain = [&](const std::vector<uint64_t>& chain) {
        if (chain.size() != clustersNeeded) return;
        if (tryChains++ >= kMaxTryChains) return;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - budgetStart)
                .count() >= kBudgetMs)
            return;
        const int sc = ScoreChain(chain, fileSize, extLower);
        if (sc > bestScore) {
            bestScore = sc;
            best = chain;
            improved = true;
        }
    };

    for (uint64_t runStart = winStart; runStart + remaining <= winEnd; ++runStart) {
        if (runStart == startLcn) continue;
        if (!isFreeRun(runStart, remaining)) continue;
        std::vector<uint64_t> chain;
        chain.reserve(clustersNeeded);
        chain.push_back(startLcn);
        for (uint32_t k = 0; k < remaining; ++k) chain.push_back(runStart + k);
        tryChain(chain);
        if (bestScore >= ScoreToInt(FormatPayloadScore::Complete)) break;
    }

    for (uint32_t a = 1; a < remaining; ++a) {
        const uint64_t run1Start = startLcn + 1;
        const uint32_t run1Len = a;
        const uint32_t run2Len = remaining - a;
        if (!isFreeRun(run1Start, run1Len)) continue;

        for (uint64_t run2Start = winStart; run2Start + run2Len <= winEnd; ++run2Start) {
            if (run2Start == startLcn) continue;
            if (!isFreeRun(run2Start, run2Len)) continue;
            std::vector<uint64_t> chain;
            chain.reserve(clustersNeeded);
            chain.push_back(startLcn);
            for (uint32_t k = 0; k < run1Len; ++k) chain.push_back(run1Start + k);
            for (uint32_t k = 0; k < run2Len; ++k) chain.push_back(run2Start + k);
            tryChain(chain);
            if (bestScore >= ScoreToInt(FormatPayloadScore::Complete)) break;
        }
        if (bestScore >= ScoreToInt(FormatPayloadScore::Complete)) break;
    }

    if (!improved) return false;
    outBest = std::move(best);
    return true;
}

bool NtfsFragmentStitcher::TryImprove(uint64_t startLcn,
                                      uint64_t fileSize,
                                      const std::wstring& extension,
                                      std::vector<uint64_t>& lcnChain)
{
    if (fileSize == 0 || lcnChain.empty() || m_bitmap.empty()) return false;

    std::wstring extLower = LowerExt(extension);
    if (extLower.empty()) {
        std::vector<uint8_t> probe(512);
        const uint64_t first = lcnChain.front();
        if (first != 0) {
            const uint64_t off = m_geo.partStartBytes + first * m_geo.bytesPerCluster;
            if (m_reader.ReadBytes(off, probe.data(), 512)) {
                extLower = FormatPayloadScoreUtil::ProbeExtension(probe.data(), 512);
            }
        }
    }
    if (!FormatPayloadScoreUtil::SupportsFragmentStitch(extLower)) return false;

    const uint32_t clustersNeeded =
        static_cast<uint32_t>((fileSize + m_geo.bytesPerCluster - 1) / m_geo.bytesPerCluster);
    if (clustersNeeded <= 1) return false;

    std::vector<uint64_t> baseLcns = CompactLcns(lcnChain);
    if (baseLcns.empty()) baseLcns.push_back(startLcn);

    while (baseLcns.size() < clustersNeeded) {
        const uint64_t next = baseLcns.back() + 1;
        if (next >= m_geo.totalClusters && m_geo.totalClusters > 0) break;
        baseLcns.push_back(next);
    }
    if (baseLcns.size() > clustersNeeded) {
        baseLcns.resize(clustersNeeded);
    }

    const int baseScore = ScoreChain(baseLcns, fileSize, extLower);
    if (baseScore >= ScoreToInt(FormatPayloadScore::Complete)) {
        const auto compact = CompactLcns(lcnChain);
        if (baseLcns != compact) {
            lcnChain = baseLcns;
            return true;
        }
        return false;
    }

    std::vector<uint64_t> bestLcns;
    if (!SearchAndPickBest(startLcn, fileSize, extLower, clustersNeeded, baseLcns, baseScore,
                           bestLcns)) {
        return false;
    }

    lcnChain = std::move(bestLcns);
    LOG_INFO(L"NtfsFragmentStitcher: improved " + extension + L" to score " +
             std::to_wstring(ScoreChain(lcnChain, fileSize, extLower)));
    return true;
}

bool NtfsFragmentStitcher::TryImproveNode(SectorReader& reader,
                                          const Partition& partition,
                                          FileNode& node)
{
    if (node.isDirectory || node.size == 0 || node.clusters.empty() ||
        !node.residentData.empty() || node.isNtfsCompressed) {
        return false;
    }
    if (node.fsType.find(L"NTFS") == std::wstring::npos) return false;

    std::wstring extLower = LowerExt(node.extension);
    if (!FormatPayloadScoreUtil::SupportsFragmentStitch(extLower)) {
        uint64_t firstLcn = 0;
        for (uint64_t lcn : node.clusters) {
            if (lcn != 0) {
                firstLcn = lcn;
                break;
            }
        }
        if (firstLcn != 0) {
            NtfsVolumeGeometry probeGeo{};
            std::vector<uint64_t> probeMft;
            if (LoadGeometry(reader, partition, probeGeo, probeMft) &&
                probeGeo.bytesPerCluster > 0) {
                std::vector<uint8_t> probe(512);
                const uint64_t off =
                    probeGeo.partStartBytes + firstLcn * probeGeo.bytesPerCluster;
                if (reader.ReadBytes(off, probe.data(), 512)) {
                    extLower = FormatPayloadScoreUtil::ProbeExtension(probe.data(), 512);
                }
            }
        }
    }
    if (!FormatPayloadScoreUtil::SupportsFragmentStitch(extLower)) return false;

    NtfsVolumeGeometry geo{};
    std::vector<uint64_t> mftClusters;
    if (!LoadGeometry(reader, partition, geo, mftClusters)) return false;

    NtfsFragmentStitcher stitcher(reader, geo);
    if (!stitcher.LoadVolumeBitmap(mftClusters)) return false;

    uint64_t startLcn = 0;
    for (uint64_t lcn : node.clusters) {
        if (lcn != 0) {
            startLcn = lcn;
            break;
        }
    }
    if (startLcn == 0) return false;

    return stitcher.TryImprove(startLcn, node.size, extLower, node.clusters);
}
