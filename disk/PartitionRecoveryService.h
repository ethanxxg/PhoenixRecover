#pragma once

#include "PartitionProbe.h"

#include <atomic>
#include <functional>
#include <vector>

// Low-coupling facade for MBR/GPT partition tables and lost-partition discovery.
// Callers depend on this module — not on PartitionProbe implementation details.
namespace PartitionRecoveryService {

using Candidate = PartitionCandidate;
using Options   = PartitionProbe::Options;
using ProbeMode = PartitionProbe::ProbeMode;

std::vector<Candidate> ProbeVolume(
    SectorReader& reader,
    const Options& opt,
    const std::atomic<bool>* cancelFlag,
    std::function<void(int percent)> onProgress);

std::vector<Candidate> ProbePhysicalDisk(
    int physicalDiskIdx,
    const Options& opt,
    const std::atomic<bool>* cancelFlag,
    std::function<void(int percent)> onProgress);

// MBR + GPT entries only (no signature scan).
std::vector<Candidate> ListTablePartitions(
    SectorReader& reader,
    Options opt,
    const std::atomic<bool>* cancelFlag,
    std::function<void(int percent)> onProgress);

// Signature scan only — recover partitions missing from the partition table.
std::vector<Candidate> RecoverLostPartitions(
    SectorReader& reader,
    Options opt,
    const std::atomic<bool>* cancelFlag,
    std::function<void(int percent)> onProgress);

Partition ToPartition(const Candidate& c, int physicalDiskIdx);

} // namespace PartitionRecoveryService
