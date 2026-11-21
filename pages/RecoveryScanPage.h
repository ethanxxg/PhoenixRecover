#pragma once

#include <QtCore/QHash>
#include <QtCore/QElapsedTimer>
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QTimer>
#include <QtCore/QVector>
#include <QtGui/QPixmap>
#include <QtWidgets/QWidget>
#include <QTableWidgetItem>

#include <atomic>
#include <memory>
#include <vector>

#include "../controllers/ScanTypes.h"
#include "../recover/FileNode.h"

struct RecoverBatchResult;

class ScanResultStore;
class ScanResultModel;
class ScanResultFilterModel;
class QLabel;
class QProgressBar;
class QPushButton;
class QCheckBox;
class QLineEdit;
class QComboBox;
class QTabWidget;
class QTableView;
class QTreeWidget;
class QTreeWidgetItem;
class QTableWidget;
class QListWidget;
class QListWidgetItem;
class QFrame;
class QStackedWidget;
class QResizeEvent;
class QThread;
class QProgressDialog;

class ScanProgressBar;
class PreviewPanel;
class ScanFooterBar;
class FilterPanel;

class RecoveryScanPage : public QWidget
{
    Q_OBJECT

public:
    explicit RecoveryScanPage(QWidget *parent = nullptr);
    ~RecoveryScanPage() override;
    void setResultStore(ScanResultStore *resultStore);

    void beginScanSession(const QString &locationOption,
                          const QString &diskLetter);
    void setNonDiskPlaceholder(const QString &locationOption);

public slots:
    void appendScanFileBatch(const QVector<ScanFileDto> &batch);
    void onScanIndexUpdated(quint64 revision);
    void setScanProgress(int percent, quint32 foundCount, quint64 scannedBytes, quint64 totalBytes, quint64 elapsedMs, const QString &phaseText);
    void setScanFinished(bool ok, const QString &message);

signals:
    void requestBack();
    void requestStopScan();
    void requestRepairFiles(const QStringList& localPaths);

private slots:
    void applyGridThumbnail(quint64 fileId, quint64 listGeneration, QPixmap pixmap);
    void decodeVideoThumbnailOnMainThread(quint64 fileId, quint64 listGeneration, QString tempFilePath);
    void decodeImageThumbnailOnMainThread(quint64 fileId, quint64 listGeneration, QString tempFilePath);
    void decodeDocumentThumbnailOnMainThread(quint64 fileId, quint64 listGeneration, QString tempFilePath);
    void onRecoverAllClicked();
    void onRepairRequested();
    void onRetryFailedClicked();
    void hideRecoveryCompletePanel();
    void exportRecoveryDetailReport();

private:
    void onPathTreeCurrentChanged(QTreeWidgetItem *current);
    void onTypeTreeCurrentChanged(QTreeWidgetItem *current);
    void onLeftTabChanged(int index);
    void onPathTreeItemChanged(QTreeWidgetItem *item, int column);
    void onTypeTreeItemChanged(QTreeWidgetItem *item, int column);
    void onTreeItemExpanded(QTreeWidgetItem *item);
    void onTreeItemCollapsed(QTreeWidgetItem *item);
    void onResultViewportChanged();
    void processUiPump();
    void onFilterChanged(const QString &keyword);

