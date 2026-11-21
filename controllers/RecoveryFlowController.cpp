#include "RecoveryFlowController.h"
#include "RecoveryScanController.h"
#include "ScanResultStore.h"
#include "../pages/RecoveryLocationPage.h"
#include "../pages/RecoveryScanPage.h"
#include "../pages/SDCardConnectionPage.h"
#include "../pages/MoreRecoveryToolsPage.h"
#include "../pages/FileRepairPage.h"
#include "../scan/ScanSerializer.h"
#include "../disk/DiskManager.h"
#include "../disk/Imager.h"
#include "../disk/SmartInfo.h"
#include "../disk/PartitionRecoveryService.h"
#include "../log/Logger.h"

#include <QtCore/QMetaObject>
#include <QtCore/QPointer>
#include <QtCore/QRunnable>
#include <QtCore/QThreadPool>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QDateTime>
#include <QtCore/QFileInfo>
#include <QtCore/QSettings>
#include <QtCore/QStandardPaths>
#include <QtCore/QStringList>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QInputDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QWidget>

#include <algorithm>
#include <functional>

namespace {

bool testModeEnabled()
{
    return qEnvironmentVariableIsSet("PHOENIX_TEST_MODE") ||
           !qgetenv("PHOENIX_MOCK_SCAN_COUNT").isEmpty();
}

class SmartProbeRunnable final : public QRunnable
{
public:
    explicit SmartProbeRunnable(std::function<void()> task)
        : task_(std::move(task))
    {
        setAutoDelete(true);
    }

    void run() override
    {
        if (task_) {
            task_();
        }
    }

private:
    std::function<void()> task_;
};

}

RecoveryFlowController::RecoveryFlowController(QWidget *uiParent, QObject *parent)
    : QObject(parent)
    , uiParent_(uiParent)
{
}

RecoveryFlowController::~RecoveryFlowController()
{
    if (imagingCancel_)
        imagingCancel_->store(true);
    if (imagingThread_.joinable())
        imagingThread_.join();
    if (partitionCancel_)
        partitionCancel_->store(true);
    if (partitionThread_.joinable())
        partitionThread_.join();
}

void RecoveryFlowController::init(QStackedWidget *outerStack,
                                  QStackedWidget *wizardPages,
                                  QWidget *wizardShell,
                                  RecoveryScanController *scanCtrl,
                                  RecoveryScanPage *scanPage,
                                  RecoveryLocationPage *locationPage,
                                  SDCardConnectionPage *sdPage,
                                  MoreRecoveryToolsPage *toolsPage,
                                  FileRepairPage *repairPage)
{
    outerStack_ = outerStack;
    wizardPages_ = wizardPages;
    wizardShell_ = wizardShell;
    scanCtrl_ = scanCtrl;
    scanPage_ = scanPage;
    locationPage_ = locationPage;
    sdPage_ = sdPage;
    toolsPage_ = toolsPage;
    repairPage_ = repairPage;

    // LocationPage → FlowController
    connect(locationPage_, &RecoveryLocationPage::requestStartScan,
            this, &RecoveryFlowController::onStartScanRequested);

    // SDCardConnectionPage → FlowController
    connect(sdPage_, &SDCardConnectionPage::requestSdCardRecovery,
            this, &RecoveryFlowController::onSdCardRecoveryRequested);

    // MoreRecoveryToolsPage → FlowController
    connect(toolsPage_, &MoreRecoveryToolsPage::requestToolRecovery,
            this, &RecoveryFlowController::onToolRecoveryRequested);
    connect(toolsPage_, &MoreRecoveryToolsPage::requestPartitionScan,
            this, &RecoveryFlowController::onPartitionScanRequested);
    connect(toolsPage_, &MoreRecoveryToolsPage::requestSessionLoad,
            this, &RecoveryFlowController::onSessionLoadRequested);
    connect(toolsPage_, &MoreRecoveryToolsPage::requestSessionDelete,
            this, &RecoveryFlowController::onSessionDeleteRequested);
    connect(toolsPage_, &MoreRecoveryToolsPage::requestSessionImport,
            this, &RecoveryFlowController::onSessionImportRequested);
    connect(toolsPage_, &MoreRecoveryToolsPage::requestSessionExport,
            this, &RecoveryFlowController::onSessionExportRequested);

    // ScanPage → FlowController
    connect(scanPage_, &RecoveryScanPage::requestBack,
            this, &RecoveryFlowController::onScanPageBack);

    if (repairPage_) {
        connect(repairPage_, &FileRepairPage::requestBack,
                this, &RecoveryFlowController::onRepairPageBack);
    }

    if (scanPage_) {
        connect(scanPage_, &RecoveryScanPage::requestRepairFiles,
                this, &RecoveryFlowController::onRequestRepairFiles);
    }

    // ScanPage → ScanController (stop scan directly)
    connect(scanPage_, &RecoveryScanPage::requestStopScan,
            scanCtrl_, &RecoveryScanController::stopScan);

    // ScanController → ScanPage (progress/index/finish display)
    connect(scanCtrl_, &RecoveryScanController::scanIndexUpdated,
            scanPage_, &RecoveryScanPage::onScanIndexUpdated);
    connect(scanCtrl_, &RecoveryScanController::scanProgress,
            scanPage_, &RecoveryScanPage::setScanProgress);
    connect(scanCtrl_, &RecoveryScanController::scanFinished,
            scanPage_, &RecoveryScanPage::setScanFinished);

    // ScanController → FlowController (state transition on finish)
    connect(scanCtrl_, &RecoveryScanController::scanFinished,
            this, &RecoveryFlowController::onScanFinished);

    LOG_INFO(L"RecoveryFlowController::init completed – all signal connections established");
}

FlowState RecoveryFlowController::currentState() const
{
    return state_;
}

// ── Navigation methods ──────────────────────────────────────────────────────

void RecoveryFlowController::navigateToLocation()
{
    setState(FlowState::LocationSelect);
    navigateToPage(FlowState::LocationSelect);
}

void RecoveryFlowController::navigateToSDCard()
{
    setState(FlowState::SDCard);
    navigateToPage(FlowState::SDCard);
}

void RecoveryFlowController::navigateToMoreTools()
{
    setState(FlowState::MoreTools);
    navigateToPage(FlowState::MoreTools);
}

