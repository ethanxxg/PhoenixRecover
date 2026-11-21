#pragma once

#include "IFileRepair.h"
#include <QFile>
#include <vector>

class ZipRepair : public IFileRepair
{
public:
    QString formatName() const override;
    QStringList supportedExtensions() const override;
    bool canRepair(const QByteArray& header, qint64 fileSize) const override;
    RepairResult repair(const QString& inputPath,
                        const QString& outputPath,
                        std::atomic<bool>* cancel = nullptr) override;

private:
    struct LocalFileEntry
    {
        qint64   localHeaderOffset = 0;
        uint16_t versionNeeded     = 20;
        uint16_t flags             = 0;
        uint16_t method            = 0;
        uint16_t modTime           = 0;
        uint16_t modDate           = 0;
        uint32_t crc32             = 0;
        uint32_t compressedSize    = 0;
        uint32_t uncompressedSize  = 0;
        QByteArray fileName;
        QByteArray extraField;
        qint64   dataOffset        = 0;
        qint64   dataEndOffset     = 0;
    };

    std::vector<LocalFileEntry> scanLocalHeaders(QFile& input);
    bool rebuildCentralDirectory(QFile& input, QFile& output,
                                 const std::vector<LocalFileEntry>& entries,
                                 std::atomic<bool>* cancel);
};
