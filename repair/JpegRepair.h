#pragma once

#include "IFileRepair.h"

class JpegRepair : public IFileRepair
{
public:
    QString formatName() const override;
    QStringList supportedExtensions() const override;
    bool canRepair(const QByteArray& header, qint64 fileSize) const override;
    RepairResult repair(const QString& inputPath,
                        const QString& outputPath,
                        std::atomic<bool>* cancel = nullptr) override;

private:
    static int findSoiOffset(const QByteArray& data);
    static bool endsWithEoi(const QByteArray& data);
};
