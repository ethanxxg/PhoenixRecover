#pragma once

#include "RepairTypes.h"
#include <QStringList>
#include <QByteArray>

class IFileRepair
{
public:
    virtual ~IFileRepair() = default;

    virtual QString formatName() const = 0;
    virtual QStringList supportedExtensions() const = 0;
    virtual bool canRepair(const QByteArray& header, qint64 fileSize) const = 0;
    virtual RepairResult repair(const QString& inputPath,
                                const QString& outputPath,
                                std::atomic<bool>* cancel = nullptr) = 0;
};
