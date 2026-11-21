#include "GridPreviewExtensions.h"

#include <QtCore/QStringList>

#include <algorithm>
#include <limits>

namespace GridPreview {

static const QStringList& imageList()
{
    static const QStringList k = {
        QStringLiteral("jpg"),  QStringLiteral("jpeg"), QStringLiteral("jfif"), QStringLiteral("jpe"),
        QStringLiteral("png"),  QStringLiteral("gif"),    QStringLiteral("bmp"),  QStringLiteral("dib"),
        QStringLiteral("webp"), QStringLiteral("ico"),    QStringLiteral("tif"),  QStringLiteral("tiff"),
        QStringLiteral("heic"), QStringLiteral("heif"),   QStringLiteral("avif"), QStringLiteral("jp2"),
        QStringLiteral("j2c"),  QStringLiteral("j2k"),    QStringLiteral("ppm"),  QStringLiteral("pgm"),
        QStringLiteral("pbm"),  QStringLiteral("pam"),    QStringLiteral("xpm"),  QStringLiteral("tga"),
        QStringLiteral("cr2"),  QStringLiteral("nef"),    QStringLiteral("arw"),  QStringLiteral("dng"),
        QStringLiteral("orf"),  QStringLiteral("rw2"),    QStringLiteral("pef"),  QStringLiteral("srw"),
        QStringLiteral("raf"),  QStringLiteral("3fr"),    QStringLiteral("mrw"),  QStringLiteral("raw"),
        QStringLiteral("psd"),  QStringLiteral("exr"),    QStringLiteral("hdr"),  QStringLiteral("svg")};
    return k;
}

static const QStringList& videoList()
{
    static const QStringList k = {
        QStringLiteral("mp4"), QStringLiteral("m4v"), QStringLiteral("mov"), QStringLiteral("avi"),
        QStringLiteral("mkv"), QStringLiteral("wmv"), QStringLiteral("webm"), QStringLiteral("flv"),
        QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("ts"), QStringLiteral("m2ts"),
        QStringLiteral("m2t"), QStringLiteral("3gp"), QStringLiteral("3g2")};
    return k;
}

static const QStringList& documentList()
{
    // Office Open XML + legacy Office + common “recover decision” types (Windows Shell 缩略图).
    static const QStringList k = {
        QStringLiteral("pdf"),
        QStringLiteral("docx"), QStringLiteral("pptx"), QStringLiteral("xlsx"),
        QStringLiteral("doc"),  QStringLiteral("ppt"),  QStringLiteral("xls"),
        QStringLiteral("zip"),
        QStringLiteral("rtf"),  QStringLiteral("txt"),  QStringLiteral("md"),
        QStringLiteral("odt"),  QStringLiteral("ods"),  QStringLiteral("odp"),
        QStringLiteral("epub"), QStringLiteral("html"), QStringLiteral("htm"),
    };
    return k;
}

bool isImageExt(const QString& lower)
{
    return imageList().contains(lower);
}

bool isVideoExt(const QString& lower)
{
    return videoList().contains(lower);
}

bool isDocumentExt(const QString& lower)
{
    return documentList().contains(lower);
}

bool isThumbnailExt(const QString& lower)
{
    return isImageExt(lower) || isVideoExt(lower) || isDocumentExt(lower);
}

quint32 previewByteCapForExtension(const QString& extLower)
{
    if (isVideoExt(extLower)) {
        return 64u * 1024u * 1024u;
    }
    if (isImageExt(extLower)) {
        return 16u * 1024u * 1024u;
    }
    if (isDocumentExt(extLower)) {
        return 64u * 1024u * 1024u;
    }
    return 256u * 1024u;
}

quint32 effectivePreviewReadCap(const QString& extLower, qint64 logicalSizeBytes)
{
    const quint32 prefixCap = previewByteCapForExtension(extLower);
    if (logicalSizeBytes <= 0) {
        return prefixCap;
    }
    if (isVideoExt(extLower)) {
        constexpr qint64 kWholeFileIfUnder = 128LL * 1024 * 1024;
        if (logicalSizeBytes <= kWholeFileIfUnder) {
            return static_cast<quint32>(
                std::min<qint64>(logicalSizeBytes, qint64(std::numeric_limits<quint32>::max())));
        }
        return prefixCap;
    }
    if (isImageExt(extLower)) {
        constexpr qint64 kWholeImageIfUnder = 64LL * 1024 * 1024;
        if (logicalSizeBytes <= kWholeImageIfUnder) {
            return static_cast<quint32>(
                std::min<qint64>(logicalSizeBytes, qint64(std::numeric_limits<quint32>::max())));
        }
        return prefixCap;
    }
    if (isDocumentExt(extLower)) {
        // PDF / Office 容器：尾部常有 xref/central directory；小文件整读更稳。
        constexpr qint64 kWholeIfUnder = 128LL * 1024 * 1024;
        if (logicalSizeBytes <= kWholeIfUnder) {
            return static_cast<quint32>(
                std::min<qint64>(logicalSizeBytes, qint64(std::numeric_limits<quint32>::max())));
        }
        return prefixCap;
    }
    return prefixCap;
}

} // namespace GridPreview
