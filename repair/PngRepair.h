#pragma once

#include "IFileRepair.h"

class PngRepair : public IFileRepair
{
public:
    QString formatName() const override;
    QStringList supportedExtensions() const override;
    bool canRepair(const QByteArray& header, qint64 fileSize) const override;
    RepairResult repair(const QString& inputPath,
                        const QString& outputPath,
                        std::atomic<bool>* cancel = nullptr) override;

private:
    static bool hasPngSignature(const QByteArray& data, int offset = 0);
    static bool hasIendChunk(const QByteArray& data);
};
