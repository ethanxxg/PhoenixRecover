#pragma once

#include <QtCore/QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class QWidget;
class QStackedWidget;
class QProgressDialog;

class RecoveryScanController;
class RecoveryScanPage;
class RecoveryLocationPage;
class SDCardConnectionPage;
class MoreRecoveryToolsPage;
class FileRepairPage;
struct ImageResult;
struct PartitionCandidate;

enum class FlowState {
    Idle,
    LocationSelect,
    Scanning,
    Results,
    RecoveryTarget,
    RecoveryComplete,
    SDCard,
    MoreTools,
    Imaging,
    Repair,
    SmartHealth,
};

class RecoveryFlowController : public QObject
{
    Q_OBJECT

public:
    explicit RecoveryFlowController(QWidget *uiParent, QObject *parent = nullptr);
    ~RecoveryFlowController() override;

    void init(QStackedWidget *outerStack,
              QStackedWidget *wizardPages,
              QWidget *wizardShell,
              RecoveryScanController *scanCtrl,
              RecoveryScanPage *scanPage,
              RecoveryLocationPage *locationPage,
              SDCardConnectionPage *sdPage,
              MoreRecoveryToolsPage *toolsPage,
              FileRepairPage *repairPage);

    FlowState currentState() const;

    void navigateToLocation();
    void navigateToSDCard();
    void navigateToMoreTools();
    void startScan(const QString &locationOption, const QString &driveLetter);
    void showResults();
    void startRecovery();
    void showComplete();
    void navigateBack();

    void tryLoadLastSession();
    void saveCurrentSession();
    bool hasUnsavedSession() const;

public slots:
    void onStartScanRequested(const QString &location, const QString &disk);
    void onSdCardRecoveryRequested();
    void onToolRecoveryRequested(const QString &toolName);
    void onScanPageBack();
    void onRepairPageBack();
    void onRequestRepairFiles(const QStringList& localPaths);
    void onPartitionScanRequested(int physicalDiskIdx,
                                  quint64 startSector,
                                  quint64 totalSectors,
                                  const QString &fsType);
    void onSessionLoadRequested(const QString &path);
    void onSessionDeleteRequested(const QString &path);
    void onSessionImportRequested();
    void onSessionExportRequested(const QString &path);
    void onScanFinished(bool ok, const QString &msg);
    void onSidebarChanged(int pageIndex);

signals:
    void stateChanged(FlowState newState);

private:
    void setState(FlowState newState);
    void navigateToPage(FlowState target);
    QString sessionDir() const;
    void startImaging();
    void finishImaging(const ImageResult& result);
    void startPartitionProbe();
    void finishPartitionProbe(const std::vector<PartitionCandidate>& candidates);

    FlowState state_ = FlowState::Idle;
    FlowState prevState_ = FlowState::Idle;

    QWidget *uiParent_ = nullptr;
    QStackedWidget *outerStack_ = nullptr;
    QStackedWidget *wizardPages_ = nullptr;
    QWidget *wizardShell_ = nullptr;
    RecoveryScanController *scanCtrl_ = nullptr;
    RecoveryScanPage *scanPage_ = nullptr;
    RecoveryLocationPage *locationPage_ = nullptr;
    SDCardConnectionPage *sdPage_ = nullptr;
    MoreRecoveryToolsPage *toolsPage_ = nullptr;
    FileRepairPage *repairPage_ = nullptr;
    QString lastSessionPath_;
    QString lastImagePath_;
    bool sessionDirty_ = false;
    bool repairReturnToScan_ = false;

    QProgressDialog *imagingProgress_ = nullptr;
    std::shared_ptr<std::atomic<bool>> imagingCancel_;
    std::thread imagingThread_;
    QProgressDialog *partitionProgress_ = nullptr;
    std::shared_ptr<std::atomic<bool>> partitionCancel_;
    std::thread partitionThread_;
    int partitionDiskIndex_ = -1;
};
