#pragma once

#include "IFileRepair.h"
#include <QFile>
#include <vector>

class Mp4Repair : public IFileRepair
{
public:
    QString formatName() const override;
    QStringList supportedExtensions() const override;
    bool canRepair(const QByteArray& header, qint64 fileSize) const override;
    RepairResult repair(const QString& inputPath,
                        const QString& outputPath,
                        std::atomic<bool>* cancel = nullptr) override;

private:
    struct BoxInfo
    {
        QByteArray type;    // 4 bytes
        qint64     offset = 0;
        qint64     size   = 0;
    };

    std::vector<BoxInfo> parseTopLevelBoxes(QFile& input);
    bool relocateMoov(QFile& input, QFile& output, std::atomic<bool>* cancel);
    bool adjustChunkOffsets(QByteArray& moovData, qint64 offsetDelta);
    bool tryFfmpegRemux(const QString& inputPath, const QString& outputPath);
};
