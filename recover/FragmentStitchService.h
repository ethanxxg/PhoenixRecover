#pragma once

#include "../common/Platform.h"
#include "FragmentStitchPolicy.h"
#include "CarveFragmentBlock.h"
#include "FileNode.h"
#if !PHOENIX_LINUX_EXT_FS_ONLY
#include "FatFragmentStitcher.h"
#endif
#include "../disk/Partition.h"
#include "../disk/SectorReader.h"

#include <cstdint>
#include <vector>

class FileSystem;

// Single integration point for fragment reassembly. Callers (FAT parser,
// RecoverEngine, tests) depend on this module — not on FatFragmentStitcher directly.
namespace FragmentStitchService {

#if !PHOENIX_LINUX_EXT_FS_ONLY
bool TryImproveFatClusterChain(SectorReader& reader,
                               const FatVolumeGeometry& geo,
                               const std::vector<uint8_t>* fatTableBytes,
                               uint32_t startCluster,
                               uint64_t fileSize,
                               const std::wstring& extension,
                               std::vector<uint64_t>& absSectorChain,
                               const FragmentStitchPolicy& policy);
#endif

bool TryImproveForRecovery(SectorReader& reader,
                           const Partition& partition,
                           FileNode& node,
                           const FragmentStitchPolicy& policy);

// GraphCarve: stitch multiple disk spans into ordered node.rawFragments.
bool TryGraphStitchCarveBlocks(SectorReader& reader,
                               const Partition& partition,
                               const std::wstring& extension,
                               std::vector<CarveFragmentBlock>& blocks,
                               FileNode& outNode,
                               const FragmentStitchPolicy& policy);

void ApplyPolicyToFileSystem(FileSystem& fs, const FragmentStitchPolicy& policy);

} // namespace FragmentStitchService
