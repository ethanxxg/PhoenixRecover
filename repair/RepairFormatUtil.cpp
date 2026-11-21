#include "RepairFormatUtil.h"

#include <QtCore/QStringList>

namespace RepairFormatUtil {

static const QStringList& allExtensions()
{
    static const QStringList exts = {
        QStringLiteral("zip"), QStringLiteral("jar"), QStringLiteral("apk"),
        QStringLiteral("docx"), QStringLiteral("pptx"), QStringLiteral("xlsx"),
        QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("jfif"), QStringLiteral("jpe"),
        QStringLiteral("png"), QStringLiteral("gif"),
        QStringLiteral("webp"), QStringLiteral("heic"), QStringLiteral("heif"),
        QStringLiteral("tif"), QStringLiteral("tiff"), QStringLiteral("bmp"), QStringLiteral("dib"),
        QStringLiteral("cr2"), QStringLiteral("nef"), QStringLiteral("arw"), QStringLiteral("orf"),
        QStringLiteral("rw2"), QStringLiteral("dng"), QStringLiteral("raw"), QStringLiteral("raf"),
        QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("m4v"), QStringLiteral("m4a"),
        QStringLiteral("avi"), QStringLiteral("mkv"), QStringLiteral("webm"), QStringLiteral("flv"),
        QStringLiteral("wmv"), QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("m2ts"),
        QStringLiteral("ts"), QStringLiteral("3gp"), QStringLiteral("3g2"), QStringLiteral("asf"),
        QStringLiteral("ogv"), QStringLiteral("divx"),
    };
    return exts;
}

bool isRepairableExtension(const QString& ext)
{
    return allExtensions().contains(ext.trimmed().toLower());
}

QStringList repairableExtensionFilter()
{
    return allExtensions();
}

} // namespace RepairFormatUtil
