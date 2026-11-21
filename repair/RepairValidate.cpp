#include "RepairValidate.h"
#include "ZipEntryList.h"

#include <QFile>
#include <QFileInfo>
namespace {

uint32_t readLE32(const char* p)
{
    const auto* u = reinterpret_cast<const uint8_t*>(p);
    return static_cast<uint32_t>(u[0] | (u[1] << 8) | (u[2] << 16) | (u[3] << 24));
}

bool zipEntryExists(const QStringList& names, const QString& entry)
{
    const QString norm = entry.toLower();
    for (const QString& n : names) {
        if (n.compare(entry, Qt::CaseInsensitive) == 0) {
            return true;
        }
        QString normalized = n;
        normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (normalized.toLower() == norm) {
            return true;
        }
    }
    return false;
}

QStringList requiredOfficeEntries(const QString& extLower)
{
    QStringList req;
    req << QStringLiteral("[Content_Types].xml") << QStringLiteral("_rels/.rels");
    if (extLower == QStringLiteral("docx")) {
        req << QStringLiteral("word/document.xml");
    } else if (extLower == QStringLiteral("pptx")) {
        req << QStringLiteral("ppt/presentation.xml");
    } else if (extLower == QStringLiteral("xlsx")) {
        req << QStringLiteral("xl/workbook.xml");
    }
    return req;
}

} // namespace

RepairValidationOutcome validateZipContainer(const QString& path)
{
    RepairValidationOutcome out;
    const QStringList names = listZipLocalEntryNamesFromPath(path);
    if (names.isEmpty()) {
        out.detail = QStringLiteral("ZIP 内无有效本地文件头");
        return out;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        out.detail = QStringLiteral("无法打开输出文件");
        return out;
    }
    const QByteArray head = f.read(4);
    f.close();
    if (head.size() < 4 || readLE32(head.constData()) != 0x04034b50) {
        out.detail = QStringLiteral("缺少 PK\\x03\\x04 头");
        return out;
    }

    out.passed = true;
    out.detail = QStringLiteral("ZIP 结构校验通过（%1 个条目）").arg(names.size());
    return out;
}

RepairValidationOutcome validateOfficeOpenXml(const QString& path, const QString& extension)
{
    RepairValidationOutcome out;
    const QString ext = extension.toLower();

    auto zipCheck = validateZipContainer(path);
    if (!zipCheck.passed) {
        out.detail = zipCheck.detail;
        return out;
    }

    const QStringList names = listZipLocalEntryNamesFromPath(path);
    QStringList missing;
    for (const QString& req : requiredOfficeEntries(ext)) {
        if (!zipEntryExists(names, req)) {
            missing.append(req);
        }
    }

    if (!missing.isEmpty()) {
        out.detail = QStringLiteral("缺少 Office 核心部件: %1").arg(missing.join(QStringLiteral(", ")));
        return out;
    }

    out.passed = true;
    out.detail = QStringLiteral("Office Open XML 核心部件齐全");
    return out;
}

RepairValidationOutcome validateJpegFile(const QString& path)
{
    RepairValidationOutcome out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        out.detail = QStringLiteral("无法读取 JPEG");
        return out;
    }
    const QByteArray data = f.readAll();
    f.close();
    if (data.size() < 4) {
        out.detail = QStringLiteral("文件过短");
        return out;
    }
    const bool soi = static_cast<uint8_t>(data[0]) == 0xFF && static_cast<uint8_t>(data[1]) == 0xD8;
    const bool eoi = static_cast<uint8_t>(data[data.size() - 2]) == 0xFF
                     && static_cast<uint8_t>(data[data.size() - 1]) == 0xD9;
    if (!soi || !eoi) {
        out.detail = QStringLiteral("缺少 SOI/EOI 标记");
        return out;
    }
    out.passed = true;
    out.detail = QStringLiteral("JPEG SOI/EOI 标记完整");
    return out;
}

RepairValidationOutcome validatePngFile(const QString& path)
{
    RepairValidationOutcome out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        out.detail = QStringLiteral("无法读取 PNG");
        return out;
    }
    const QByteArray data = f.readAll();
    f.close();
    static const char kSig[] = {static_cast<char>(0x89), 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (data.size() < 12 || !data.startsWith(QByteArray(kSig, 8))) {
        out.detail = QStringLiteral("PNG 签名无效");
        return out;
    }
    if (!data.contains("IEND")) {
        out.detail = QStringLiteral("缺少 IEND chunk");
        return out;
    }
    out.passed = true;
    out.detail = QStringLiteral("PNG 签名与 IEND 存在");
    return out;
}

void applyPostRepairValidation(RepairResult& result,
                               const QString& outputPath,
                               const QString& formatName,
                               const QString& inputPath)
{
    if (!result.success) {
        result.validated = false;
        return;
    }

    Q_UNUSED(inputPath);

    RepairValidationOutcome v;
    const QString fmt = formatName.toUpper();
    const QString ext = QFileInfo(outputPath).suffix().toLower();

    if (fmt.contains(QStringLiteral("OFFICE")) || ext == QStringLiteral("docx")
        || ext == QStringLiteral("pptx") || ext == QStringLiteral("xlsx")) {
        v = validateOfficeOpenXml(outputPath, ext);
    } else if (fmt == QStringLiteral("ZIP")) {
        v = validateZipContainer(outputPath);
    } else if (fmt == QStringLiteral("JPEG")) {
        v = validateJpegFile(outputPath);
    } else if (fmt == QStringLiteral("PNG")) {
        v = validatePngFile(outputPath);
    } else if (fmt.contains(QStringLiteral("RAW"))) {
        const QString jpg = QFileInfo(outputPath).absolutePath() + QLatin1Char('/')
                            + QFileInfo(outputPath).completeBaseName() + QStringLiteral("_preview.jpg");
        if (QFile::exists(jpg)) {
            v = validateJpegFile(jpg);
        } else {
            v.passed = QFileInfo(outputPath).size() > 0;
            v.detail = QStringLiteral("RAW 已处理（未生成 JPEG 侧车文件）");
        }
    } else if (fmt.contains(QStringLiteral("FFMPEG")) || fmt.contains(QStringLiteral("MP4"))
               || fmt.contains(QStringLiteral("VIDEO"))) {
        result.validated = true;
        result.validationDetail = QStringLiteral("容器修复完成（未做 FFmpeg 深度探测）");
        if (!result.message.contains(result.validationDetail)) {
            result.message += QStringLiteral("；") + result.validationDetail;
        }
        return;
    } else {
        result.validated = true;
        result.validationDetail = QStringLiteral("未配置专项校验");
        return;
    }

    result.validated = v.passed;
    result.validationDetail = v.detail;
    if (!v.detail.isEmpty()) {
        result.message += QStringLiteral("；校验: ") + v.detail;
    }
}