    void insertPathForDto(const ScanFileDto &dto);
    void addFileToTypeTree(const ScanFileDto &dto);
    void rebuildTreeViews();
    QStringList checkedPathTreeItems() const;
    QStringList checkedTypeTreeItems() const;
    void restoreTreeChecks(QTreeWidget *tree, const QStringList &keys, bool pathTree);
    void loadMockScanData();
    void setResultsMode(bool enabled);
    void refillTableFromPathItem(QTreeWidgetItem *item);
    void refillTableFromTypeItem(QTreeWidgetItem *item);
    void refillTableFromSelection();
    void setTreeSubtreeCheckState(QTreeWidgetItem *item, Qt::CheckState state);
    void updateParentCheckState(QTreeWidgetItem *item);
    QTreeWidgetItem *findPathTreeItemByRelativePath(const QString &relPath) const;
    bool isPathUnderCheckedFolder(const QString &folderRelPath, bool isDirectory) const;
    QCheckBox *tableRowCheckBoxAt(int row) const;
    QCheckBox *gridCardCheckBoxAt(int index) const;
    QWidget *createTableCheckBoxWidget(quint64 id, bool isDirectory, const QString &relPath, Qt::CheckState state);
    void syncTableChecksFromTreeSelection();
    void updateHeaderSelectAllCheckState();
    void updateDetailFromTableRow(int row);
    void setPreviewMode(bool enabled);
    void fillListView(const QList<ScanFileDto> &files, Qt::CheckState defaultState);
    void fillGridView(const QList<ScanFileDto> &files, Qt::CheckState defaultState);
    void appendDtoToListView(const ScanFileDto &dto, Qt::CheckState state);
    void appendDtoToGridView(const ScanFileDto &dto, Qt::CheckState state);
    QWidget *createGridCard(const ScanFileDto &dto, Qt::CheckState state);
    void scheduleGridThumbnail(quint64 fileId);
    bool currentSelectionAcceptsDto(const ScanFileDto &dto, Qt::CheckState *state = nullptr) const;
    bool tableContainsDto(quint64 id) const;
    bool gridContainsDto(quint64 id) const;
    void markTreeItemCaptionDirty(QTreeWidgetItem *item);
    void flushDirtyTreeCaptions();
    void refreshResultsHeader();
    int ensurePathChildrenLoaded(QTreeWidgetItem *item, int maxAppend, const char *source);
    void ensureTypeChildrenLoaded(QTreeWidgetItem *item);
    QTreeWidgetItem *ensurePathTreeItemByRelativePath(const QString &relPath);
    QTreeWidgetItem *ensureTypeTreeItemByKey(const QString &category, const QString &subLabel);
    QTreeWidgetItem *createPathTreeItem(const QString &relPath, const QString &name) const;
    QTreeWidgetItem *createTypeCategoryItem(const QString &category) const;
    QTreeWidgetItem *createTypeSubItem(const QString &category, const QString &subLabel) const;
    void updateTreeItemCount(QTreeWidgetItem *item);
    void refreshCheckedKeyCaches();
    Qt::CheckState computePathParentState(const QString &relPath) const;
    Qt::CheckState computeTypeParentState(const QString &key) const;
    bool isTypeUnderCheckedNode(const QString &category, const QString &subLabel) const;
    QList<ScanFileDto> collectVisibleFilesForPath(const QString &folderRel) const;
    QList<ScanFileDto> collectVisibleFilesForType(QTreeWidgetItem *item) const;
    void resetVisibleResults(const QList<ScanFileDto> &visibleFiles, Qt::CheckState defaultState);
    void ensureVisibleResultRowsLoaded(bool force = false);
    int targetVisibleRowCount() const;
    void appendTableRow(const ScanFileDto &dto, Qt::CheckState state);
    void appendGridRow(const ScanFileDto &dto, Qt::CheckState state);
    bool matchesSearch(const ScanFileDto &dto) const;
    bool matchesSearch(const QString &name, const QString &type, const QString &path) const;
    void refreshSearchHighlight();
    void refreshTreeSearchHighlight(QTreeWidget *tree);
    bool treeItemMatchesSearch(QTreeWidgetItem *item, bool pathTree) const;
    QTreeWidgetItem *findFirstSearchMatch(QTreeWidget *tree, bool pathTree) const;
    QVector<quint64> checkedRecoverableIdsFromTable() const;
    quint64 checkedRecoverableBytes() const;
    QVector<quint64> repairCandidateIdsFromSelection() const;
    QStringList exportFilesToRepairStaging(const QVector<quint64>& fileIds);
    bool isRepairableDto(const ScanFileDto& dto) const;
    void showRecoveryCompletePanel(const RecoverBatchResult &batch,
                                   const QString &targetDir,
                                   const QString &detailText);
    void applyRecoveryResultFilter(int filterIndex);

protected:
    void resizeEvent(QResizeEvent *event) override;