void RecoveryFlowController::startScan(const QString &locationOption, const QString &driveLetter)
{
    LOG_INFO(L"RecoveryFlowController::startScan"
             L" location=" + locationOption.toStdWString() +
             L" disk=" + driveLetter.toStdWString());

    // 仅在真实本地磁盘扫描前执行一次风险提示；镜像扫描本身已经是隔离副本，
    // 模拟扫描也不应触发物理设备查询。提示只改变流程建议，不会绕过用户选择。
    QSettings prefs;
    const bool suggestImage = prefs.value(QStringLiteral("recovery/imageFirst"), true).toBool();
    const bool mockScan = !qgetenv("PHOENIX_MOCK_SCAN_COUNT").isEmpty();
    if (suggestImage && !mockScan && locationOption == QStringLiteral("本地磁盘") &&
        !driveLetter.isEmpty()) {
        const std::vector<SmartInfo> smartInfos = SmartReader::ReadAllDisks();
        bool risky = false;
        QString riskDetail;
        for (const SmartInfo &info : smartInfos) {
            if (info.health == DiskHealthStatus::Warning ||
                info.health == DiskHealthStatus::Critical ||
                info.health == DiskHealthStatus::Failing ||
                info.pendingSectors > 0 || info.uncorrectableSectors > 0 ||
                info.reallocatedSectors > 0) {
                risky = true;
                riskDetail = QStringLiteral("检测到 SMART 状态异常或介质错误计数，建议先制作只读镜像。\n\n")
                                 + QString::fromStdWString(SmartReader::HealthStatusText(info.health));
                break;
            }
        }
        if (risky) {
            const auto reply = QMessageBox::question(
                uiParent_, QStringLiteral("建议先制作磁盘镜像"),
                riskDetail + QStringLiteral("\n\n是否现在打开磁盘镜像流程？"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (reply == QMessageBox::Yes) {
                startImaging();
                return;
            }
        }
    }

    sessionDirty_ = false;
    setState(FlowState::Scanning);
    navigateToPage(FlowState::Scanning);

    const bool testMode = qEnvironmentVariableIsSet("PHOENIX_TEST_MODE") ||
                          !qgetenv("PHOENIX_MOCK_SCAN_COUNT").isEmpty();
    if (testMode) {
        // 测试模式只展示模拟扫描结果，绝不把本地盘、镜像或物理分区
        // 交给真实扫描 worker。这样 UI 验证不会触碰用户设备。
        if (!locationOption.isEmpty() && !driveLetter.isEmpty()) {
            scanPage_->beginScanSession(locationOption, driveLetter);
        } else {
            scanPage_->setNonDiskPlaceholder(QStringLiteral("测试数据"));
        }
        return;
    }

    if (locationOption == QStringLiteral("本地磁盘") && !driveLetter.isEmpty()) {
        scanPage_->beginScanSession(locationOption, driveLetter);
        scanCtrl_->startVolumeScan(driveLetter);
    } else if (locationOption == QStringLiteral("磁盘镜像") &&
               !driveLetter.isEmpty() && QFileInfo::exists(driveLetter)) {
        scanPage_->beginScanSession(locationOption, driveLetter);
        scanCtrl_->startImageScan(driveLetter);
    } else {
        scanPage_->setNonDiskPlaceholder(locationOption);
    }
}

void RecoveryFlowController::showResults()
{
    setState(FlowState::Results);
    navigateToPage(FlowState::Results);
}

void RecoveryFlowController::startRecovery()
{
    setState(FlowState::RecoveryTarget);
}

void RecoveryFlowController::showComplete()
{
    setState(FlowState::RecoveryComplete);
}

void RecoveryFlowController::navigateBack()
{
    switch (state_) {
    case FlowState::Scanning: {
        const auto reply = QMessageBox::question(
            uiParent_,
            QStringLiteral("确认放弃扫描"),
            QStringLiteral("扫描正在进行，确认放弃当前扫描结果？"),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            return;
        }
        if (scanCtrl_) {
            scanCtrl_->stopScan();
        }
        setState(prevState_);
        navigateToPage(prevState_);
        break;
    }
    case FlowState::Results:
        setState(FlowState::LocationSelect);
        navigateToPage(FlowState::LocationSelect);
        break;
    case FlowState::RecoveryTarget:
        setState(FlowState::Results);
        navigateToPage(FlowState::Results);
        break;
    case FlowState::RecoveryComplete:
        setState(FlowState::Results);
        navigateToPage(FlowState::Results);
        break;
    default:
        setState(FlowState::Idle);
        navigateToPage(FlowState::LocationSelect);
        break;
    }
}

// ── Slots ────────────────────────────────────────────────────────────────────

void RecoveryFlowController::onStartScanRequested(const QString &location, const QString &disk)
{
    LOG_INFO(L"RecoveryFlowController::onStartScanRequested"
             L" location=" + location.toStdWString() +
             L" disk=" + disk.toStdWString());
    startScan(location, disk);
}

void RecoveryFlowController::onSdCardRecoveryRequested()
{
    LOG_INFO(L"RecoveryFlowController::onSdCardRecoveryRequested");
    QMessageBox::information(uiParent_,
                             QStringLiteral("SD 卡恢复"),
                             QStringLiteral("正在检测可移动存储设备，请确认读卡器已正确连接。\n"
                                            "检测到设备后将自动开始扫描。"));
}

void RecoveryFlowController::startImaging()
{
    if (testModeEnabled()) {
        QMessageBox::information(uiParent_, QStringLiteral("测试模式"),
                                 QStringLiteral("测试模式不会读取物理磁盘或制作真实镜像。"));
        return;
    }
    if (imagingThread_.joinable()) {
        QMessageBox::information(uiParent_, QStringLiteral("磁盘镜像"),
                                 QStringLiteral("已有镜像任务正在运行。"));
        return;
    }

    const std::vector<PhysicalDisk> disks = DiskManager::GetDisks();
    if (disks.empty()) {
        QMessageBox::warning(
            uiParent_, QStringLiteral("磁盘镜像"),
            QStringLiteral("未检测到可访问的物理磁盘。请以管理员权限运行程序。"));
        return;
    }

    QStringList diskItems;
    for (const PhysicalDisk& disk : disks) {
        QString item = QStringLiteral("PhysicalDrive%1").arg(disk.index);
        if (!disk.model.empty())
            item += QStringLiteral(" - ") + QString::fromStdWString(disk.model);
        if (disk.sizeBytes > 0)
            item += QStringLiteral(" (%1 GB)")
                .arg(QString::number(static_cast<double>(disk.sizeBytes) /
                                     (1024.0 * 1024.0 * 1024.0), 'f', 1));
        diskItems << item;
    }

    bool selected = false;
    const QString selectedItem = QInputDialog::getItem(
        uiParent_, QStringLiteral("选择源磁盘"),
        QStringLiteral("要读取并制作镜像的物理磁盘："),
        diskItems, 0, false, &selected);
    if (!selected)
        return;

    const int diskRow = diskItems.indexOf(selectedItem);
    if (diskRow < 0 || diskRow >= static_cast<int>(disks.size()))
        return;
    const int diskIndex = disks[static_cast<size_t>(diskRow)].index;

    const QString outputPath = QFileDialog::getSaveFileName(
        uiParent_, QStringLiteral("选择镜像输出文件"), QString(),
        QStringLiteral("Raw 镜像 (*.img *.dd);;所有文件 (*)"));
    if (outputPath.isEmpty())
        return;

    const ImageOutputSafety outputSafety =
        Imager::CheckOutputPath(diskIndex, outputPath.toStdWString());
    if (outputSafety == ImageOutputSafety::SamePhysicalDisk) {
        QMessageBox::critical(
            uiParent_, QStringLiteral("禁止写回源磁盘"),
            QStringLiteral("输出路径位于源物理磁盘上。\n"
                           "请改用另一块物理磁盘或网络存储，否则可能覆盖待恢复数据。"));
        return;
    }

    const auto confirm = QMessageBox::warning(
        uiParent_, QStringLiteral("确认创建镜像"),
        QStringLiteral("镜像必须保存到另一块物理磁盘，不能保存到源磁盘。\n\n"
                       "源磁盘：PhysicalDrive%1\n输出：%2\n\n"
                       "镜像过程可能持续较长时间，是否继续？")
            .arg(diskIndex).arg(outputPath),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (confirm != QMessageBox::Yes)
        return;

    setState(FlowState::Imaging);
    lastImagePath_ = outputPath;
    imagingCancel_ = std::make_shared<std::atomic<bool>>(false);
    imagingProgress_ = new QProgressDialog(
        QStringLiteral("正在准备磁盘镜像…"), QStringLiteral("取消"),
        0, 100, uiParent_);
    imagingProgress_->setWindowTitle(QStringLiteral("磁盘镜像"));
    imagingProgress_->setWindowModality(Qt::WindowModal);
    imagingProgress_->setAutoClose(false);
    imagingProgress_->setAutoReset(false);
    imagingProgress_->setMinimumDuration(0);

    connect(imagingProgress_, &QProgressDialog::canceled, this, [this]() {
        if (imagingCancel_)
            imagingCancel_->store(true);
        if (imagingProgress_)
            imagingProgress_->setLabelText(QStringLiteral("正在停止镜像，请等待当前读取完成…"));
    });
    imagingProgress_->show();

    const auto cancel = imagingCancel_;
    QPointer<RecoveryFlowController> self(this);
    imagingThread_ = std::thread(
        [self, cancel, diskIndex, outputPath]() {
            const ImageResult result = Imager::CreatePhysicalDiskImage(
                diskIndex, outputPath.toStdWString(), 4u * 1024u * 1024u,
                cancel.get(),
                [self](const ImageProgress& progress) {
                    if (!self)
                        return;
                    QMetaObject::invokeMethod(
                        self.data(), [self, progress]() {
                            if (!self || !self->imagingProgress_)
                                return;
                            int percent = 0;
                            if (progress.totalBytes > 0) {
                                const uint64_t bounded =
                                    (std::min)(progress.doneBytes,
                                               progress.totalBytes);
                                percent = static_cast<int>(
                                    bounded * 100 / progress.totalBytes);
                            }
                            self->imagingProgress_->setValue(percent);
                            self->imagingProgress_->setLabelText(
                                QStringLiteral("已读取 %1 / %2，坏区域 %3")
                                    .arg(QString::number(
                                        static_cast<double>(progress.doneBytes) /
                                        (1024.0 * 1024.0 * 1024.0), 'f', 2))
                                    .arg(QString::number(
                                        static_cast<double>(progress.totalBytes) /
                                        (1024.0 * 1024.0 * 1024.0), 'f', 2))
                                    .arg(QString::number(
                                        static_cast<double>(progress.badBytes) /
                                        (1024.0 * 1024.0), 'f', 1)) +
                                QStringLiteral(" MB"));
                        }, Qt::QueuedConnection);
                });

            if (!self)
                return;
            QMetaObject::invokeMethod(
                self.data(), [self, result]() {
                    if (self)
                        self->finishImaging(result);
                }, Qt::QueuedConnection);
        });
}

void RecoveryFlowController::finishImaging(const ImageResult& result)
{
    if (imagingThread_.joinable())
        imagingThread_.join();
    imagingCancel_.reset();

    if (imagingProgress_) {
        imagingProgress_->close();
        imagingProgress_->deleteLater();
        imagingProgress_ = nullptr;
    }

    setState(FlowState::MoreTools);
    const QString message = QString::fromStdWString(result.message);
    const QString hash = QString::fromStdWString(result.sha256);
    if (result.ok && result.badBytes == 0) {
        QMessageBox::information(uiParent_, QStringLiteral("磁盘镜像"),
                                 QStringLiteral("镜像创建成功。\n写入：%1 GB\nSHA-256：%2\n%3")
                                     .arg(QString::number(
                                         static_cast<double>(result.writtenBytes) /
                                         (1024.0 * 1024.0 * 1024.0), 'f', 2))
                                     .arg(hash.isEmpty() ? QStringLiteral("不可用") : hash)
                                     .arg(message));
    } else if (result.ok) {
        QMessageBox::warning(uiParent_, QStringLiteral("磁盘镜像"),
                             QStringLiteral("镜像已完成，但包含 %1 MB 读失败区域。\nSHA-256：%2\n%3")
                                 .arg(QString::number(
                                     static_cast<double>(result.badBytes) /
                                     (1024.0 * 1024.0), 'f', 1))
                                 .arg(hash.isEmpty() ? QStringLiteral("不可用") : hash)
                                 .arg(message));
    } else {
        QMessageBox::warning(uiParent_, QStringLiteral("磁盘镜像"),
                             message.isEmpty()
                                 ? QStringLiteral("镜像未完成。")
                                 : message);
    }

    if (result.ok && !lastImagePath_.isEmpty() && scanPage_ && scanCtrl_) {
        const QString imagePath = lastImagePath_;
        lastImagePath_.clear();
        QMessageBox::information(
            uiParent_, QStringLiteral("开始扫描镜像"),
            QStringLiteral("镜像已校验完成，现在将从镜像启动恢复扫描。"));
        setState(FlowState::Scanning);
        navigateToPage(FlowState::Scanning);
        scanPage_->beginScanSession(QStringLiteral("磁盘镜像"), imagePath);
        scanCtrl_->startImageScan(imagePath);
    }
}

void RecoveryFlowController::startPartitionProbe()
{
    if (testModeEnabled()) {
        partitionDiskIndex_ = 0;
        PartitionCandidate ntfs;
        ntfs.startSector = 2048;
        ntfs.totalSectors = 131072;
        ntfs.sizeBytes = ntfs.totalSectors * 512;
        ntfs.fsType = L"NTFS";
        ntfs.source = L"TEST_DATA";
        ntfs.confidence = 98;

        PartitionCandidate fat;
        fat.startSector = 264192;
        fat.totalSectors = 65536;
        fat.sizeBytes = fat.totalSectors * 512;
        fat.fsType = L"FAT32";
        fat.source = L"TEST_DATA";
        fat.confidence = 91;

        finishPartitionProbe({ntfs, fat});
        return;
    }
    if (partitionThread_.joinable()) {
        QMessageBox::information(uiParent_, QStringLiteral("分区探测"),
                                 QStringLiteral("已有分区探测任务正在运行。"));
        return;
    }

    const std::vector<PhysicalDisk> disks = DiskManager::GetDisks();
    if (disks.empty()) {
        QMessageBox::warning(uiParent_, QStringLiteral("分区探测"),
                             QStringLiteral("未检测到可访问的物理磁盘，请确认设备已连接。"));
        return;
    }

    QStringList diskItems;
    for (const PhysicalDisk& disk : disks) {
        QString item = QStringLiteral("PhysicalDrive%1").arg(disk.index);
        if (!disk.model.empty()) {
            item += QStringLiteral(" - ") + QString::fromStdWString(disk.model);
        }
        if (disk.sizeBytes > 0) {
            item += QStringLiteral(" (%1 GB)")
                        .arg(QString::number(static_cast<double>(disk.sizeBytes) /
                                             (1024.0 * 1024.0 * 1024.0),
                                             'f', 1));
        }
        diskItems << item;
    }

    bool selected = false;
    const QString selectedItem = QInputDialog::getItem(
        uiParent_, QStringLiteral("选择探测设备"),
        QStringLiteral("需要探测分区结构的物理磁盘："),
        diskItems, 0, false, &selected);
    if (!selected) {
        return;
    }

    const int diskRow = diskItems.indexOf(selectedItem);
    if (diskRow < 0 || diskRow >= static_cast<int>(disks.size())) {
        return;
    }
    const int diskIndex = disks[static_cast<size_t>(diskRow)].index;
    partitionDiskIndex_ = diskIndex;

    partitionCancel_ = std::make_shared<std::atomic<bool>>(false);
    partitionProgress_ = new QProgressDialog(
        QStringLiteral("正在探测分区结构…"), QStringLiteral("取消"), 0, 100, uiParent_);
    partitionProgress_->setWindowTitle(QStringLiteral("分区探测"));
    partitionProgress_->setWindowModality(Qt::WindowModal);
    partitionProgress_->setAutoClose(false);
    partitionProgress_->setAutoReset(false);
    partitionProgress_->setMinimumDuration(0);
    connect(partitionProgress_, &QProgressDialog::canceled, this, [this]() {
        if (partitionCancel_) {
            partitionCancel_->store(true);
        }
        if (partitionProgress_) {
            partitionProgress_->setLabelText(QStringLiteral("正在停止探测，请等待当前读取完成…"));
        }
    });
    partitionProgress_->show();

    const auto cancel = partitionCancel_;
    QPointer<RecoveryFlowController> self(this);
    partitionThread_ = std::thread([self, cancel, diskIndex]() {
        PartitionRecoveryService::Options options;
        options.mode = PartitionRecoveryService::ProbeMode::Full;
        const std::vector<PartitionCandidate> candidates =
            PartitionRecoveryService::ProbePhysicalDisk(
                diskIndex, options, cancel.get(), [self](int percent) {
                    if (!self) {
                        return;
                    }
                    QMetaObject::invokeMethod(
                        self.data(), [self, percent]() {
                            if (self && self->partitionProgress_) {
                                self->partitionProgress_->setValue(
                                    (std::max)(0, (std::min)(percent, 100)));
                            }
                        }, Qt::QueuedConnection);
                });

        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(
            self.data(), [self, candidates]() {
                if (self) {
                    self->finishPartitionProbe(candidates);
                }
            }, Qt::QueuedConnection);
    });
}

void RecoveryFlowController::finishPartitionProbe(
    const std::vector<PartitionCandidate>& candidates)
{
    if (partitionThread_.joinable()) {
        partitionThread_.join();
    }
    partitionCancel_.reset();

    if (partitionProgress_) {
        partitionProgress_->close();
        partitionProgress_->deleteLater();
        partitionProgress_ = nullptr;
    }

    QString summary;
    if (candidates.empty()) {
        summary = QStringLiteral(
            "没有发现可验证的分区候选。\n\n"
            "可能原因：\n"
            "• 分区表和文件系统签名均已损坏\n"
            "• 设备读取权限不足\n"
            "• 当前设备需要先制作镜像再探测\n\n"
            "本次探测不会写回分区表。\n");
    } else {
        summary = QStringLiteral("发现 %1 个分区候选：\n\n").arg(candidates.size());
        for (size_t i = 0; i < candidates.size(); ++i) {
            const PartitionCandidate& candidate = candidates[i];
            summary += QStringLiteral("[%1] %2 · %3 · 起始 LBA %4 · 大小 %5 GB · 置信度 %6% · 来源 %7\n")
                           .arg(static_cast<int>(i + 1))
                           .arg(QString::fromStdWString(candidate.fsType))
                           .arg(QString::fromStdWString(candidate.source))
                           .arg(QString::number(candidate.startSector))
                           .arg(QString::number(static_cast<double>(candidate.sizeBytes) /
                                                (1024.0 * 1024.0 * 1024.0),
                                                'f', 2))
                           .arg(candidate.confidence)
                           .arg(QString::fromStdWString(candidate.source));
        }
        summary += QStringLiteral("\n请在确认候选分区后，再进入只读文件扫描。\n");
    }

    setState(FlowState::MoreTools);
    navigateToPage(FlowState::MoreTools);
    if (toolsPage_) {
        if (candidates.empty()) {
            toolsPage_->setPartitionResults(summary);
        } else {
            toolsPage_->setPartitionCandidates(partitionDiskIndex_, candidates, summary);
        }
    }
    partitionDiskIndex_ = -1;
    if (candidates.empty()) {
        QMessageBox::warning(uiParent_, QStringLiteral("分区探测"), summary);
    } else {
        QMessageBox::information(uiParent_, QStringLiteral("分区探测"),
                                 QStringLiteral("已完成探测，发现 %1 个候选分区。")
                                     .arg(candidates.size()));
    }
}

void RecoveryFlowController::onToolRecoveryRequested(const QString &toolName)
{
    LOG_INFO(L"RecoveryFlowController::onToolRecoveryRequested"
             L" tool=" + toolName.toStdWString());

    if (toolName.contains(QStringLiteral("镜像"))) {
        startImaging();
        return;
    }

    // SMART is a short, read-only query and can be shown directly without
    // starting a long-running worker. Disk imaging remains an asynchronous
    // workflow until its cancel/progress page is wired in.
    if (toolName.contains(QStringLiteral("SMART"), Qt::CaseInsensitive)) {
        setState(FlowState::SmartHealth);
        if (testModeEnabled()) {
            const QString summary = QStringLiteral(
                "测试模式已跳过 SMART 物理设备查询。\n\n"
                "当前验证只使用模拟扫描数据和 testdata 镜像，不会访问用户电脑中的真实设备。\n");
            setState(FlowState::MoreTools);
            navigateToPage(FlowState::MoreTools);
            if (toolsPage_) {
                toolsPage_->setSmartSummary(summary);
            }
            return;
        }
        if (toolsPage_) {
            setState(FlowState::MoreTools);
            navigateToPage(FlowState::MoreTools);
            toolsPage_->setSmartSummary(QStringLiteral("正在后台读取 SMART 信息，请稍候…"));
        }

        QPointer<RecoveryFlowController> guard(this);
        auto *probe = new SmartProbeRunnable([guard]() {
            const std::vector<SmartInfo> infos = SmartReader::ReadAllDisks();
            QStringList lines;
            for (const SmartInfo &info : infos) {
                const QString model = info.model.empty()
                    ? QStringLiteral("PhysicalDrive%1").arg(info.diskIndex)
                    : QString::fromStdWString(info.model);
                const QString health = QString::fromStdWString(
                    SmartReader::HealthStatusText(info.health));
                QString line = QStringLiteral("磁盘 %1：%2，状态：%3")
                    .arg(info.diskIndex).arg(model).arg(health);
                if (info.reallocatedSectors || info.pendingSectors ||
                    info.uncorrectableSectors) {
                    line += QStringLiteral("\n  重映射 %1，待定 %2，不可校正 %3")
                        .arg(info.reallocatedSectors)
                        .arg(info.pendingSectors)
                        .arg(info.uncorrectableSectors);
                }
                lines << line;
            }
            if (lines.isEmpty()) {
                lines << QStringLiteral(
                    "未能读取任何磁盘的 SMART 信息。\n"
                    "请以管理员权限运行，或确认设备/控制器允许 SMART 查询。");
            }
            const QString summary = lines.join(QStringLiteral("\n\n"));
            QMetaObject::invokeMethod(guard.data(), [guard, summary]() {
                if (!guard) {
                    return;
                }
                guard->setState(FlowState::MoreTools);
                guard->navigateToPage(FlowState::MoreTools);
                if (guard->toolsPage_) {
                    guard->toolsPage_->setSmartSummary(summary);
                }
            }, Qt::QueuedConnection);
        });
        QThreadPool::globalInstance()->start(probe);
        return;

        const std::vector<SmartInfo> infos = SmartReader::ReadAllDisks();
        QStringList lines;
        for (const SmartInfo& info : infos) {
            const QString model = info.model.empty()
                ? QStringLiteral("PhysicalDrive%1").arg(info.diskIndex)
                : QString::fromStdWString(info.model);
            const QString health = QString::fromStdWString(
                SmartReader::HealthStatusText(info.health));
            QString line = QStringLiteral("磁盘 %1：%2，状态：%3")
                .arg(info.diskIndex).arg(model).arg(health);
            if (info.reallocatedSectors || info.pendingSectors ||
                info.uncorrectableSectors) {
                line += QStringLiteral("\n  重映射=%1，待定=%2，不可校正=%3")
                    .arg(info.reallocatedSectors)
                    .arg(info.pendingSectors)
                    .arg(info.uncorrectableSectors);
            }
            lines << line;
        }

        if (lines.isEmpty()) {
            lines << QStringLiteral(
                "未能读取任何磁盘的 SMART 信息。\n"
                "请以管理员权限运行，或确认设备/控制器允许 SMART 查询。");
        }

        const QString summary = lines.join(QStringLiteral("\n\n"));
        setState(FlowState::MoreTools);
        navigateToPage(FlowState::MoreTools);
        if (toolsPage_) {
            toolsPage_->setSmartSummary(summary);
        }
        return;
    }

    if (toolName.contains(QStringLiteral("分区探测"))) {
        startPartitionProbe();
        return;
    }

    if (toolName.contains(QStringLiteral("扫描会话"))) {
        const QDir dir(sessionDir());
        const QFileInfoList entries = dir.entryInfoList(
            QStringList() << QStringLiteral("*.prs"),
            QDir::Files | QDir::Readable,
            QDir::Time);
        QStringList labels;
        QStringList paths;
        QString summary;
        if (entries.isEmpty()) {
            summary = QStringLiteral(
                "当前没有找到已保存的扫描会话。\n\n"
                "扫描完成后，程序会自动保存索引到应用数据目录；\n"
                "下次可以从这里继续筛选和恢复。\n");
        } else {
            summary = QStringLiteral("找到 %1 个扫描会话：\n\n").arg(entries.size());
            for (const QFileInfo &entry : entries) {
                const QString label = QStringLiteral("%1 · %2 · %3 KB")
                                           .arg(entry.completeBaseName())
                                           .arg(entry.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")))
                                           .arg(QString::number(entry.size() / 1024));
                labels << label;
                paths << entry.absoluteFilePath();
                summary += label + QLatin1Char('\n');
            }
            summary += QStringLiteral("\n选择会话后会执行完整格式校验，再加载到结果页。\n");
        }
        setState(FlowState::MoreTools);
        navigateToPage(FlowState::MoreTools);
        if (toolsPage_) {
            toolsPage_->setSessionEntries(labels, paths, summary);
        }
        return;
    }

    if (toolName.contains(QStringLiteral("恢复指南"))) {
        setState(FlowState::MoreTools);
        navigateToPage(FlowState::MoreTools);
        if (toolsPage_) {
            toolsPage_->showToolPage(QStringLiteral("恢复指南"));
        }
        return;
    }

    if (toolName == QStringLiteral("文件修复")) {
        setState(FlowState::Repair);
        navigateToPage(FlowState::Repair);
    } else {
        QMessageBox::information(uiParent_,
                                 QStringLiteral("更多恢复工具"),
                                 QStringLiteral("「%1」功能即将上线，敬请期待。").arg(toolName));
    }
}

void RecoveryFlowController::onPartitionScanRequested(int physicalDiskIdx,
                                                       quint64 startSector,
                                                       quint64 totalSectors,
                                                       const QString &fsType)
{
    if (!scanPage_ || !scanCtrl_ || physicalDiskIdx < 0 || totalSectors == 0) {
        QMessageBox::warning(uiParent_, QStringLiteral("分区扫描"),
                             QStringLiteral("当前候选分区缺少有效的物理位置或大小信息。"));
        return;
    }

    if (testModeEnabled()) {
        startScan(QStringLiteral("模拟分区扫描 · %1")
                      .arg(fsType.isEmpty() ? QStringLiteral("未知文件系统") : fsType),
                  QStringLiteral("TEST_DATA"));
        return;
    }

    const QString sourceId = QStringLiteral("PhysicalDrive%1").arg(physicalDiskIdx);
    setState(FlowState::Scanning);
    navigateToPage(FlowState::Scanning);
    scanPage_->beginScanSession(QStringLiteral("物理磁盘分区 · %1 · %2")
                                    .arg(sourceId)
                                    .arg(fsType.isEmpty() ? QStringLiteral("未知文件系统") : fsType),
                                sourceId);
    scanCtrl_->startPartitionScan(physicalDiskIdx, startSector, totalSectors, fsType);
}

void RecoveryFlowController::onSessionLoadRequested(const QString &path)
{
    if (testModeEnabled()) {
        QMessageBox::information(uiParent_, QStringLiteral("测试模式"),
                                 QStringLiteral("测试模式不会加载用户历史会话，只使用当前测试数据。"));
        return;
    }
    const QString normalized = QDir::cleanPath(path);
    if (normalized.isEmpty() || !QFile::exists(normalized)) {
        QMessageBox::warning(uiParent_, QStringLiteral("扫描会话"),
                             QStringLiteral("所选扫描会话不存在或已被移动。"));
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("session/lastPath"), normalized);
    tryLoadLastSession();
}

void RecoveryFlowController::onSessionDeleteRequested(const QString &path)
{
    const QString normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString root = QDir::cleanPath(QFileInfo(sessionDir()).absoluteFilePath());
    const bool isSessionFile = normalized.endsWith(QStringLiteral(".prs"), Qt::CaseInsensitive);
    const bool insideSessionDir = normalized.startsWith(root + QDir::separator()) &&
                                  QFileInfo(normalized).dir().absolutePath().compare(root, Qt::CaseInsensitive) == 0;
    if (!isSessionFile || !insideSessionDir || !QFileInfo::exists(normalized)) {
        QMessageBox::warning(uiParent_, QStringLiteral("删除扫描会话"),
                             QStringLiteral("只能删除应用数据目录中的有效 .prs 会话文件。"));
        return;
    }

    const auto reply = QMessageBox::question(
        uiParent_, QStringLiteral("删除扫描会话"),
        QStringLiteral("确定删除以下扫描会话吗？\n\n%1\n\n只会删除会话索引，不会删除源盘数据。")
            .arg(QFileInfo(normalized).fileName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return;
    }
    if (!QFile::remove(normalized)) {
        QMessageBox::warning(uiParent_, QStringLiteral("删除扫描会话"),
                             QStringLiteral("删除失败：%1").arg(normalized));
        return;
    }

    QSettings settings;
    if (QDir::cleanPath(settings.value(QStringLiteral("session/lastPath")).toString())
            .compare(normalized, Qt::CaseInsensitive) == 0) {
        settings.remove(QStringLiteral("session/lastPath"));
    }
    if (toolsPage_) {
        onToolRecoveryRequested(QStringLiteral("扫描会话"));
    }
}

void RecoveryFlowController::onSessionImportRequested()
{
    const QString sourcePath = QFileDialog::getOpenFileName(
        uiParent_, QStringLiteral("导入扫描会话"), QString(),
        QStringLiteral("PhoenixRecover 会话 (*.prs);;所有文件 (*)"));
    if (sourcePath.isEmpty()) {
        return;
    }

    ScanResult result;
    Partition partition;
    if (!ScanSerializer::Load(sourcePath.toStdWString(), result, partition)) {
        QMessageBox::warning(uiParent_, QStringLiteral("导入扫描会话"),
                             QStringLiteral("所选文件不是有效的 PhoenixRecover 扫描会话，或文件已经损坏。"));
        return;
    }

    QDir destinationDir(sessionDir());
    if (!destinationDir.exists() && !destinationDir.mkpath(QStringLiteral("."))) {
        QMessageBox::warning(uiParent_, QStringLiteral("导入扫描会话"),
                             QStringLiteral("无法创建应用会话目录。"));
        return;
    }
    QString baseName = QFileInfo(sourcePath).completeBaseName();
    if (baseName.isEmpty()) {
        baseName = QStringLiteral("imported_session");
    }
    QString destinationPath = destinationDir.filePath(baseName + QStringLiteral(".prs"));
    if (QFileInfo::exists(destinationPath)) {
        destinationPath = destinationDir.filePath(
            baseName + QStringLiteral("_%1.prs")
                .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    }
    if (!QFile::copy(sourcePath, destinationPath)) {
        QMessageBox::warning(uiParent_, QStringLiteral("导入扫描会话"),
                             QStringLiteral("复制会话文件失败，请检查磁盘空间和文件权限。"));
        return;
    }
    QMessageBox::information(uiParent_, QStringLiteral("导入扫描会话"),
                             QStringLiteral("会话已导入，可以在列表中选择并加载。"));
    onToolRecoveryRequested(QStringLiteral("扫描会话"));
}

void RecoveryFlowController::onSessionExportRequested(const QString &path)
{
    const QString normalized = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString root = QDir::cleanPath(QFileInfo(sessionDir()).absoluteFilePath());
    const bool isSessionFile = normalized.endsWith(QStringLiteral(".prs"), Qt::CaseInsensitive);
    const bool insideSessionDir = normalized.startsWith(root + QDir::separator()) &&
                                  QFileInfo(normalized).dir().absolutePath().compare(root, Qt::CaseInsensitive) == 0;
    if (!isSessionFile || !insideSessionDir || !QFileInfo::exists(normalized)) {
        QMessageBox::warning(uiParent_, QStringLiteral("导出扫描会话"),
                             QStringLiteral("只能导出应用数据目录中的有效 .prs 会话文件。"));
        return;
    }

    const QString defaultName = QFileInfo(normalized).completeBaseName() + QStringLiteral(".prs");
    QString destination = QFileDialog::getSaveFileName(
        uiParent_, QStringLiteral("导出扫描会话"), defaultName,
        QStringLiteral("PhoenixRecover 会话 (*.prs);;所有文件 (*)"));
    if (destination.isEmpty()) {
        return;
    }
    if (!destination.endsWith(QStringLiteral(".prs"), Qt::CaseInsensitive)) {
        destination += QStringLiteral(".prs");
    }
    if (QDir::cleanPath(destination).compare(normalized, Qt::CaseInsensitive) == 0) {
        QMessageBox::information(uiParent_, QStringLiteral("导出扫描会话"),
                                 QStringLiteral("导出位置与原会话相同，无需重复复制。"));
        return;
    }
    if (QFileInfo::exists(destination) && !QFile::remove(destination)) {
        QMessageBox::warning(uiParent_, QStringLiteral("导出扫描会话"),
                             QStringLiteral("无法覆盖目标文件。"));
        return;
    }
    if (!QFile::copy(normalized, destination)) {
        QMessageBox::warning(uiParent_, QStringLiteral("导出扫描会话"),
                             QStringLiteral("导出失败，请检查目标位置和磁盘空间。"));
        return;
    }
    QMessageBox::information(uiParent_, QStringLiteral("导出扫描会话"),
                             QStringLiteral("扫描会话已导出到：\n%1").arg(destination));
}

void RecoveryFlowController::onRepairPageBack()
{
    if (repairReturnToScan_) {
        repairReturnToScan_ = false;
        setState(FlowState::Results);
        if (scanPage_ && outerStack_) {
            outerStack_->setCurrentWidget(scanPage_);
        }
        return;
    }
    setState(FlowState::MoreTools);
    navigateToPage(FlowState::MoreTools);
}

void RecoveryFlowController::onRequestRepairFiles(const QStringList& localPaths)
{
    if (!repairPage_ || localPaths.isEmpty()) {
        return;
    }
    repairReturnToScan_ = (state_ == FlowState::Results || state_ == FlowState::Scanning);
    repairPage_->setInputFiles(localPaths, false);
    setState(FlowState::Repair);
    navigateToPage(FlowState::Repair);
}

void RecoveryFlowController::onScanPageBack()
{
    LOG_INFO(L"RecoveryFlowController::onScanPageBack state=" +
             std::to_wstring(static_cast<int>(state_)));

    if (state_ == FlowState::Scanning) {
        if (scanCtrl_) {
            scanCtrl_->stopScan();
        }
    }

    const FlowState backTarget = (prevState_ == FlowState::SDCard)
                                     ? FlowState::SDCard
                                     : FlowState::LocationSelect;
    setState(backTarget);
    navigateToPage(backTarget);
}

void RecoveryFlowController::onScanFinished(bool ok, const QString &msg)
{
    LOG_INFO(L"RecoveryFlowController::onScanFinished"
             L" ok=" + std::to_wstring(ok ? 1 : 0) +
             L" msg=" + msg.toStdWString());

    if (ok) {
        setState(FlowState::Results);
        sessionDirty_ = true;
        saveCurrentSession();
    }
}

void RecoveryFlowController::onSidebarChanged(int pageIndex)
{
    LOG_INFO(L"RecoveryFlowController::onSidebarChanged pageIndex=" +
             std::to_wstring(pageIndex));

    if (state_ == FlowState::Scanning) {
        const auto reply = QMessageBox::question(
            uiParent_,
            QStringLiteral("确认离开"),
            QStringLiteral("扫描正在进行，切换页面将停止当前扫描。是否继续？"),
            QMessageBox::Yes | QMessageBox::No);
        if (reply != QMessageBox::Yes) {
            return;
        }
        if (scanCtrl_) {
            scanCtrl_->stopScan();
        }
    }

    switch (pageIndex) {
    case 0:
        navigateToLocation();
        break;
    case 1:
        navigateToSDCard();
        break;
    case 2:
        navigateToMoreTools();
        break;
    case 3:
        setState(FlowState::Repair);
        navigateToPage(FlowState::Repair);
        break;
    default:
        navigateToLocation();
        break;
    }
}

// ── Session management ───────────────────────────────────────────────────────

void RecoveryFlowController::tryLoadLastSession()
{
    if (testModeEnabled()) {
        LOG_INFO(L"RecoveryFlowController::tryLoadLastSession skipped in test mode");
        return;
    }
    QSettings settings;
    const QString path = settings.value(QStringLiteral("session/lastPath")).toString();
    if (path.isEmpty() || !QFile::exists(path)) {
        return;
    }

    const auto reply = QMessageBox::question(
        uiParent_,
        QStringLiteral("加载上次扫描"),
        QStringLiteral("检测到上次的扫描结果，是否加载？"),
        QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes) {
        settings.remove(QStringLiteral("session/lastPath"));
        return;
    }

    ScanResult result;
    Partition partition;
    const std::wstring wpath = path.toStdWString();
    if (!ScanSerializer::Load(wpath, result, partition)) {
        LOG_INFO(L"RecoveryFlowController::tryLoadLastSession – Load failed: " + wpath);
        QMessageBox::warning(uiParent_,
                             QStringLiteral("加载失败"),
                             QStringLiteral("无法加载上次的扫描结果文件，文件可能已损坏。"));
        settings.remove(QStringLiteral("session/lastPath"));
        return;
    }

    if (!scanCtrl_ || !scanCtrl_->resultStore()) {
        return;
    }

    ScanResultStore *store = scanCtrl_->resultStore();
    store->clear();
    store->setScanVolumePartition(partition);

    for (const FileNode &node : result.files) {
        ScanFileDto dto;
        dto.id = static_cast<quint64>(node.id);
        dto.name = QString::fromStdWString(node.name);
        dto.fullPath = QString::fromStdWString(node.originalPath);
        dto.relativePath = dto.fullPath;
        const int colon = dto.relativePath.indexOf(QLatin1Char(':'));
        if (colon >= 0) {
            dto.relativePath = dto.relativePath.mid(colon + 1);
        }
        if (dto.relativePath.startsWith(QLatin1Char('\\')) || dto.relativePath.startsWith(QLatin1Char('/'))) {
            dto.relativePath = dto.relativePath.mid(1);
        }
        dto.extension = QString::fromStdWString(node.extension);
        dto.fsType = QString::fromStdWString(node.fsType);
        dto.sizeBytes = static_cast<qint64>(node.size);
        dto.parentId = static_cast<quint64>(node.parentId);
        dto.isDirectory = node.isDirectory;
        dto.isDeleted = (node.status != FileStatus::Normal);
        dto.modifyTimeFt = node.modifyTime;

        store->upsert(dto);
        store->setRecoveredFileNode(dto.id, node);
    }

    lastSessionPath_ = path;
    sessionDirty_ = false;

    LOG_INFO(L"RecoveryFlowController::tryLoadLastSession – loaded " +
             std::to_wstring(result.files.size()) + L" files");

    const QString driveLetter = QString::fromStdWString(partition.driveLetter);
    if (scanPage_) {
        scanPage_->setResultStore(store);
        scanPage_->beginScanSession(QStringLiteral("本地磁盘"), driveLetter);
        scanPage_->setScanFinished(true, QStringLiteral("已加载上次扫描结果"));
    }

    setState(FlowState::Results);
    navigateToPage(FlowState::Results);
}

void RecoveryFlowController::saveCurrentSession()
{
    if (!scanCtrl_ || !scanCtrl_->resultStore()) {
        return;
    }

    ScanResultStore *store = scanCtrl_->resultStore();
    Partition partition;
    if (!store->tryGetScanVolumePartition(&partition)) {
        LOG_INFO(L"RecoveryFlowController::saveCurrentSession – no partition info, skipping");
        return;
    }

    const QHash<quint64, FileNode> nodes = store->allRecoveredFileNodes();
    if (nodes.isEmpty()) {
        LOG_INFO(L"RecoveryFlowController::saveCurrentSession – no file nodes, skipping");
        return;
    }

    ScanResult result;
    result.files.reserve(static_cast<size_t>(nodes.size()));
    for (auto it = nodes.cbegin(); it != nodes.cend(); ++it) {
        result.files.push_back(it.value());
    }
    result.foundCount = static_cast<uint32_t>(nodes.size());
    result.totalRecords = result.foundCount;

    const QString dir = sessionDir();
    QDir().mkpath(dir);
    const std::wstring filename = ScanSerializer::SuggestFilename(partition);
    const QString filePath = dir + QStringLiteral("/") + QString::fromStdWString(filename);
    const std::wstring wpath = filePath.toStdWString();

    if (ScanSerializer::Save(wpath, result, partition)) {
        lastSessionPath_ = filePath;
        sessionDirty_ = false;
        QSettings settings;
        settings.setValue(QStringLiteral("session/lastPath"), filePath);
        LOG_INFO(L"RecoveryFlowController::saveCurrentSession – saved to " + wpath);
    } else {
        LOG_INFO(L"RecoveryFlowController::saveCurrentSession – Save failed: " + wpath);
    }
}

bool RecoveryFlowController::hasUnsavedSession() const
{
    return sessionDirty_;
}

// ── Private ──────────────────────────────────────────────────────────────────

void RecoveryFlowController::setState(FlowState newState)
{
    if (state_ == newState) {
        return;
    }
    prevState_ = state_;
    state_ = newState;
    LOG_INFO(L"RecoveryFlowController::setState " +
             std::to_wstring(static_cast<int>(prevState_)) +
             L" → " + std::to_wstring(static_cast<int>(state_)));
    emit stateChanged(state_);
}

void RecoveryFlowController::navigateToPage(FlowState target)
{
    if (!outerStack_ || !wizardPages_ || !wizardShell_) {
        return;
    }

    switch (target) {
    case FlowState::Idle:
    case FlowState::LocationSelect:
        outerStack_->setCurrentWidget(wizardShell_);
        if (locationPage_) {
            wizardPages_->setCurrentWidget(locationPage_);
        }
        break;
    case FlowState::SDCard:
        outerStack_->setCurrentWidget(wizardShell_);
        if (sdPage_) {
            wizardPages_->setCurrentWidget(sdPage_);
        }
        break;
    case FlowState::MoreTools:
    case FlowState::Imaging:
    case FlowState::SmartHealth:
        outerStack_->setCurrentWidget(wizardShell_);
        if (toolsPage_) {
            wizardPages_->setCurrentWidget(toolsPage_);
        }
        break;
    case FlowState::Repair:
        outerStack_->setCurrentWidget(wizardShell_);
        if (repairPage_) {
            wizardPages_->setCurrentWidget(repairPage_);
        }
        break;
    case FlowState::Scanning:
    case FlowState::Results:
    case FlowState::RecoveryTarget:
    case FlowState::RecoveryComplete:
        if (scanPage_) {
            outerStack_->setCurrentWidget(scanPage_);
        }
        break;
    }
}

QString RecoveryFlowController::sessionDir() const
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/sessions");
}
