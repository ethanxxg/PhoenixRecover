#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <atomic>

#include "../repair/RepairReport.h"

class RepairManager;

class FileRepairWorker : public QObject
{
    Q_OBJECT

public:
    explicit FileRepairWorker(QObject *parent = nullptr);
    ~FileRepairWorker() override;

public slots:
    void runBatchRepair(QStringList inputPaths, QString outputDir);
    void requestCancel();

signals:
    void progress(int current, int total, QString fileName);
    void jobFinished(RepairJobRecord record);
    void allFinished(QVector<RepairJobRecord> jobs);

private:
    std::atomic<bool> cancelFlag_{false};
    RepairManager *manager_ = nullptr;
};
