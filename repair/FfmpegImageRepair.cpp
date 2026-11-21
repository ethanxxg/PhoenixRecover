#include "FfmpegImageRepair.h"
#include "RepairFfmpeg.h"

#include <QFileInfo>

QString FfmpegImageRepair::formatName() const
{
    return QStringLiteral("Image (FFmpeg)");
}

QStringList FfmpegImageRepair::supportedExtensions() const
{
    return {
        QStringLiteral("webp"), QStringLiteral("heic"), QStringLiteral("heif"),
        QStringLiteral("tif"),  QStringLiteral("tiff"), QStringLiteral("bmp"),
        QStringLiteral("dib"),
    };
}

bool FfmpegImageRepair::canRepair(const QByteArray& header, qint64 fileSize) const
{
    if (fileSize < 8) {
        return false;
    }
    if (header.startsWith("RIFF") && header.size() >= 12 && header.mid(8, 4) == "WEBP") {
        return true;
    }
    if (header.size() >= 12) {
        const QByteArray ftyp = header.mid(4, 4);
        if (ftyp == "heic" || ftyp == "heix" || ftyp == "mif1" || ftyp == "msf1") {
            return true;
        }
    }
    if (header.startsWith("BM")) {
        return true;
    }
    if (header.startsWith("II*\0") || header.startsWith("MM\0*")) {
        return true;
    }
    return false;
}

RepairResult FfmpegImageRepair::repair(const QString& inputPath,
                                        const QString& outputPath,
                                        std::atomic<bool>* cancel)
{
    RepairResult result;
    result.originalSize = QFileInfo(inputPath).size();

    if (cancel && cancel->load()) {
        result.message = QStringLiteral("修复已取消");
        return result;
    }

    if (RepairFfmpeg::executablePath().isEmpty()) {
        result.message = QStringLiteral("未找到 FFmpeg，无法修复该图片格式");
        return result;
    }

    if (RepairFfmpeg::transcodeImage(inputPath, outputPath)) {
        result.success = true;
        result.repairedSize = QFileInfo(outputPath).size();
        result.message = QStringLiteral("FFmpeg 重新导出图片成功");
        return result;
    }

    result.message = QStringLiteral("FFmpeg 无法修复该图片");
    return result;
}
