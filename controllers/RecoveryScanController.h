#pragma once

#include <QtCore/QObject>
#include <QtCore/QThread>

#include "ScanTypes.h"

class RecoveryScanWorker;
class ScanResultStore;

// 在子线程启动卷扫描，通过信号将进度与文件批次送到 UI（QueuedConnection）。
class RecoveryScanController : public QObject
{
    Q_OBJECT

public:
    explicit RecoveryScanController(QObject *parent = nullptr);
    ~RecoveryScanController() override;
    ScanResultStore *resultStore() const;

public slots:
    void startVolumeScan(const QString &driveLetter);
    void startImageScan(const QString &imagePath);
    void startPartitionScan(int physicalDiskIdx,
                            quint64 startSector,
                            quint64 totalSectors,
                            const QString &fsType);
    void stopScan();

signals:
    void scanIndexUpdated(quint64 revision);
    void scanProgress(int percent, quint32 foundCount, quint64 scannedBytes, quint64 totalBytes, quint64 elapsedMs, QString phaseText);
    void scanFinished(bool ok, const QString &message);

private:
    QThread thread_;
    RecoveryScanWorker *worker_ = nullptr;
    ScanResultStore *resultStore_ = nullptr;
};
