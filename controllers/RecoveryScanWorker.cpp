#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "RecoveryScanWorker.h"
#include "ScanResultStore.h"

#include "../disk/DiskManager.h"
#include "../disk/Partition.h"
#include "../disk/PhysicalDisk.h"
#include "../disk/SectorReader.h"
#include "../disk/VirtualDiskSectorSource.h"
#include "../disk/PartitionProbe.h"
#include "../log/Logger.h"
#include "../recover/FileNode.h"
#include "../recover/RecoverEngine.h"
#include "../scan/UnifiedVolumeScanner.h"
#include "../recover/FragmentStitchPolicy.h"

#include <QtCore/QSettings>

#include <chrono>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

std::wstring normalizeDriveLetter(const QString &q)
{
    QString s = q.trimmed().toUpper();
    if (s.isEmpty()) {
        return {};
    }
    if (!s.endsWith(QLatin1Char(':'))) {
        s += QLatin1Char(':');
    }
    return std::wstring(reinterpret_cast<const wchar_t *>(s.utf16()),
                        static_cast<size_t>(s.size()));
}

bool findPartitionByLetter(const std::wstring &letter, Partition &out)
{
    std::vector<PhysicalDisk> disks = DiskManager::GetDisks();
    for (const PhysicalDisk &d : disks) {
        for (const Partition &p : d.partitions) {
            if (_wcsicmp(p.driveLetter.c_str(), letter.c_str()) == 0) {
                out = p;
                return true;
            }
        }
    }
    return false;
}

