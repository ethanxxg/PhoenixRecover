#include "PartitionRecoveryService.h"

namespace PartitionRecoveryService {

std::vector<Candidate> ProbeVolume(
    SectorReader& reader,
    const Options& opt,
    const std::atomic<bool>* cancelFlag,
    std::function<void(int percent)> onProgress)
{
    return PartitionProbe::ProbeVolume(reader, opt, cancelFlag, std::move(onProgress));
}

std::vector<Candidate> ProbePhysicalDisk(
    int physicalDiskIdx,
    const Options& opt,
    const std::atomic<bool>* cancelFlag,
    std::function<void(int percent)> onProgress)
{
    return PartitionProbe::ProbePhysicalDisk(
        physicalDiskIdx, opt, cancelFlag, std::move(onProgress));
}

std::vector<Candidate> ListTablePartitions(
    SectorReader& reader,
    Options opt,
    const std::atomic<bool>* cancelFlag,
    std::function<void(int percent)> onProgress)
{
    opt.mode = ProbeMode::TableOnly;
    return PartitionProbe::ProbeVolume(reader, opt, cancelFlag, std::move(onProgress));
}

std::vector<Candidate> RecoverLostPartitions(
    SectorReader& reader,
    Options opt,
    const std::atomic<bool>* cancelFlag,
    std::function<void(int percent)> onProgress)
{
    opt.mode = ProbeMode::LostOnly;
    return PartitionProbe::ProbeVolume(reader, opt, cancelFlag, std::move(onProgress));
}

Partition ToPartition(const Candidate& c, int physicalDiskIdx)
{
    return PartitionProbe::ToPartition(c, physicalDiskIdx);
}

} // namespace PartitionRecoveryService
