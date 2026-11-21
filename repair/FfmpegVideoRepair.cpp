#include "FfmpegVideoRepair.h"
#include "RepairFfmpeg.h"

#include <QFile>
#include <QFileInfo>

QString FfmpegVideoRepair::formatName() const
{
    return QStringLiteral("Video (FFmpeg)");
}

QStringList FfmpegVideoRepair::supportedExtensions() const
{
    return {
        QStringLiteral("avi"),  QStringLiteral("mkv"),  QStringLiteral("webm"),
        QStringLiteral("flv"),  QStringLiteral("wmv"),  QStringLiteral("mpg"),
        QStringLiteral("mpeg"), QStringLiteral("m2ts"), QStringLiteral("ts"),
        QStringLiteral("3gp"),  QStringLiteral("3g2"),  QStringLiteral("asf"),
        QStringLiteral("ogv"),  QStringLiteral("divx"),
    };
}

bool FfmpegVideoRepair::matchesVideoMagic(const QByteArray& header)
{
    if (header.size() < 4) {
        return false;
    }
    if (header.startsWith("RIFF") && header.size() >= 12 && header.mid(8, 4) == "AVI ") {
        return true;
    }
    if (header.startsWith("FLV")) {
        return true;
    }
    const auto b0 = static_cast<uint8_t>(header[0]);
    const auto b1 = static_cast<uint8_t>(header[1]);
    const auto b2 = static_cast<uint8_t>(header[2]);
    const auto b3 = static_cast<uint8_t>(header[3]);
    if (b0 == 0x1A && b1 == 0x45 && b2 == 0xDF && b3 == 0xA3) { // Matroska / WebM
        return true;
    }
    if (b0 == 0x30 && b1 == 0x26 && b2 == 0xB2 && b3 == 0x75) { // ASF/WMV
        return true;
    }
    if (header.startsWith("\0\0\1\xBA") || header.startsWith("\0\0\1\xB3")) { // MPEG-PS
        return true;
    }
    if (header.size() >= 8 && header.mid(4, 4) == "ftyp") { // handled by Mp4Repair
        return false;
    }
    return false;
}

bool FfmpegVideoRepair::canRepair(const QByteArray& header, qint64 fileSize) const
{
    if (fileSize < 16) {
        return false;
    }
    return matchesVideoMagic(header);
}

RepairResult FfmpegVideoRepair::repair(const QString& inputPath,
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
        result.message = QStringLiteral("未找到 FFmpeg，无法修复该视频容器");
        return result;
    }

    const QString ext = QFileInfo(outputPath).suffix().toLower();
    if (RepairFfmpeg::remuxVideo(inputPath, outputPath, ext)) {
        result.success = true;
        result.repairedSize = QFileInfo(outputPath).size();
        result.message = QStringLiteral("FFmpeg 重封装修复成功");
        return result;
    }

    result.message = QStringLiteral("FFmpeg 无法重建该视频容器");
    return result;
}
