#include "FileRepairController.h"
#include "FileRepairWorker.h"

#include <QtCore/QMetaObject>

FileRepairController::FileRepairController(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<RepairJobRecord>("RepairJobRecord");
    qRegisterMetaType<QVector<RepairJobRecord>>("QVector<RepairJobRecord>");

    worker_ = new FileRepairWorker();
    worker_->moveToThread(&thread_);

    connect(worker_, &FileRepairWorker::progress, this, &FileRepairController::progress, Qt::QueuedConnection);
    connect(worker_, &FileRepairWorker::jobFinished, this, &FileRepairController::jobFinished, Qt::QueuedConnection);
    connect(worker_, &FileRepairWorker::allFinished, this, [this](const QVector<RepairJobRecord> &jobs) {
        running_ = false;
        emit batchFinished(jobs);
    },
            Qt::QueuedConnection);

    thread_.start();
}

FileRepairController::~FileRepairController()
{
    thread_.quit();
    thread_.wait(10'000);
}

void FileRepairController::startRepair(const QStringList &inputPaths, const QString &outputDir)
{
    if (running_) {
        return;
    }
    running_ = true;
    QMetaObject::invokeMethod(worker_,
                              "runBatchRepair",
                              Qt::QueuedConnection,
                              Q_ARG(QStringList, inputPaths),
                              Q_ARG(QString, outputDir));
}

void FileRepairController::cancelRepair()
{
    if (!worker_) {
        return;
    }
    QMetaObject::invokeMethod(worker_, "requestCancel", Qt::QueuedConnection);
}
