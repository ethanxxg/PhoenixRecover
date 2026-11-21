#include "JpegRepair.h"

#include <QFile>

namespace {

bool looksLikeJpegPayload(const QByteArray& header)
{
    for (int i = 0; i + 1 < header.size(); ++i) {
        const auto a = static_cast<uint8_t>(header[i]);
        const auto b = static_cast<uint8_t>(header[i + 1]);
        if (a == 0xFF && (b == 0xD8 || b == 0xE0 || b == 0xE1 || b == 0xDB || b == 0xC0 || b == 0xC4)) {
            return true;
        }
    }
    return false;
}

} // namespace

QString JpegRepair::formatName() const
{
    return QStringLiteral("JPEG");
}

QStringList JpegRepair::supportedExtensions() const
{
    return {QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("jfif"), QStringLiteral("jpe")};
}

int JpegRepair::findSoiOffset(const QByteArray& data)
{
    for (int i = 0; i + 1 < data.size(); ++i) {
        if (static_cast<uint8_t>(data[i]) == 0xFF && static_cast<uint8_t>(data[i + 1]) == 0xD8) {
            return i;
        }
    }
    return -1;
}

bool JpegRepair::endsWithEoi(const QByteArray& data)
{
    if (data.size() < 2) {
        return false;
    }
    const int n = data.size();
    return static_cast<uint8_t>(data[n - 2]) == 0xFF && static_cast<uint8_t>(data[n - 1]) == 0xD9;
}

bool JpegRepair::canRepair(const QByteArray& header, qint64 fileSize) const
{
    if (fileSize < 4) {
        return false;
    }
    if (header.size() >= 2 && static_cast<uint8_t>(header[0]) == 0xFF
        && static_cast<uint8_t>(header[1]) == 0xD8) {
        return true;
    }
    if (findSoiOffset(header) >= 0) {
        return true;
    }
    return looksLikeJpegPayload(header) && fileSize >= 64;
}

RepairResult JpegRepair::repair(const QString& inputPath,
                                const QString& outputPath,
                                std::atomic<bool>* cancel)
{
    RepairResult result;

    QFile input(inputPath);
    if (!input.open(QIODevice::ReadOnly)) {
        result.message = QStringLiteral("无法打开输入文件");
        return result;
    }
    result.originalSize = input.size();
    QByteArray data = input.readAll();
    input.close();

    if (cancel && cancel->load()) {
        result.message = QStringLiteral("修复已取消");
        return result;
    }

    int soi = findSoiOffset(data);
    if (soi < 0) {
        data.prepend(char(0xD8));
        data.prepend(char(0xFF));
        soi = 0;
    } else if (soi > 0) {
        data = data.mid(soi);
    }

    bool appendedEoi = false;
    if (!endsWithEoi(data)) {
        data.append(char(0xFF));
        data.append(char(0xD9));
        appendedEoi = true;
    }

    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result.message = QStringLiteral("无法创建输出文件");
        return result;
    }
    if (output.write(data) != data.size()) {
        result.message = QStringLiteral("写入输出文件失败");
        return result;
    }
    output.close();

    result.success = true;
    result.repairedSize = data.size();
    result.message = appendedEoi ? QStringLiteral("已对齐 SOI 并补全 EOI 标记")
                                 : QStringLiteral("已对齐 JPEG 起始标记");
    return result;
}
