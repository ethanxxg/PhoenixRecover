#include "VolumeSectorScanner.h"

#include "FileSystemModuleService.h"
#include "../disk/SectorReader.h"
#include "../fs/FileSystem.h"
#include "../log/Logger.h"
#include "../recover/FragmentStitchService.h"

#include <chrono>
#include <memory>

ScanResult VolumeSectorScanner::Run(SectorReader &reader, const Partition &partition)
{
    ScanResult result;
    LOG_INFO(L"VolumeSectorScanner::Run start"
             L" volume=" + partition.driveLetter +
             L" fs=" + partition.fsType +
             L" totalSectors=" + std::to_wstring(partition.totalSectors) +
             L" sectorSize=" + std::to_wstring(reader.GetSectorSize()));
    auto fs = FileSystemModuleService::CreateParserForPartition(reader, partition, result);
    if (!fs) {
        return result;
    }

    if (!fs->Open(reader, partition)) {
        result.errorMsg = L"Failed to open partition with selected file system parser";
        return result;
    }

    FragmentStitchService::ApplyPolicyToFileSystem(*fs, fragmentStitchPolicy_);

    if (onFileFound_) {
        fs->SetFileFoundCallback(onFileFound_);
    }

    uint64_t lastReportedSectors = 0;
    auto lastReportedAt = std::chrono::steady_clock::now();
    const uint64_t sectorReportStep = 1024;
    const auto progressReportInterval = std::chrono::milliseconds(100);
    uint64_t sectorCallbackCount = 0;
    uint64_t progressEmitCount = 0;
    auto lastDiagLogAt = std::chrono::steady_clock::now();
    const uint64_t configuredTotalBytes =
        (partition.sizeBytes > 0)
            ? partition.sizeBytes
            : ((partition.totalSectors > 0)
                   ? (partition.totalSectors * reader.GetSectorSize())
                   : reader.GetTotalBytes());
    LOG_INFO(L"VolumeSectorScanner progress policy"
             L" sectorStep=" + std::to_wstring(sectorReportStep) +
             L" intervalMs=" + std::to_wstring(progressReportInterval.count()));
    const auto reportProgress = [this,
                                 &reader,
                                 &fs,
                                 &lastReportedSectors,
                                 &lastReportedAt,
                                 &progressEmitCount,
                                 configuredTotalBytes,
                                 sectorReportStep,
                                 progressReportInterval](bool force) {
        if (!onProgress_) {
            return;
        }
        const uint64_t currentSectors = reader.GetSectorsRead();
        const auto now = std::chrono::steady_clock::now();
        const bool sectorsAdvancedEnough =
            currentSectors >= lastReportedSectors + sectorReportStep;
        const bool intervalReached =
            (now - lastReportedAt) >= progressReportInterval;
        if (!force && !sectorsAdvancedEnough && !intervalReached) {
            return;
        }

        ScanProgress p;
        p.scannedBytes = currentSectors * reader.GetSectorSize();
        p.totalBytes = configuredTotalBytes;
        p.foundCount = fs ? fs->GetCurrentFileCount() : 0;
        p.percent = (p.totalBytes > 0)
            ? static_cast<int>((std::min)(uint64_t(100), (p.scannedBytes * 100) / p.totalBytes))
            : 0;
        onProgress_(p);
        ++progressEmitCount;
        lastReportedSectors = currentSectors;
        lastReportedAt = now;
    };

    reader.SetSectorReadCallback([&](uint64_t, uint64_t) {
        ++sectorCallbackCount;
        reportProgress(false);
        const auto now = std::chrono::steady_clock::now();
        if ((now - lastDiagLogAt) >= std::chrono::seconds(2)) {
            LOG_INFO(L"VolumeSectorScanner heartbeat"
                     L" sectorCallbacks=" + std::to_wstring(sectorCallbackCount) +
                     L" progressEmits=" + std::to_wstring(progressEmitCount) +
                     L" sectorsRead=" + std::to_wstring(reader.GetSectorsRead()) +
                     L" filesSeen=" + std::to_wstring(fs ? fs->GetCurrentFileCount() : 0));
            lastDiagLogAt = now;
        }
    });

    const auto scanStartedAt = std::chrono::steady_clock::now();
    if (!fs->ScanMetadata(nullptr, cancelFlag_)) {
        result.errorMsg = L"ScanMetadata failed";
        result.cancelled = cancelFlag_ && cancelFlag_->load();
        reader.SetSectorReadCallback(nullptr);
        LOG_WARN(L"VolumeSectorScanner::Run aborted"
                 L" sectorCallbacks=" + std::to_wstring(sectorCallbackCount) +
                 L" progressEmits=" + std::to_wstring(progressEmitCount) +
                 L" elapsedMs=" +
                 std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - scanStartedAt)
                                     .count()));
        return result;
    }

    reader.SetSectorReadCallback(nullptr);

    result.files = fs->GetAllFiles();
    result.scannedBytes = reader.GetSectorsRead() * reader.GetSectorSize();
    result.foundCount = static_cast<uint32_t>(result.files.size());
    result.totalRecords = fs->GetTotalRecordCount();
    result.inUseCount = fs->GetInUseCount();
    result.cancelled = cancelFlag_ && cancelFlag_->load();
    reportProgress(true);
    LOG_INFO(L"VolumeSectorScanner::Run done"
             L" sectorCallbacks=" + std::to_wstring(sectorCallbackCount) +
             L" progressEmits=" + std::to_wstring(progressEmitCount) +
             L" files=" + std::to_wstring(result.files.size()) +
             L" elapsedMs=" +
             std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - scanStartedAt)
                                 .count()));
    return result;
}
