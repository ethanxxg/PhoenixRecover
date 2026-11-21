#pragma once

#include <QString>
#include <QStringList>
#include <QtWidgets/QStyle>
#include <QtWidgets/QWidget>

#include <string>
#include <vector>

struct PartitionCandidate;

class QLabel;
class QComboBox;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QStackedWidget;
class QToolButton;

class MoreRecoveryToolsPage : public QWidget
{
    Q_OBJECT

public:
    explicit MoreRecoveryToolsPage(QWidget *parent = nullptr);

    void showToolPage(const QString &toolName);
    void showHome();
    void setSmartSummary(const QString &summary);
    void setPartitionResults(const QString &summary);
    void setPartitionCandidates(int physicalDiskIdx,
                                const std::vector<PartitionCandidate> &candidates,
                                const QString &summary);
    void setSessionEntries(const QStringList &labels,
                           const QStringList &paths,
                           const QString &summary);

signals:
    // UI 只表达意图；具体引擎调用由 RecoveryFlowController 完成。
    void requestToolRecovery(const QString &toolName);
    void requestPartitionScan(int physicalDiskIdx,
                              quint64 startSector,
                              quint64 totalSectors,
                              const QString &fsType);
    void requestSessionLoad(const QString &path);
    void requestSessionDelete(const QString &path);
    void requestSessionImport();
    void requestSessionExport(const QString &path);

private:
    QToolButton *makeToolCard(const QString &title,
                              const QString &subtitle,
                              QStyle::StandardPixmap iconType);
    void updateToolDetail(const QString &title,
                          const QString &description,
                          const QString &content,
                          const QString &actionText,
                          bool actionVisible = true);
    void refillSessionCombo(const QString &keyword = QString());

    QStackedWidget *stack_ = nullptr;
    QWidget *homePage_ = nullptr;
    QWidget *detailPage_ = nullptr;
    QLabel *detailTitle_ = nullptr;
    QLabel *detailDescription_ = nullptr;
    QPlainTextEdit *detailContent_ = nullptr;
    QComboBox *partitionCandidateCombo_ = nullptr;
    QComboBox *sessionCombo_ = nullptr;
    QLineEdit *sessionSearch_ = nullptr;
    QPushButton *sessionDeleteButton_ = nullptr;
    QPushButton *sessionRefreshButton_ = nullptr;
    QPushButton *sessionImportButton_ = nullptr;
    QPushButton *sessionExportButton_ = nullptr;
    QPushButton *detailAction_ = nullptr;
    QString currentTool_;
    QStringList sessionLabels_;
    QStringList sessionPaths_;
};
