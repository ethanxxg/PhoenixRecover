#pragma once

#include <QtCore/QObject>
#include <QtCore/QStringList>
#include <QtCore/QThread>
#include <QtCore/QVector>

#include "../repair/RepairReport.h"

class FileRepairWorker;

class FileRepairController : public QObject
{
    Q_OBJECT

public:
    explicit FileRepairController(QObject *parent = nullptr);
    ~FileRepairController() override;

    void startRepair(const QStringList &inputPaths, const QString &outputDir);
    void cancelRepair();
    bool isRunning() const { return running_; }

signals:
    void progress(int current, int total, QString fileName);
    void jobFinished(RepairJobRecord record);
    void batchFinished(QVector<RepairJobRecord> jobs);

private:
    QThread thread_;
    FileRepairWorker *worker_ = nullptr;
    bool running_ = false;
};