    static QString relativeVolumePath(const QString &fullPath, const QString &diskLetter);
    static QString fileTypeDisplay(const ScanFileDto &dto);
    static QString formatSize(qint64 bytes);
    static QString formatFileTime(quint64 fileTime100ns);
    static QString categoryForExtension(const QString &ext);

    // Sub-components
    ScanProgressBar *scanProgressBar_ = nullptr;
    PreviewPanel *previewPanel_ = nullptr;
    ScanFooterBar *scanFooterBar_ = nullptr;
    FilterPanel *filterPanel_ = nullptr;

    QLabel *pathLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QLabel *resultsMetaLabel_ = nullptr;
    QLabel *metricFoundLabel_ = nullptr;
    QLabel *metricScannedLabel_ = nullptr;
    QLabel *metricTotalLabel_ = nullptr;
    QLabel *metricElapsedLabel_ = nullptr;
    QLabel *metricSpeedLabel_ = nullptr;
    QFrame *scanStatusCard_ = nullptr;
    QLabel *scanPercentLabel_ = nullptr;
    QLabel *scanPhaseLabel_ = nullptr;
    QLabel *scanDescriptionLabel_ = nullptr;
    QLabel *scanProgressTextLabel_ = nullptr;
    QLabel *scanEtaLabel_ = nullptr;
    QLabel *scanFoundLabel_ = nullptr;
    QLabel *scanScannedLabel_ = nullptr;
    QLabel *scanSpeedLabel_ = nullptr;
    QProgressBar *scanCardProgressBar_ = nullptr;
    QPushButton *scanPreviewResultsBtn_ = nullptr;
    QWidget *resultsContentWrap_ = nullptr;
    QFrame *resultsSummaryWrap_ = nullptr;
    QLabel *resultsFilesSummaryLabel_ = nullptr;
    QLabel *resultsBytesSummaryLabel_ = nullptr;
    QLabel *resultsSelectedSummaryLabel_ = nullptr;
    QLabel *footerFoundLabel_ = nullptr;
    QLabel *footerSubLabel_ = nullptr;
    QLabel *footerScannedLabel_ = nullptr;
    QLabel *footerElapsedLabel_ = nullptr;
    QLabel *footerSpeedLabel_ = nullptr;
    QProgressBar *progressBar_ = nullptr;
    QTabWidget *leftTabs_ = nullptr;
    QTreeWidget *pathTree_ = nullptr;
    QTreeWidget *typeTree_ = nullptr;
    QTableWidget *table_ = nullptr;
    QFrame *previewFrame_ = nullptr;
    QFrame *previewDetailFrame_ = nullptr;
    QLabel *previewTitleLabel_ = nullptr;
    QLabel *previewMetaLabel_ = nullptr;
    QLabel *previewContentLabel_ = nullptr;
    quint64 previewCurrentFileId_ = 0;

    ScanResultStore *resultStore_ = nullptr;
    QTimer *uiPumpTimer_ = nullptr;
    quint64 pendingStoreRevision_ = 0;
    quint64 lastVisibleStoreRevision_ = 0;
    QTreeWidgetItem *pathRoot_ = nullptr;
    QTreeWidgetItem *reconstructedRoot_ = nullptr;
    QHash<quint64, ScanFileDto> files_;
    QHash<QString, QTreeWidgetItem *> typeCategoryItems_;
    QHash<QString, QTreeWidgetItem *> typeSubcategoryItems_;
    QHash<QString, QStringList> pathChildDirectories_;
    QHash<QString, int> pathNodeCounts_;
    QHash<QString, QVector<quint64> > pathDirectChildIds_;
    QHash<QString, QStringList> typeCategoryChildren_;
    QHash<QString, int> typeNodeCounts_;
    QHash<QString, QVector<quint64> > typeNodeFileIds_;
    QSet<QString> checkedPathKeys_;
    QSet<QString> checkedTypeKeys_;
    QList<ScanFileDto> visibleResults_;
    QVector<quint64> visibleTypeCandidateIds_;
    int visibleTypeCandidateCursor_ = 0;
    bool visibleResultsUseTypePaging_ = false;
    bool visibleResultsContinuationScheduled_ = false;
    quint64 visibleResultsGeneration_ = 0;
    Qt::CheckState visibleResultsDefaultState_ = Qt::Unchecked;
    int visibleResultsLoaded_ = 0;

