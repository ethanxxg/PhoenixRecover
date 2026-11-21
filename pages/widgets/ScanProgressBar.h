#pragma once

#include <QtWidgets/QWidget>

class QLabel;
class QProgressBar;

class ScanProgressBar : public QWidget
{
    Q_OBJECT

public:
    explicit ScanProgressBar(QWidget *parent = nullptr);

    void setProgress(int percent, quint64 scannedBytes, quint64 totalBytes,
                     quint64 elapsedMs, const QString &phaseText);
    void reset();

private:
    QProgressBar *progressBar_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QLabel *metricFoundLabel_ = nullptr;
    QLabel *metricScannedLabel_ = nullptr;
    QLabel *metricTotalLabel_ = nullptr;
    QLabel *metricElapsedLabel_ = nullptr;
    QLabel *metricSpeedLabel_ = nullptr;

    void setMetricLabel(QLabel *label, const QString &value, const QString &caption);
    static QString formatElapsedCompact(qint64 elapsedMs);
};