bool fillPartitionFromVolume(const std::wstring &letter, Partition &out, std::wstring &err)
{
    std::wstring root = letter;
    if (!root.empty() && root.back() != L'\\') {
        root += L'\\';
    }

    wchar_t fsName[64]{};
    wchar_t volLabel[256]{};
    if (!GetVolumeInformationW(root.c_str(), volLabel, 256, nullptr, nullptr, nullptr, fsName, 64)) {
        err = L"GetVolumeInformationW failed (err=" + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    out.driveLetter = letter;
    out.label = volLabel;
    out.fsType = fsName;
    out.startSector = 0;
    return true;
}

ScanFileDto toDto(const FileNode &n)
{
    ScanFileDto d;
    d.id = static_cast<quint64>(n.id);
    d.name = QString::fromStdWString(n.name);
    d.fullPath = QString::fromStdWString(n.originalPath);
    d.relativePath = d.fullPath;
    const int colon = d.relativePath.indexOf(QLatin1Char(':'));
    if (colon >= 0) {
        d.relativePath = d.relativePath.mid(colon + 1);
    }
    if (d.relativePath.startsWith(QLatin1Char('\\')) || d.relativePath.startsWith(QLatin1Char('/'))) {
        d.relativePath = d.relativePath.mid(1);
    }
    d.extension = QString::fromStdWString(n.extension);
    d.fsType = QString::fromStdWString(n.fsType);
    d.sizeBytes = static_cast<qint64>(n.size);
    d.parentId = static_cast<quint64>(n.parentId);
    d.isDirectory = n.isDirectory;
    d.isDeleted = (n.status != FileStatus::Normal);
    d.modifyTimeFt = n.modifyTime;
    return d;
}

bool wantsPreviewThumb(const std::wstring &ext)
{
    static const wchar_t *const kImg[] = {
        L"jpg",  L"jpeg", L"jfif", L"jpe",  L"png",   L"gif",  L"bmp",  L"dib",  L"webp", L"ico",
        L"tif",  L"tiff", L"heic", L"heif", L"avif",  L"jp2",  L"j2c",  L"j2k",  L"ppm",  L"pgm",
        L"pbm",  L"pam",  L"xpm",  L"tga",  L"cr2",   L"nef",  L"arw",  L"dng",  L"orf",  L"rw2",
        L"pef",  L"srw",  L"raf",  L"3fr",  L"mrw",   L"raw",  L"psd",  L"exr",  L"hdr",  L"svg"};
    static const wchar_t *const kVid[] = {L"mp4",  L"m4v",  L"mov",  L"avi",  L"mkv",
                                          L"wmv",  L"webm", L"flv",  L"mpg",  L"mpeg",
                                          L"ts",   L"m2ts", L"m2t",  L"3gp",  L"3g2"};
    for (const wchar_t *pe : kImg) {
        if (_wcsicmp(ext.c_str(), pe) == 0) {
            return true;
        }
    }
    for (const wchar_t *pe : kVid) {
        if (_wcsicmp(ext.c_str(), pe) == 0) {
            return true;
        }
    }
    return false;
}

bool isPreviewVideoExtension(const std::wstring &ext)
{
    static const wchar_t *const kVid[] = {L"mp4",  L"m4v",  L"mov",  L"avi",  L"mkv",
                                          L"wmv",  L"webm", L"flv",  L"mpg",  L"mpeg",
                                          L"ts",   L"m2ts", L"m2t",  L"3gp",  L"3g2"};
    for (const wchar_t *pe : kVid) {
        if (_wcsicmp(ext.c_str(), pe) == 0) {
            return true;
        }
    }
    return false;
}

bool isPreviewImageExtension(const std::wstring &ext)
{
    static const wchar_t *const kImg[] = {
        L"jpg",  L"jpeg", L"jfif", L"jpe",  L"png",   L"gif",  L"bmp",  L"dib",  L"webp", L"ico",
        L"tif",  L"tiff", L"heic", L"heif", L"avif",  L"jp2",  L"j2c",  L"j2k",  L"ppm",  L"pgm",
        L"pbm",  L"pam",  L"xpm",  L"tga",  L"cr2",   L"nef",  L"arw",  L"dng",  L"orf",  L"rw2",
        L"pef",  L"srw",  L"raf",  L"3fr",  L"mrw",   L"raw",  L"psd",  L"exr",  L"hdr",  L"svg"};
    for (const wchar_t *pe : kImg) {
        if (_wcsicmp(ext.c_str(), pe) == 0) {
            return true;
        }
    }
    return false;
}

void tryAttachPreviewPlan(SectorReader &reader, const Partition &part,
                          const FileNode &n, ScanResultStore *store, ScanFileDto &d)
{
    d.previewKind = ScanPreviewKind::None;
    d.previewVolumeOffset = 0;
    d.previewReadLength = 0;
    if (!store || n.isDirectory || !wantsPreviewThumb(n.extension)) {
        return;
    }
    if (n.size == 0 && n.rawSize == 0 && n.residentData.empty() && n.clusters.empty()) {
        return;
    }

    uint32_t kMaxPreview = 256u * 1024u;
    const bool isVid = isPreviewVideoExtension(n.extension);
    const bool isImg = isPreviewImageExtension(n.extension);
    if (isVid) {
        kMaxPreview = 64u * 1024u * 1024u;
    } else if (isImg) {
        kMaxPreview = 16u * 1024u * 1024u;
    }

    if (!n.residentData.empty()) {
        const size_t cap = std::min(n.residentData.size(), static_cast<size_t>(kMaxPreview));
        store->setPreviewResidentBlob(
            d.id,
            QByteArray(reinterpret_cast<const char *>(n.residentData.data()),
                       static_cast<int>(cap)));
        d.previewKind = ScanPreviewKind::ResidentBlob;
        return;
    }

    uint64_t sliceOff = 0;
    uint32_t sliceLen = 0;
    const bool sliceOk =
        RecoverEngine::TryPreviewSingleReadSlice(reader, part, n, kMaxPreview, sliceOff, sliceLen)
        && sliceLen > 0;
    // 视频容器解码需要足够前缀；首段连续区常只有 1 簇(4KB) 时 VolumeSlice 必失败，应走 FullNode。
    constexpr uint32_t kMinVideoSlice = 2u * 1024u * 1024u;
    // 图片 256KB 头常不够完整 JPEG/PNG；首簇过小时同样走 FullNode 按序读前缀。
    constexpr uint32_t kMinImageSlice = 512u * 1024u;
    const uint64_t logicalSize = n.size > 0 ? n.size : n.rawSize;
    const bool videoSliceEnough =
        !isVid
        || (sliceLen >= kMinVideoSlice)
        || (logicalSize > 0 && logicalSize < kMinVideoSlice
            && static_cast<uint64_t>(sliceLen) >= logicalSize);
    const bool imageSliceEnough =
        !isImg
        || (sliceLen >= kMinImageSlice)
        || (logicalSize > 0 && logicalSize < kMinImageSlice
            && static_cast<uint64_t>(sliceLen) >= logicalSize);
    if (sliceOk && videoSliceEnough && imageSliceEnough) {
        d.previewKind = ScanPreviewKind::VolumeSlice;
        d.previewVolumeOffset = sliceOff;
        d.previewReadLength = sliceLen;
        return;
    }

    if (n.clusters.empty() && n.rawSize == 0) {
        d.previewKind = ScanPreviewKind::None;
        return;
    }

    store->setPreviewFileNodeSnapshot(n.id, n);
    d.previewKind = ScanPreviewKind::FullNode;
}

} // namespace

