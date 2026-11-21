#include "RecoveryScanController.h"
#include "RecoveryScanWorker.h"
#include "ScanResultStore.h"
#include "../log/Logger.h"

#include <QtCore/QVector>

RecoveryScanController::RecoveryScanController(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<ScanFileDto>("ScanFileDto");
    qRegisterMetaType<QVector<ScanFileDto>>("QVector<ScanFileDto>");
    qRegisterMetaType<quint64>("quint64");

    resultStore_ = new ScanResultStore;
    worker_ = new RecoveryScanWorker(resultStore_);
    worker_->moveToThread(&thread_);

    connect(worker_,
            &RecoveryScanWorker::indexUpdated,
            this,
            &RecoveryScanController::scanIndexUpdated,
            Qt::QueuedConnection);
    connect(worker_,
            &RecoveryScanWorker::progress,
            this,
            &RecoveryScanController::scanProgress,
            Qt::QueuedConnection);
    connect(worker_,
            &RecoveryScanWorker::finished,
            this,
            &RecoveryScanController::scanFinished,
            Qt::QueuedConnection);

    thread_.start();
}

RecoveryScanController::~RecoveryScanController()
{
    thread_.quit();
    thread_.wait(10'000);
    delete resultStore_;
}

ScanResultStore *RecoveryScanController::resultStore() const
{
    return resultStore_;
}

void RecoveryScanController::startVolumeScan(const QString &driveLetter)
{
    if (qEnvironmentVariableIsSet("PHOENIX_TEST_MODE") ||
        !qgetenv("PHOENIX_MOCK_SCAN_COUNT").isEmpty()) {
        LOG_INFO(L"RecoveryScanController::startVolumeScan skipped in test mode");
        return;
    }
    if (!worker_) {
        LOG_WARN(L"RecoveryScanController::startVolumeScan skipped because worker_ is null");
        return;
    }
    LOG_INFO(L"RecoveryScanController::startVolumeScan invoke worker"
             L" drive=" +
             std::wstring(reinterpret_cast<const wchar_t *>(driveLetter.utf16()),
                          static_cast<size_t>(driveLetter.size())));
    QMetaObject::invokeMethod(
        worker_,
        "runVolumeScan",
        Qt::QueuedConnection,
        Q_ARG(QString, driveLetter));
}

void RecoveryScanController::startImageScan(const QString &imagePath)
{
    if (qEnvironmentVariableIsSet("PHOENIX_TEST_MODE") ||
        !qgetenv("PHOENIX_MOCK_SCAN_COUNT").isEmpty()) {
        LOG_INFO(L"RecoveryScanController::startImageScan skipped in test mode");
        return;
    }
    if (!worker_) {
        LOG_WARN(L"RecoveryScanController::startImageScan skipped because worker_ is null");
        return;
    }
    QMetaObject::invokeMethod(
        worker_, "runImageScan", Qt::QueuedConnection,
        Q_ARG(QString, imagePath));
}

void RecoveryScanController::startPartitionScan(int physicalDiskIdx,
                                                 quint64 startSector,
                                                 quint64 totalSectors,
                                                 const QString &fsType)
{
    if (qEnvironmentVariableIsSet("PHOENIX_TEST_MODE") ||
        !qgetenv("PHOENIX_MOCK_SCAN_COUNT").isEmpty()) {
        LOG_INFO(L"RecoveryScanController::startPartitionScan skipped in test mode");
        return;
    }
    if (!worker_) {
        LOG_WARN(L"RecoveryScanController::startPartitionScan skipped because worker_ is null");
        return;
    }
    QMetaObject::invokeMethod(
        worker_, "runPartitionScan", Qt::QueuedConnection,
        Q_ARG(int, physicalDiskIdx),
        Q_ARG(quint64, startSector),
        Q_ARG(quint64, totalSectors),
        Q_ARG(QString, fsType));
}

void RecoveryScanController::stopScan()
{
    if (!worker_) return;
    // The worker thread is occupied by the scan loop, so a queued slot call
    // cannot run until the scan has already returned. requestStopScan only
    // performs an atomic store and is therefore safe to call directly from
    // the controller thread; this makes cancellation responsive.
    worker_->requestStopScan();
}
