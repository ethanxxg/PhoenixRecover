#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <atomic>

#include "ScanTypes.h"

class ScanResultStore;

// 在独立线程中执行统一卷扫描，仅由 RecoveryScanController 持有与 invoke。
class RecoveryScanWorker : public QObject
{
    Q_OBJECT

public:
    explicit RecoveryScanWorker(ScanResultStore *resultStore, QObject *parent = nullptr);

public slots:
    void runVolumeScan(QString driveLetter);
    void runImageScan(QString imagePath);
    void runPartitionScan(int physicalDiskIdx,
                          quint64 startSector,
                          quint64 totalSectors,
                          QString fsType);
    void requestStopScan();

signals:
    void indexUpdated(quint64 revision);
    void progress(int percent, quint32 foundCount, quint64 scannedBytes, quint64 totalBytes, quint64 elapsedMs, QString phaseText);
    void finished(bool ok, QString message);

private:
    std::atomic<bool> cancelFlag_{false};
    ScanResultStore *resultStore_ = nullptr;
    QString pendingImagePath_;
    int pendingPhysicalDiskIdx_ = -1;
    quint64 pendingPartitionStartSector_ = 0;
    quint64 pendingPartitionTotalSectors_ = 0;
    QString pendingPartitionFsType_;
};
