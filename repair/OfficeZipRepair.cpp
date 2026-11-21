#include "OfficeZipRepair.h"
#include "ZipRepair.h"

QString OfficeZipRepair::formatName() const
{
    return QStringLiteral("Office Open XML");
}

QStringList OfficeZipRepair::supportedExtensions() const
{
    return {
        QStringLiteral("docx"),
        QStringLiteral("pptx"),
        QStringLiteral("xlsx"),
    };
}

bool OfficeZipRepair::canRepair(const QByteArray& header, qint64 fileSize) const
{
    if (fileSize < 22) {
        return false;
    }
    if (header.size() < 4) {
        return false;
    }
    const auto* h = reinterpret_cast<const uint8_t*>(header.constData());
    return h[0] == 0x50 && h[1] == 0x4B && h[2] == 0x03 && h[3] == 0x04;
}

RepairResult OfficeZipRepair::repair(const QString& inputPath,
                                      const QString& outputPath,
                                      std::atomic<bool>* cancel)
{
    RepairResult result = zipRepair_.repair(inputPath, outputPath, cancel);
    if (!result.success) {
        result.validated = false;
    }
    return result;
}
