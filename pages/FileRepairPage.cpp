#include "FileRepairPage.h"

#include "../controllers/FileRepairController.h"
#include "../repair/RepairReport.h"

#include <QtWidgets/QFileDialog>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>
#include <QtCore/QStandardPaths>
#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtGui/QDesktopServices>

FileRepairPage::FileRepairPage(QWidget *parent)
    : QWidget(parent)
    , controller_(new FileRepairController(this))
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(14);

    auto *title = new QLabel(QStringLiteral("文件修复"), this);
    title->setStyleSheet(QStringLiteral("font-size: 20px; font-weight: 600; color: #111827;"));
    layout->addWidget(title);

    auto *hint = new QLabel(
        QStringLiteral("支持 ZIP、Office(docx/pptx/xlsx)、SQLite(db/sqlite)、图片与视频；修复后自动自检并写入报告。"
                       "部分格式需 FFmpeg。"),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("font-size: 14px; color: #6b7280;"));
    layout->addWidget(hint);

    fileList_ = new QListWidget(this);
    fileList_->setMinimumHeight(120);
    layout->addWidget(fileList_);

    auto *fileRow = new QHBoxLayout();
    auto *addBtn = new QPushButton(QStringLiteral("添加文件…"), this);
    auto *clearBtn = new QPushButton(QStringLiteral("清空列表"), this);
    fileRow->addWidget(addBtn);
    fileRow->addWidget(clearBtn);
    fileRow->addStretch(1);
    layout->addLayout(fileRow);

    outputLabel_ = new QLabel(QStringLiteral("输出目录：未选择"), this);
    outputLabel_->setStyleSheet(QStringLiteral("font-size: 13px; color: #374151;"));
    layout->addWidget(outputLabel_);

    auto *outRow = new QHBoxLayout();
    auto *outBtn = new QPushButton(QStringLiteral("选择输出目录…"), this);
    outRow->addWidget(outBtn);
    outRow->addStretch(1);
    layout->addLayout(outRow);

    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    layout->addWidget(progressBar_);

    statusLabel_ = new QLabel(QStringLiteral("就绪"), this);
    statusLabel_->setStyleSheet(QStringLiteral("font-size: 13px; color: #4b5563;"));
    layout->addWidget(statusLabel_);

    logList_ = new QListWidget(this);
    logList_->setMinimumHeight(140);
    layout->addWidget(logList_, 1);

    auto *bottom = new QHBoxLayout();
    auto *backBtn = new QPushButton(QStringLiteral("返回"), this);
    cancelBtn_ = new QPushButton(QStringLiteral("取消"), this);
    exportBtn_ = new QPushButton(QStringLiteral("导出报告"), this);
    previewBtn_ = new QPushButton(QStringLiteral("预览修复结果"), this);
    startBtn_ = new QPushButton(QStringLiteral("开始修复"), this);
    startBtn_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #1890ff; color: white; border: none; border-radius: 8px; padding: 8px 16px; }"
        "QPushButton:hover { background: #1677ff; }"
        "QPushButton:disabled { background: #9ca3af; }"));

    bottom->addWidget(backBtn);
    bottom->addStretch(1);
    bottom->addWidget(previewBtn_);
    bottom->addWidget(exportBtn_);
    bottom->addWidget(cancelBtn_);
    bottom->addWidget(startBtn_);
    layout->addLayout(bottom);

    exportBtn_->setEnabled(false);
    previewBtn_->setEnabled(false);
    cancelBtn_->setEnabled(false);

    const QString defaultOut = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                               + QStringLiteral("/PhoenixRecover/repaired");
    outputDir_ = defaultOut;
    outputLabel_->setText(QStringLiteral("输出目录：%1").arg(outputDir_));

    connect(addBtn, &QPushButton::clicked, this, &FileRepairPage::onAddFiles);
    connect(clearBtn, &QPushButton::clicked, fileList_, &QListWidget::clear);
    connect(outBtn, &QPushButton::clicked, this, &FileRepairPage::onChooseOutputDir);
    connect(startBtn_, &QPushButton::clicked, this, &FileRepairPage::onStartRepair);
    connect(cancelBtn_, &QPushButton::clicked, this, &FileRepairPage::onCancelRepair);
    connect(exportBtn_, &QPushButton::clicked, this, &FileRepairPage::onExportReport);
    connect(previewBtn_, &QPushButton::clicked, this, &FileRepairPage::onPreviewRepaired);
    connect(backBtn, &QPushButton::clicked, this, &FileRepairPage::requestBack);

    connect(controller_, &FileRepairController::progress, this, [this](int cur, int total, const QString &name) {
        progressBar_->setMaximum(total > 0 ? total : 1);
        progressBar_->setValue(cur);
        statusLabel_->setText(QStringLiteral("正在修复 (%1/%2): %3").arg(cur).arg(total).arg(name));
    });
    connect(controller_, &FileRepairController::jobFinished, this, [this](const RepairJobRecord &rec) {
        const QString valTag = rec.result.validated ? QStringLiteral("VALID")
                                                    : (rec.result.success ? QStringLiteral("UNVALIDATED")
                                                                          : QStringLiteral("FAIL"));
        const QString line = QStringLiteral("[%1] %2 — %3")
                                 .arg(valTag, QFileInfo(rec.inputPath).fileName(), rec.result.message);
        appendLogLine(line);
    });
    connect(controller_, &FileRepairController::batchFinished, this, &FileRepairPage::onBatchFinished);
}

