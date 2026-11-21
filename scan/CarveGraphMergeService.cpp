#include "CarveGraphMergeService.h"

#include "../recover/CarveFragmentBlock.h"
#include "../recover/FormatPayloadScore.h"
#include "../recover/GraphCarveFragmentStitcher.h"
#include "../log/Logger.h"

#include <algorithm>
#include <cwchar>
#include <map>
#include <sstream>
#include <vector>

namespace CarveGraphMergeService {

namespace {

std::wstring LowerExt(std::wstring ext)
{
    for (auto& c : ext) {
        if (c >= L'A' && c <= L'Z') {
            c = static_cast<wchar_t>(c - L'A' + L'a');
        }
    }
    return ext;
}

bool IsRawCarveCandidate(const FileNode& n)
{
    if (n.isDirectory || n.fsType != L"RAW") {
        return false;
    }
    if (n.rawOffset == 0 && n.rawSize == 0) {
        return false;
    }
    return FormatPayloadScoreUtil::SupportsFragmentStitch(LowerExt(n.extension));
}

std::vector<std::vector<size_t>> ClusterByGap(const std::vector<FileNode>& candidates,
                                                const std::vector<size_t>& indices,
                                                uint64_t maxGapBytes,
                                                uint32_t minBlocks)
{
    std::vector<std::vector<size_t>> clusters;
    if (indices.size() < minBlocks) {
        return clusters;
    }

    std::vector<size_t> sorted = indices;
    std::sort(sorted.begin(), sorted.end(), [&](size_t a, size_t b) {
        return candidates[a].rawOffset < candidates[b].rawOffset;
    });

    std::vector<size_t> current;
    uint64_t clusterEnd = 0;

    auto flush = [&]() {
        if (current.size() >= minBlocks) {
            clusters.push_back(current);
        }
        current.clear();
        clusterEnd = 0;
    };

    for (size_t idx : sorted) {
        const auto& n = candidates[idx];
        const uint64_t start = n.rawOffset;
        const uint64_t end = start + n.rawSize;

        if (!current.empty()) {
            if (start < clusterEnd) {
                continue;
            }
            if (start - clusterEnd > maxGapBytes) {
                flush();
            }
        }

        current.push_back(idx);
        clusterEnd = end;
    }
    flush();
    return clusters;
}

std::wstring MakeMergedName(uint64_t firstOffset, const std::wstring& ext)
{
    std::wostringstream oss;
    oss << L"RECOV_GRAPH_" << std::hex << std::uppercase << firstOffset << L'.' << ext;
    return oss.str();
}

} // namespace

uint32_t MergeRawCarveCandidates(SectorReader& reader,
                                 const Partition& partition,
                                 std::vector<FileNode>& candidates,
                                 const FragmentStitchPolicy& stitchPolicy,
                                 const MergeConfig& config)
{
    if (!stitchPolicy.enabled || !stitchPolicy.graphMergeCarvedSpans) {
        return 0;
    }

    std::map<std::wstring, std::vector<size_t>> byExt;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (IsRawCarveCandidate(candidates[i])) {
            byExt[LowerExt(candidates[i].extension)].push_back(i);
        }
    }

    if (byExt.empty()) {
        return 0;
    }

    std::vector<bool> removed(candidates.size(), false);
    std::vector<FileNode> mergedNodes;
    uint32_t mergeCount = 0;

    FragmentStitchPolicy graphPolicy = stitchPolicy;
    graphPolicy.strategy = FragmentStitchStrategy::GraphCarve;

    for (auto& entry : byExt) {
        const std::wstring& ext = entry.first;
        const auto clusters =
            ClusterByGap(candidates, entry.second, config.clusterMaxGapBytes,
                         config.minBlocksPerMerge);

        for (const auto& cluster : clusters) {
            if (cluster.size() < config.minBlocksPerMerge) {
                continue;
            }

            std::vector<CarveFragmentBlock> blocks;
            blocks.reserve(cluster.size());
            for (size_t idx : cluster) {
                const auto& n = candidates[idx];
                blocks.push_back(CarveFragmentBlock{ n.rawOffset, n.rawSize });
            }

            std::vector<CarveFragmentBlock> ordered;
            int stitchScore = -1;
            GraphCarveFragmentStitcher stitcher(reader);
            if (!stitcher.StitchBlocks(ext, blocks, ordered, stitchScore, graphPolicy)) {
                continue;
            }
            if (stitchScore < static_cast<int>(FormatPayloadScore::Weak)) {
                continue;
            }

            FileNode merged;
            merged.rawFragments = std::move(ordered);
            uint64_t total = 0;
            for (const auto& s : merged.rawFragments) {
                total += s.size;
            }
            merged.size = total;
            merged.rawSize = total;
            merged.rawOffset = merged.rawFragments.front().diskOffset;

            const uint64_t firstOff = merged.rawFragments.front().diskOffset;
            merged.name = MakeMergedName(firstOff, ext);
            merged.extension = ext;
            merged.originalPath = L"\\$RAW\\" + merged.name;
            merged.fsType = L"RAW";
            merged.status = FileStatus::Lost;
            if (merged.recoverability == RecoverabilityLevel::Unknown) {
                merged.recoverability = RecoverabilityLevel::Good;
            }

            for (size_t idx : cluster) {
                removed[idx] = true;
            }
            mergedNodes.push_back(std::move(merged));
            ++mergeCount;

            LOG_INFO(L"CarveGraphMergeService: merged " + std::to_wstring(cluster.size()) +
                     L" RAW spans -> " + merged.name);
        }
    }

    if (mergeCount == 0) {
        return 0;
    }

    std::vector<FileNode> compact;
    const size_t removedCount =
        static_cast<size_t>(std::count(removed.begin(), removed.end(), true));
    compact.reserve(candidates.size() - removedCount + mergedNodes.size());
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (!removed[i]) {
            compact.push_back(std::move(candidates[i]));
        }
    }
    for (auto& m : mergedNodes) {
        compact.push_back(std::move(m));
    }
    candidates = std::move(compact);
    return mergeCount;
}

} // namespace CarveGraphMergeService
