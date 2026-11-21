#include "SqliteRepair.h"

#include "SqliteRecoverService.h"

#include <QFileInfo>

QString SqliteRepair::formatName() const
{
    return QStringLiteral("SQLite");
}

QStringList SqliteRepair::supportedExtensions() const
{
    return {
        QStringLiteral("sqlite"),
        QStringLiteral("sqlite3"),
        QStringLiteral("db"),
    };
}

bool SqliteRepair::canRepair(const QByteArray& header, qint64 fileSize) const
{
    Q_UNUSED(fileSize);
    return SqliteRecoverService::IsSqliteMagic(
        reinterpret_cast<const uint8_t*>(header.constData()),
        static_cast<size_t>(header.size()));
}

RepairResult SqliteRepair::repair(const QString& inputPath,
                                  const QString& outputPath,
                                  std::atomic<bool>* cancel)
{
    Q_UNUSED(cancel);

    RepairResult result;
    QFileInfo inFi(inputPath);
    result.originalSize = inFi.size();

    const auto report = SqliteRecoverService::RecoverFile(
        inputPath.toStdWString(), outputPath.toStdWString());

    result.success = report.success;
    result.repairedSize = report.success ? QFileInfo(outputPath).size() : 0;
    if (report.success) {
        result.message = QStringLiteral("已重建 %1 页（页大小 %2 字节）")
                             .arg(report.pagesCopied)
                             .arg(report.pageSize);
        result.validated = true;
        result.validationDetail =
            QStringLiteral("页级校验通过；WAL/指针修复仍为后续能力");
    } else {
        result.message = QString::fromStdWString(report.message);
    }
    return result;
}