RecoveryScanWorker::RecoveryScanWorker(ScanResultStore *resultStore, QObject *parent)
    : QObject(parent)
    , resultStore_(resultStore)
{
}

void RecoveryScanWorker::runImageScan(QString imagePath)
{
    pendingImagePath_ = imagePath;
    runVolumeScan(QString());
}

void RecoveryScanWorker::runPartitionScan(int physicalDiskIdx,
                                          quint64 startSector,
                                          quint64 totalSectors,
                                          QString fsType)
{
    pendingPhysicalDiskIdx_ = physicalDiskIdx;
    pendingPartitionStartSector_ = startSector;
    pendingPartitionTotalSectors_ = totalSectors;
    pendingPartitionFsType_ = std::move(fsType);
    runVolumeScan(QString());
}

void RecoveryScanWorker::runVolumeScan(QString driveLetter)
{
    cancelFlag_.store(false);
    if (resultStore_) {
        resultStore_->clear();
    }

    const QString imagePath = pendingImagePath_;
    pendingImagePath_.clear();
    const bool imageScan = !imagePath.isEmpty();
    const bool partitionScan = !imageScan && pendingPhysicalDiskIdx_ >= 0;
    const int physicalDiskIdx = pendingPhysicalDiskIdx_;
    const uint64_t partitionStartSector = pendingPartitionStartSector_;
    const uint64_t partitionTotalSectors = pendingPartitionTotalSectors_;
    const QString partitionFsType = pendingPartitionFsType_;
    pendingPhysicalDiskIdx_ = -1;
    pendingPartitionStartSector_ = 0;
    pendingPartitionTotalSectors_ = 0;
    pendingPartitionFsType_.clear();
    const std::wstring letter = imageScan
        ? std::wstring()
        : normalizeDriveLetter(driveLetter);
    if (!imageScan && !partitionScan && letter.empty()) {
        emit finished(false, QStringLiteral("无效的盘符。"));
        return;
    }

    // A1-2: container formats (VHD/VHDX/VMDK/QCOW2/VDI) go through the
    // read-only virtual sector source. Plain .img/.dd/.raw keep the original
    // OpenImage() path so their exact byte length is preserved.
    const std::wstring imageWide = imageScan ? imagePath.toStdWString() : std::wstring();
    const bool useVirtual = imageScan && phoenix::IsVirtualDiskImagePath(imageWide);
    std::string virtualDetail;

    SectorReader reader;
    const bool readerOpened = useVirtual
        ? reader.OpenVirtualImage(imageWide, &virtualDetail)
        : imageScan
              ? reader.OpenImage(imageWide)
              : partitionScan
                    ? reader.Open(physicalDiskIdx)
                    : reader.OpenVolume(letter);
    if (!readerOpened) {
        if (useVirtual) {
            LOG_WARN(L"RecoveryScanWorker::OpenVirtualImage failed image=" +
                     imageWide + L" reason=" +
                     std::wstring(virtualDetail.begin(), virtualDetail.end()));
            emit finished(
                false,
                QStringLiteral("无法打开虚拟磁盘镜像 %1：%2\n\n该文件可能是不支持的容器子型（如差分盘、压缩 grain、加密卷）。"
                               "程序不会把它当作裸盘扫描，以免产生误报。")
                    .arg(imagePath, QString::fromStdString(virtualDetail)));
            return;
        }
        const DWORD e = reader.GetLastWinError();
        emit finished(
            false,
            QStringLiteral("无法打开卷 %1（错误 %2）。请以管理员身份运行，并确认盘符正确。")
                .arg(driveLetter)
                .arg(static_cast<qulonglong>(e)));
        return;
    }
    if (useVirtual) {
        const auto& info = reader.GetVirtualDiskInfo();
        const std::string fmtName = phoenix::VirtualDiskFormatName(info.format);
        LOG_INFO(L"RecoveryScanWorker::OpenVirtualImage ok image=" + imageWide +
                 L" format=" + std::wstring(fmtName.begin(), fmtName.end()) +
                 L" sectors=" + std::to_wstring(reader.GetTotalSectors()) +
                 L" detail=" + std::wstring(virtualDetail.begin(), virtualDetail.end()));
    }
    reader.SetCancelFlag(&cancelFlag_);

    Partition part;
    if (imageScan) {
        PartitionProbe::Options probeOptions;
        probeOptions.mode = PartitionProbe::ProbeMode::Full;
        const auto candidates = PartitionProbe::ProbeVolume(
            reader, probeOptions, &cancelFlag_, nullptr);
        if (!candidates.empty()) {
            part = PartitionProbe::ToPartition(candidates.front(), -1);
        } else {
            part.startSector = 0;
            part.totalSectors = reader.GetTotalSectors();
            part.sizeBytes = reader.GetTotalBytes();
            part.fsType = L"RAW";
        }
        LOG_INFO(L"RecoveryScanWorker::runImageScan start image=" +
                 imagePath.toStdWString() +
                 L" startSector=" + std::to_wstring(part.startSector));
    } else if (partitionScan) {
        part.physicalDiskIdx = physicalDiskIdx;
        part.startSector = partitionStartSector;
        part.totalSectors = partitionTotalSectors;
        part.fsType = partitionFsType.toStdWString();
        part.driveLetter.clear();
        LOG_INFO(L"RecoveryScanWorker::runPartitionScan start physicalDisk=" +
                 std::to_wstring(physicalDiskIdx) +
                 L" startSector=" + std::to_wstring(part.startSector) +
                 L" totalSectors=" + std::to_wstring(part.totalSectors));
    } else if (!findPartitionByLetter(letter, part)) {
        std::wstring err;
        if (!fillPartitionFromVolume(letter, part, err)) {
            emit finished(false, QString::fromStdWString(err));
            return;
        }
    }

    if (!imageScan && !partitionScan)
        part.startSector = 0;
    if (part.totalSectors == 0) {
        part.totalSectors = partitionScan && reader.GetTotalSectors() > part.startSector
                                ? reader.GetTotalSectors() - part.startSector
                                : reader.GetTotalSectors();
    }
    if (part.sizeBytes == 0) {
        part.sizeBytes = (part.totalSectors > 0)
                             ? (part.totalSectors * reader.GetSectorSize())
                             : reader.GetTotalBytes();
    }
    LOG_INFO(L"RecoveryScanWorker::runVolumeScan start"
             L" drive=" + letter +
             L" totalSectors=" + std::to_wstring(part.totalSectors) +
             L" sectorSize=" + std::to_wstring(reader.GetSectorSize()) +
             L" totalBytes=" + std::to_wstring(part.sizeBytes));

    if (resultStore_) {
        resultStore_->setScanVolumePartition(part);
    }

    std::unordered_set<uint64_t> emitted;
    QVector<ScanFileDto> pendingDtos;
    pendingDtos.reserve(32);
    const int maxBatchSize = 32;
    const auto fileFlushInterval = std::chrono::milliseconds(25);
    const auto indexEmitInterval = std::chrono::milliseconds(300);
    const auto progressEmitInterval = std::chrono::seconds(1);
    auto lastFileFlushAt = std::chrono::steady_clock::now();
    auto lastIndexEmitAt = std::chrono::steady_clock::now();
    auto lastProgressEmitAt = std::chrono::steady_clock::now();
    int lastProgressPercent = -1;
    quint32 lastProgressFound = 0;
    quint64 lastProgressScanned = 0;
    quint64 lastProgressTotal = 0;
    std::atomic<uint64_t> fileCallbackCount(0);
    std::atomic<uint64_t> fileBatchEmitCount(0);
    std::atomic<uint64_t> progressCallbackCount(0);
    std::atomic<uint64_t> progressEmitCount(0);
    std::atomic<uint64_t> pendingDtoCount(0);
    std::atomic<uint64_t> lastActivityTick(0);
    std::atomic<bool> monitorRunning(true);
    const auto workerStartedAt = std::chrono::steady_clock::now();
    lastActivityTick.store(GetTickCount64());
    auto lastFileCallbackLogAt = std::chrono::steady_clock::now();

    std::thread monitor([&]() {
        uint64_t lastLoggedFileCallbacks = 0;
        uint64_t lastLoggedProgressCallbacks = 0;
        uint64_t lastLoggedFileBatches = 0;
        uint64_t lastLoggedProgressEmits = 0;
        while (monitorRunning.load()) {
            Sleep(2000);
            if (!monitorRunning.load()) {
                break;
            }
            const uint64_t nowTick = GetTickCount64();
            const uint64_t idleMs = nowTick - lastActivityTick.load();
            const uint64_t currentFileCallbacks = fileCallbackCount.load();
            const uint64_t currentProgressCallbacks = progressCallbackCount.load();
            const uint64_t currentFileBatches = fileBatchEmitCount.load();
            const uint64_t currentProgressEmits = progressEmitCount.load();
            LOG_INFO(L"Scan monitor"
                     L" idleMs=" + std::to_wstring(idleMs) +
                     L" fileCallbacks=" + std::to_wstring(currentFileCallbacks) +
                     L" fileBatchEmits=" + std::to_wstring(currentFileBatches) +
                     L" progressCallbacks=" + std::to_wstring(currentProgressCallbacks) +
                     L" progressEmits=" + std::to_wstring(currentProgressEmits) +
                     L" pendingDtos=" + std::to_wstring(pendingDtoCount.load()));

            if (idleMs >= 3000 &&
                currentFileCallbacks == lastLoggedFileCallbacks &&
                currentProgressCallbacks == lastLoggedProgressCallbacks &&
                currentFileBatches == lastLoggedFileBatches &&
                currentProgressEmits == lastLoggedProgressEmits) {
                LOG_WARN(L"Scan monitor: no observable callback activity for "
                         + std::to_wstring(idleMs)
                         + L" ms, suspect parser internal blocking");
            }

            lastLoggedFileCallbacks = currentFileCallbacks;
            lastLoggedProgressCallbacks = currentProgressCallbacks;
            lastLoggedFileBatches = currentFileBatches;
            lastLoggedProgressEmits = currentProgressEmits;
        }
    });

    const auto flushPendingDtos = [&]() {
        if (pendingDtos.isEmpty()) {
            return;
        }
        bool changed = false;
        if (resultStore_) {
            for (int i = 0; i < pendingDtos.size(); ++i) {
                changed = resultStore_->upsert(pendingDtos.at(i)) || changed;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (changed && (now - lastIndexEmitAt) >= indexEmitInterval) {
            fileBatchEmitCount.fetch_add(1);
            lastActivityTick.store(GetTickCount64());
            emit indexUpdated(resultStore_ ? resultStore_->revision() : 0);
            lastIndexEmitAt = now;
        }
        pendingDtos.clear();
        pendingDtoCount.store(0);
        lastFileFlushAt = now;
    };

    const auto emitProgressIfNeeded = [&](const ScanProgress &p, bool force) {
        const auto now = std::chrono::steady_clock::now();
        const bool intervalReached = (now - lastProgressEmitAt) >= progressEmitInterval;
        const bool percentChanged = (p.percent != lastProgressPercent);
        const bool foundChanged = (p.foundCount != lastProgressFound);
        if (!force && !intervalReached && !percentChanged && !foundChanged) {
            return;
        }

        progressEmitCount.fetch_add(1);
        lastActivityTick.store(GetTickCount64());
        const quint64 elapsedMs = static_cast<quint64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now - workerStartedAt).count());
        const QString phaseText = p.stageLabel.empty()
                                      ? QStringLiteral("扫描进行中")
                                      : QString::fromStdWString(p.stageLabel);
        emit progress(p.percent, p.foundCount, p.scannedBytes, p.totalBytes, elapsedMs, phaseText);
        lastProgressPercent = p.percent;
        lastProgressFound = p.foundCount;
        lastProgressScanned = p.scannedBytes;
        lastProgressTotal = p.totalBytes;
        lastProgressEmitAt = now;
    };

    UnifiedVolumeScanner scanner;
    scanner.SetCancelFlag(&cancelFlag_);

    UnifiedScanConfig scanConfig;
    {
        QSettings prefs;
        scanConfig.fragmentStitch.enabled =
            prefs.value(QStringLiteral("recover/formatAwareFragments"), true).toBool();
        // A1-10: opt-in parallel RAW carving. Default 1 keeps the original
        // serial scan; 0 = auto (cores), N > 1 = explicit. Clamped in
        // CarveModuleService::Run(). No UI control is added for this setting.
        const int requestedThreads =
            prefs.value(QStringLiteral("recover/rawScanThreads"), 1).toInt();
        scanConfig.rawScanThreads = requestedThreads >= 0
            ? static_cast<size_t>(requestedThreads) : 1;
    }
    scanner.SetConfig(scanConfig);

    scanner.SetFileFoundCallback([&](const FileNode &n) {
        if (!emitted.insert(n.id).second) {
            return;
        }
        const uint64_t currentFileCallbacks = fileCallbackCount.fetch_add(1) + 1;
        lastActivityTick.store(GetTickCount64());
        ScanFileDto dto = toDto(n);
        if (resultStore_) {
            resultStore_->setRecoveredFileNode(dto.id, n);
        }
        tryAttachPreviewPlan(reader, part, n, resultStore_, dto);
        pendingDtos.push_back(std::move(dto));
        pendingDtoCount.store(static_cast<uint64_t>(pendingDtos.size()));
        const auto now = std::chrono::steady_clock::now();
        if ((now - lastFileCallbackLogAt) >= std::chrono::seconds(2)) {
            LOG_INFO(L"RecoveryScanWorker file callback"
                     L" total=" + std::to_wstring(currentFileCallbacks) +
                     L" pendingDtos=" + std::to_wstring(pendingDtoCount.load()) +
                     L" latestId=" + std::to_wstring(static_cast<uint64_t>(n.id)) +
                     L" isDir=" + std::to_wstring(n.isDirectory ? 1 : 0));
            lastFileCallbackLogAt = now;
        }
        if (pendingDtos.size() >= maxBatchSize || (now - lastFileFlushAt) >= fileFlushInterval) {
            flushPendingDtos();
        }
    });
    scanner.SetProgressCallback([&](const ScanProgress &p) {
        progressCallbackCount.fetch_add(1);
        lastActivityTick.store(GetTickCount64());
        emitProgressIfNeeded(p, false);
    });
    const ScanResult result = scanner.Run(reader, part);
    const auto scanCompletedAt = std::chrono::steady_clock::now();

    ScanProgress finalProgress;
    finalProgress.percent = 100;
    finalProgress.foundCount = static_cast<quint32>(result.files.size());
    finalProgress.scannedBytes = result.scannedBytes;
    finalProgress.totalBytes = part.sizeBytes;
    finalProgress.stageLabel = L"扫描完成";
    emitProgressIfNeeded(finalProgress, true);

    if (result.cancelled) {
        emit finished(false, QStringLiteral("扫描已取消。"));
    } else if (!result.errorMsg.empty()) {
        emit finished(false, QString::fromStdWString(result.errorMsg));
    } else {
        emit finished(true,
                      QStringLiteral("扫描完成，共解析到 %1 个文件系统节点。")
                          .arg(static_cast<qulonglong>(result.files.size())));
    }

    flushPendingDtos();
    if (resultStore_) {
        emit indexUpdated(resultStore_->revision());
    }
    monitorRunning.store(false);
    if (monitor.joinable()) {
        monitor.join();
    }
    LOG_INFO(L"RecoveryScanWorker::runVolumeScan summary"
             L" fileCallbacks=" + std::to_wstring(fileCallbackCount.load()) +
             L" fileBatchEmits=" + std::to_wstring(fileBatchEmitCount.load()) +
             L" progressCallbacks=" + std::to_wstring(progressCallbackCount.load()) +
             L" progressEmits=" + std::to_wstring(progressEmitCount.load()) +
             L" scanDoneToSummaryMs=" +
             std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - scanCompletedAt)
                                 .count()) +
             L" elapsedMs=" +
             std::to_wstring(std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - workerStartedAt)
                                 .count()));
}

void RecoveryScanWorker::requestStopScan()
{
    cancelFlag_.store(true);
}
