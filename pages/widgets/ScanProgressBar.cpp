#include "ScanProgressBar.h"

#include <QtCore/QLocale>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QVBoxLayout>

ScanProgressBar::ScanProgressBar(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    statusLabel_ = new QLabel(QStringLiteral("扫描进行中"), this);
    statusLabel_->setStyleSheet(QStringLiteral(
        "font-size: 12px; color: #6b7280; padding: 7px 0 10px 0;"));
    layout->addWidget(statusLabel_);

    progressBar_ = new QProgressBar(this);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(false);
    progressBar_->setFixedHeight(10);
    progressBar_->setStyleSheet(QStringLiteral(
        "QProgressBar { border: none; background: #e7edf3; border-radius: 5px; padding: 0px; margin: 2px 0 0 0; }"
        "QProgressBar::chunk { background: #22c55e; border: none; border-radius: 5px; margin: 0px; }"));
    layout->addWidget(progressBar_);

    auto *metricWrap = new QWidget(this);
    metricWrap->setStyleSheet(QStringLiteral(
        "background: #ffffff; border-bottom: 1px solid #e5e7eb;"));
    auto *metricRow = new QHBoxLayout(metricWrap);
    metricRow->setContentsMargins(0, 11, 0, 11);
    metricRow->setSpacing(10);

    auto makeMetric = [this](const QString &value, const QString &caption, QLabel **outLabel) {
        auto *lb = new QLabel(this);
        lb->setTextFormat(Qt::RichText);
        lb->setStyleSheet(QStringLiteral("font-size: 12px;"));
        setMetricLabel(lb, value, caption);
        *outLabel = lb;
        return lb;
    };

    metricRow->addWidget(makeMetric(QStringLiteral("0"), QStringLiteral("已找到"), &metricFoundLabel_));
    metricRow->addWidget(new QLabel(QStringLiteral("|"), metricWrap));
    metricRow->addWidget(makeMetric(QStringLiteral("0 GB"), QStringLiteral("已扫描"), &metricScannedLabel_));
    metricRow->addWidget(new QLabel(QStringLiteral("|"), metricWrap));
    metricRow->addWidget(makeMetric(QStringLiteral("500 GB"), QStringLiteral("总计"), &metricTotalLabel_));
    metricRow->addWidget(new QLabel(QStringLiteral("|"), metricWrap));
    metricRow->addWidget(makeMetric(QStringLiteral("00:00"), QStringLiteral("用时"), &metricElapsedLabel_));
    metricRow->addWidget(new QLabel(QStringLiteral("|"), metricWrap));
    metricRow->addWidget(makeMetric(QStringLiteral("0 MB/s"), QStringLiteral("速度"), &metricSpeedLabel_));
    metricRow->addStretch(1);
    layout->addWidget(metricWrap);
}

void ScanProgressBar::setProgress(int percent, quint64 scannedBytes, quint64 totalBytes,
                                   quint64 elapsedMs, const QString &phaseText)
{
    if (progressBar_) {
        progressBar_->setValue(percent);
    }
    if (statusLabel_) {
        statusLabel_->setText(phaseText.isEmpty()
                                 ? QStringLiteral("扫描进行中")
                                 : QStringLiteral("扫描进行中 · %1").arg(phaseText));
    }

    const QString scannedStr = QLocale::system().formattedDataSize(
        static_cast<qint64>(scannedBytes), 1, QLocale::DataSizeIecFormat);
    const QString totalStr = QLocale::system().formattedDataSize(
        static_cast<qint64>(totalBytes), 1, QLocale::DataSizeIecFormat);
    const QString elapsed = formatElapsedCompact(static_cast<qint64>(elapsedMs));

    QString speedStr = QStringLiteral("0 MB/s");
    if (elapsedMs > 0) {
        const double mbPerSec = static_cast<double>(scannedBytes) / (1024.0 * 1024.0)
                                / (static_cast<double>(elapsedMs) / 1000.0);
        speedStr = QStringLiteral("%1 MB/s").arg(mbPerSec, 0, 'f', 1);
    }

    setMetricLabel(metricScannedLabel_, scannedStr, QStringLiteral("已扫描"));
    setMetricLabel(metricTotalLabel_, totalStr, QStringLiteral("总计"));
    setMetricLabel(metricElapsedLabel_, elapsed, QStringLiteral("用时"));
    setMetricLabel(metricSpeedLabel_, speedStr, QStringLiteral("速度"));
}

void ScanProgressBar::reset()
{
    if (progressBar_) {
        progressBar_->setValue(0);
    }
    if (statusLabel_) {
        statusLabel_->setText(QStringLiteral("扫描进行中"));
    }
    setMetricLabel(metricFoundLabel_, QStringLiteral("0"), QStringLiteral("已找到"));
    setMetricLabel(metricScannedLabel_, QStringLiteral("0 GB"), QStringLiteral("已扫描"));
    setMetricLabel(metricTotalLabel_, QStringLiteral("500 GB"), QStringLiteral("总计"));
    setMetricLabel(metricElapsedLabel_, QStringLiteral("00:00"), QStringLiteral("用时"));
    setMetricLabel(metricSpeedLabel_, QStringLiteral("0 MB/s"), QStringLiteral("速度"));
}

void ScanProgressBar::setMetricLabel(QLabel *label, const QString &value, const QString &caption)
{
    if (!label) {
        return;
    }
    label->setText(QStringLiteral("<b style='color:#111827;'>%1</b> <span style='color:#6b7280;'>%2</span>")
                       .arg(value, caption));
}

QString ScanProgressBar::formatElapsedCompact(qint64 elapsedMs)
{
    if (elapsedMs < 0) {
        elapsedMs = 0;
    }
    const qint64 totalSeconds = elapsedMs / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}
