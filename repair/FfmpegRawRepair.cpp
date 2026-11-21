#include "FfmpegRawRepair.h"
#include "RepairFfmpeg.h"

#include <QFileInfo>

QString FfmpegRawRepair::formatName() const
{
    return QStringLiteral("RAW (FFmpeg)");
}

QStringList FfmpegRawRepair::supportedExtensions() const
{
    return {
        QStringLiteral("cr2"), QStringLiteral("nef"), QStringLiteral("arw"),
        QStringLiteral("orf"), QStringLiteral("rw2"), QStringLiteral("dng"),
        QStringLiteral("raw"), QStringLiteral("raf"), QStringLiteral("pef"),
    };
}

bool FfmpegRawRepair::canRepair(const QByteArray& header, qint64 fileSize) const
{
    Q_UNUSED(header);
    return fileSize >= 1024;
}

RepairResult FfmpegRawRepair::repair(const QString& inputPath,
                                      const QString& outputPath,
                                      std::atomic<bool>* cancel)
{
    RepairResult result;
    result.originalSize = QFileInfo(inputPath).size();

    if (cancel && cancel->load()) {
        result.message = QStringLiteral("修复已取消");
        return result;
    }

    QString jpegOut = outputPath;
    const QString outExt = QFileInfo(jpegOut).suffix().toLower();
    if (outExt != QStringLiteral("jpg") && outExt != QStringLiteral("jpeg")) {
        jpegOut = QFileInfo(jpegOut).absolutePath() + QLatin1Char('/')
                  + QFileInfo(jpegOut).completeBaseName() + QStringLiteral("_preview.jpg");
    }

    if (!RepairFfmpeg::exportRawPreviewJpeg(inputPath, jpegOut)) {
        result.message = QStringLiteral("FFmpeg 无法从 RAW 导出 JPEG 预览（请确认已安装 FFmpeg）");
        return result;
    }

    result.success = true;
    result.repairedSize = QFileInfo(jpegOut).size();
    result.message = QStringLiteral("已导出 JPEG 预览: %1").arg(QFileInfo(jpegOut).fileName());
    return result;
}
