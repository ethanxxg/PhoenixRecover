#include "FileRepairWorker.h"
#include "../repair/RepairManager.h"
#include "../log/Logger.h"

#include <QDir>
#include <QFileInfo>

FileRepairWorker::FileRepairWorker(QObject *parent)
    : QObject(parent)
    , manager_(new RepairManager())
{
}

FileRepairWorker::~FileRepairWorker()
{
    delete manager_;
}

void FileRepairWorker::requestCancel()
{
    cancelFlag_.store(true);
}

void FileRepairWorker::runBatchRepair(QStringList inputPaths, QString outputDir)
{
    cancelFlag_.store(false);
    QVector<RepairJobRecord> jobs;
    jobs.reserve(inputPaths.size());

    QDir outDir(outputDir);
    if (!outDir.exists() && !outDir.mkpath(QStringLiteral("."))) {
        RepairJobRecord err;
        err.inputPath = QStringLiteral("(batch)");
        err.result.success = false;
        err.result.message = QStringLiteral("无法创建输出目录");
        jobs.append(err);
        emit allFinished(jobs);
        return;
    }

    const int total = inputPaths.size();
    int index = 0;
    for (const QString &inputPath : inputPaths) {
        if (cancelFlag_.load()) {
            break;
        }

        ++index;
        emit progress(index, total, QFileInfo(inputPath).fileName());

        RepairJobRecord record;
        record.inputPath = inputPath;

        auto *repairer = manager_->findRepairer(inputPath);
        if (!repairer) {
            record.result.success = false;
            record.result.message = QStringLiteral("不支持的文件格式或文件头无法识别");
            jobs.append(record);
            emit jobFinished(record);
            continue;
        }
        record.formatName = repairer->formatName();

        const QString baseName = QFileInfo(inputPath).completeBaseName();
        const QString ext = QFileInfo(inputPath).suffix();
        const QString outName = QStringLiteral("%1_repaired.%2").arg(baseName, ext);
        record.outputPath = outDir.filePath(outName);

        record.result = manager_->repairFile(inputPath, record.outputPath, &cancelFlag_);
        jobs.append(record);
        emit jobFinished(record);
    }

    emit allFinished(jobs);
}