void FileRepairPage::setInputFiles(const QStringList& paths, bool autoStartRepair)
{
    fileList_->clear();
    for (const QString& p : paths) {
        if (!p.isEmpty() && QFile::exists(p)) {
            fileList_->addItem(p);
        }
    }
    pendingAutoStart_ = autoStartRepair && fileList_->count() > 0;
    if (pendingAutoStart_ && !controller_->isRunning()) {
        QTimer::singleShot(0, this, [this]() {
            if (pendingAutoStart_) {
                pendingAutoStart_ = false;
                onStartRepair();
            }
        });
    }
}

void FileRepairPage::onAddFiles()
{
    const QStringList paths = QFileDialog::getOpenFileNames(
        this,
        QStringLiteral("选择待修复文件"),
        QString(),
        QStringLiteral("全部支持类型 (*.zip *.docx *.pptx *.xlsx *.jpg *.jpeg *.png *.gif *.webp "
                       "*.heic *.tif *.tiff *.bmp *.mp4 *.mov *.m4v *.mkv *.avi *.webm *.flv);;"
                       "所有文件 (*.*)"));
    for (const QString &p : paths) {
        if (fileList_->findItems(p, Qt::MatchExactly).isEmpty()) {
            fileList_->addItem(p);
        }
    }
}

void FileRepairPage::onChooseOutputDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择输出目录"), outputDir_);
    if (!dir.isEmpty()) {
        outputDir_ = dir;
        outputLabel_->setText(QStringLiteral("输出目录：%1").arg(outputDir_));
    }
}

void FileRepairPage::onStartRepair()
{
    QStringList inputs;
    for (int i = 0; i < fileList_->count(); ++i) {
        inputs << fileList_->item(i)->text();
    }
    if (inputs.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("文件修复"), QStringLiteral("请先添加至少一个待修复文件。"));
        return;
    }
    if (outputDir_.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("文件修复"), QStringLiteral("请选择输出目录。"));
        return;
    }

    logList_->clear();
    lastJobs_.clear();
    exportBtn_->setEnabled(false);
    setRunningUi(true);
    controller_->startRepair(inputs, outputDir_);
}

void FileRepairPage::onCancelRepair()
{
    controller_->cancelRepair();
    statusLabel_->setText(QStringLiteral("正在取消…"));
}

void FileRepairPage::onExportReport()
{
    if (lastJobs_.isEmpty()) {
        return;
    }
    const QString defaultName = QStringLiteral("repair_report_%1.csv")
                                    .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出修复报告"), defaultName, QStringLiteral("CSV (*.csv);;文本 (*.txt)"));
    if (path.isEmpty()) {
        return;
    }

    bool ok = false;
    if (path.endsWith(QStringLiteral(".txt"), Qt::CaseInsensitive)) {
        QFile f(path);
        ok = f.open(QIODevice::WriteOnly | QIODevice::Truncate);
        if (ok) {
            f.write(RepairReport::exportToText(lastJobs_).toUtf8());
            f.close();
        }
    } else {
        ok = RepairReport::exportToCsv(lastJobs_, path);
    }

    if (ok) {
        QMessageBox::information(this, QStringLiteral("导出成功"), QStringLiteral("报告已保存至：\n%1").arg(path));
    } else {
        QMessageBox::warning(this, QStringLiteral("导出失败"), QStringLiteral("无法写入报告文件。"));
    }
}

void FileRepairPage::onPreviewRepaired()
{
    QString path;
    for (const auto& job : lastJobs_) {
        if (job.result.success && QFile::exists(job.outputPath)) {
            path = job.outputPath;
            break;
        }
    }
    if (path.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("预览"), QStringLiteral("没有可预览的修复输出文件。"));
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
        QMessageBox::warning(this, QStringLiteral("预览"), QStringLiteral("无法用系统默认程序打开：\n%1").arg(path));
    }
}

void FileRepairPage::onBatchFinished(const QVector<RepairJobRecord> &jobs)
{
    lastJobs_ = jobs;
    setRunningUi(false);
    exportBtn_->setEnabled(!jobs.isEmpty());
    bool hasOutput = false;
    for (const auto& job : jobs) {
        if (job.result.success && QFile::exists(job.outputPath)) {
            hasOutput = true;
            break;
        }
    }
    previewBtn_->setEnabled(hasOutput);

    const int okCount = RepairReport::successCount(jobs);
    statusLabel_->setText(QStringLiteral("完成：成功 %1 / 共 %2").arg(okCount).arg(jobs.size()));
    progressBar_->setValue(progressBar_->maximum());
}

void FileRepairPage::setRunningUi(bool running)
{
    startBtn_->setEnabled(!running);
    cancelBtn_->setEnabled(running);
    fileList_->setEnabled(!running);
}

void FileRepairPage::appendLogLine(const QString &line)
{
    logList_->addItem(line);
    logList_->scrollToBottom();
}
