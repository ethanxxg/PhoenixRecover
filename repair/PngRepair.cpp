#include "PngRepair.h"

#include <QFile>

namespace {

constexpr char kPngSig[] = {static_cast<char>(0x89), 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
constexpr char kIendType[] = {'I', 'E', 'N', 'D'};
constexpr char kIendCrc[] = {static_cast<char>(0xAE), static_cast<char>(0x42),
                             static_cast<char>(0x60), static_cast<char>(0x82)};

bool pngSignatureAt(const QByteArray& data, int offset)
{
    if (offset + 8 > data.size()) {
        return false;
    }
    return data.mid(offset, 8) == QByteArray(kPngSig, 8);
}

int findPngOffset(const QByteArray& data)
{
    for (int i = 0; i + 8 <= data.size(); ++i) {
        if (pngSignatureAt(data, i)) {
            return i;
        }
    }
    return -1;
}

} // namespace

QString PngRepair::formatName() const
{
    return QStringLiteral("PNG");
}

QStringList PngRepair::supportedExtensions() const
{
    return {QStringLiteral("png")};
}

bool PngRepair::hasPngSignature(const QByteArray& data, int offset)
{
    return pngSignatureAt(data, offset);
}

bool PngRepair::hasIendChunk(const QByteArray& data)
{
    if (data.size() < 12) {
        return false;
    }
    for (int i = data.size() - 12; i >= 8; --i) {
        if (data.mid(i + 4, 4) == QByteArray(kIendType, 4)) {
            return true;
        }
        if (i < 8) {
            break;
        }
    }
    return data.size() >= 12 && data.mid(data.size() - 8, 4) == QByteArray(kIendType, 4);
}

bool PngRepair::canRepair(const QByteArray& header, qint64 fileSize) const
{
    if (fileSize < 16) {
        return false;
    }
    if (hasPngSignature(header)) {
        return true;
    }
    if (findPngOffset(header) >= 0) {
        return true;
    }
    // 部分签名损坏但仍含 IHDR 特征
    return header.contains("IHDR") && fileSize >= 33;
}

RepairResult PngRepair::repair(const QString& inputPath,
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

    int pngOff = findPngOffset(data);
    if (pngOff < 0) {
        if (data.size() >= 8 && data.contains("IHDR")) {
            QByteArray patched;
            patched.append(kPngSig, 8);
            patched.append(data);
            data = patched;
            pngOff = 0;
        } else {
            result.message = QStringLiteral("未识别为 PNG 结构");
            return result;
        }
    } else if (pngOff > 0) {
        data = data.mid(pngOff);
    }

    if (!hasPngSignature(data)) {
        data.replace(0, 8, QByteArray(kPngSig, 8));
    }

    bool appendedIend = false;
    if (!hasIendChunk(data)) {
        data.append(QByteArray::fromRawData("\0\0\0\0", 4));
        data.append(kIendType, 4);
        data.append(kIendCrc, 4);
        appendedIend = true;
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
    result.message = appendedIend ? QStringLiteral("已修正 PNG 签名并补全 IEND")
                                  : QStringLiteral("已修正 PNG 文件头");
    return result;
}
