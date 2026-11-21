#include "GifRepair.h"

#include <QFile>

namespace {

int findGifHeader(const QByteArray& data)
{
    for (int i = 0; i + 6 <= data.size(); ++i) {
        const QByteArray tag = data.mid(i, 6);
        if (tag == "GIF87a" || tag == "GIF89a") {
            return i;
        }
    }
    return -1;
}

} // namespace

QString GifRepair::formatName() const
{
    return QStringLiteral("GIF");
}

QStringList GifRepair::supportedExtensions() const
{
    return {QStringLiteral("gif")};
}

bool GifRepair::canRepair(const QByteArray& header, qint64 fileSize) const
{
    if (fileSize < 10) {
        return false;
    }
    return findGifHeader(header) >= 0;
}

RepairResult GifRepair::repair(const QString& inputPath,
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

    const int off = findGifHeader(data);
    if (off < 0) {
        result.message = QStringLiteral("未找到 GIF 文件头");
        return result;
    }
    if (off > 0) {
        data = data.mid(off);
    }

    bool appendedTrailer = false;
    if (data.isEmpty() || static_cast<uint8_t>(data.back()) != 0x3B) {
        data.append(char(0x3B));
        appendedTrailer = true;
    }

    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        result.message = QStringLiteral("无法创建输出文件");
        return result;
    }
    output.write(data);
    output.close();

    result.success = true;
    result.repairedSize = data.size();
    result.message = appendedTrailer ? QStringLiteral("已对齐 GIF 头并补全结束符")
                                     : QStringLiteral("已对齐 GIF 文件头");
    return result;
}
