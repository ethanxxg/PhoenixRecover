#pragma once

#include "../repair/RepairReport.h"

#include <QtWidgets/QWidget>

class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;
class FileRepairController;

class FileRepairPage : public QWidget
{
    Q_OBJECT

public:
    explicit FileRepairPage(QWidget *parent = nullptr);

    void setInputFiles(const QStringList& paths, bool autoStartRepair = false);

signals:
    void requestBack();

private slots:
    void onAddFiles();
    void onChooseOutputDir();
    void onStartRepair();
    void onCancelRepair();
    void onExportReport();
    void onPreviewRepaired();
    void onBatchFinished(const QVector<RepairJobRecord> &jobs);

private:
    void setRunningUi(bool running);
    void appendLogLine(const QString &line);

    FileRepairController *controller_ = nullptr;
    QListWidget *fileList_ = nullptr;
    QLabel *outputLabel_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QProgressBar *progressBar_ = nullptr;
    QListWidget *logList_ = nullptr;
    QPushButton *startBtn_ = nullptr;
    QPushButton *cancelBtn_ = nullptr;
    QPushButton *exportBtn_ = nullptr;
    QPushButton *previewBtn_ = nullptr;

    QString outputDir_;
    bool pendingAutoStart_ = false;
    QVector<RepairJobRecord> lastJobs_;
};
