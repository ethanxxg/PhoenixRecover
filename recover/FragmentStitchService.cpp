#include "FragmentStitchService.h"

#include "../common/Platform.h"
#include "GraphCarveFragmentStitcher.h"
#if !PHOENIX_LINUX_EXT_FS_ONLY
#include "FatFragmentStitcher.h"
#include "NtfsFragmentStitcher.h"
#include "../fs/fat/FatParser.h"
#endif
#include "../fs/FileSystem.h"

namespace FragmentStitchService {

#if !PHOENIX_LINUX_EXT_FS_ONLY
bool TryImproveFatClusterChain(SectorReader& reader,
                               const FatVolumeGeometry& geo,
                               const std::vector<uint8_t>* fatTableBytes,
                               uint32_t startCluster,
                               uint64_t fileSize,
                               const std::wstring& extension,
                               std::vector<uint64_t>& absSectorChain,
                               const FragmentStitchPolicy& policy)
{
    if (!policy.enabled || fileSize == 0 || absSectorChain.empty()) {
        return false;
    }

    FatFragmentStitcher stitcher(reader, geo, fatTableBytes);
    return stitcher.TryImprove(startCluster, fileSize, extension, absSectorChain);
}
#endif

bool TryGraphStitchCarveBlocks(SectorReader& reader,
                               const Partition& partition,
                               const std::wstring& extension,
                               std::vector<CarveFragmentBlock>& blocks,
                               FileNode& outNode,
                               const FragmentStitchPolicy& policy)
{
    if (!policy.enabled || policy.strategy != FragmentStitchStrategy::GraphCarve ||
        blocks.size() < 2) {
        return false;
    }

    outNode.rawFragments = blocks;
    outNode.extension = extension;
    outNode.fsType = L"RAW";
    GraphCarveFragmentStitcher stitcher(reader);
    if (!stitcher.TryImproveNode(partition, outNode, policy)) {
        return false;
    }
    blocks = outNode.rawFragments;
    return true;
}

bool TryImproveForRecovery(SectorReader& reader,
                           const Partition& partition,
                           FileNode& node,
                           const FragmentStitchPolicy& policy)
{
    if (!policy.enabled || !node.residentData.empty() || node.isDirectory || node.size == 0) {
        return false;
    }

    if (node.rawFragments.size() >= 2 &&
        policy.strategy == FragmentStitchStrategy::GraphCarve) {
        GraphCarveFragmentStitcher stitcher(reader);
        return stitcher.TryImproveNode(partition, node, policy);
    }

    if (policy.strategy != FragmentStitchStrategy::HeuristicGreedy || node.clusters.empty()) {
        return false;
    }

#if !PHOENIX_LINUX_EXT_FS_ONLY
    if (node.fsType.find(L"FAT") != std::wstring::npos) {
        return FatFragmentStitcher::TryImproveNode(reader, partition, node);
    }
    if (node.fsType.find(L"exFAT") != std::wstring::npos) {
        return FatFragmentStitcher::TryImproveExfatNode(reader, partition, node);
    }
    if (node.fsType.find(L"NTFS") != std::wstring::npos) {
        return NtfsFragmentStitcher::TryImproveNode(reader, partition, node);
    }
#endif
    (void)reader;
    (void)partition;
    (void)node;
    return false;
}

void ApplyPolicyToFileSystem(FileSystem& fs, const FragmentStitchPolicy& policy)
{
#if !PHOENIX_LINUX_EXT_FS_ONLY
    if (auto* fat = dynamic_cast<FatParser*>(&fs)) {
        fat->SetFragmentStitchPolicy(policy);
    }
#else
    (void)fs;
    (void)policy;
#endif
}

} // namespace FragmentStitchService