    QString diskLetter_;
    QString locationOption_;
    QString searchKeyword_;
    bool scanRunning_ = false;
    quint32 foundTotal_ = 0;
    quint64 scannedBytes_ = 0;
    quint64 totalBytes_ = 0;
    bool useMockData_ = false;
    bool syncingTreeChecks_ = false;
    bool syncingTableChecks_ = false;
    bool syncingHeaderCheck_ = false;
    bool resultsMode_ = false;
    bool previewMode_ = false;
    bool batchingUiUpdate_ = false;
    bool suppressTreeExpandLoad_ = false;
    QSet<QTreeWidgetItem *> dirtyTreeCaptionItems_;
    QElapsedTimer progressUiThrottleTimer_;
    QElapsedTimer scanElapsedTimer_;
    QElapsedTimer scanBatchLogTimer_;
    int lastProgressUiPercent_ = -1;
    quint32 lastProgressUiFound_ = 0;
    quint64 lastProgressUiScanned_ = 0;
    quint64 lastProgressUiTotal_ = 0;
    quint64 lastProgressUiElapsedMs_ = 0;
    quint64 scanBatchReceiveCount_ = 0;
    quint64 scanBatchItemCount_ = 0;
    bool tabRefreshScheduled_ = false;
    QString lastPathRefreshKey_;
    int lastPathRefreshItemCount_ = -1;
    quint64 lastPathRefreshRevision_ = 0;
    QString lastTypeRefreshKey_;
    int lastTypeRefreshItemCount_ = -1;
    quint64 lastTypeRefreshRevision_ = 0;

    QPushButton *backBtn_ = nullptr;
    QPushButton *stopScanBtn_ = nullptr;
    QPushButton *viewToggleBtn_ = nullptr;
    QPushButton *searchBtn_ = nullptr;
    QPushButton *recoverAllBtn_ = nullptr;
    QCheckBox *keepDirStructureCheck_ = nullptr;
    QCheckBox *formatAwareFragmentCheck_ = nullptr;
    QLineEdit *searchEdit_ = nullptr;
    QComboBox *sortCombo_ = nullptr;
    QWidget *scanMetricWrap_ = nullptr;
    QWidget *scanFooter_ = nullptr;
    bool recoveryRunning_ = false;
    QThread *recoveryThread_ = nullptr;
    QProgressDialog *recoveryProgressDialog_ = nullptr;
    std::shared_ptr<std::atomic<bool>> recoveryCancel_;
    std::shared_ptr<std::atomic<bool>> recoveryPause_;
    std::shared_ptr<std::vector<FileNode>> recoveryNodeStorage_;
    QFrame *recoveryCompletePanel_ = nullptr;
    QLabel *recoveryCompleteSummaryLabel_ = nullptr;
    QLabel *recoveryCompletePathLabel_ = nullptr;
    QTableWidget *recoveryCompleteTable_ = nullptr;
    QComboBox *recoveryResultFilterCombo_ = nullptr;
    QPushButton *recoveryCompleteBackBtn_ = nullptr;
    QPushButton *recoveryCompleteOpenDirBtn_ = nullptr;
    QPushButton *recoveryCompleteExportBtn_ = nullptr;
    QPushButton *recoveryPauseBtn_ = nullptr;
    QPushButton *recoveryRetryFailedBtn_ = nullptr;
    QSet<quint64> recoveryLastFailedIds_;
    bool recoveryRetryPending_ = false;
    QString recoveryLastDetailText_;
    QString recoveryLastTargetDir_;
    QCheckBox *headerSelectAllCheckBox_ = nullptr;
    QTableView *tableView_ = nullptr;
    ScanResultModel *scanModel_ = nullptr;
    ScanResultFilterModel *filterModel_ = nullptr;
    QTimer *searchTimer_ = nullptr;
    QString pendingSearchText_;
    QListWidget *gridView_ = nullptr;
};
