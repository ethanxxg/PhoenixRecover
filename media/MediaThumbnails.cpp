#include "MediaThumbnails.h"

#include "FfmpegVideoThumb.h"
#include "../common/Platform.h"

#include <QtGui/QImage>
#include <QtGui/QPixmap>

#if PHOENIX_OS_WINDOWS
#include "WinShellVideoThumb.h"
#include "WinWicImageDecoder.h"
#endif

namespace {

static const QSize kGridThumb(180, 110);

static QPixmap scaleToGrid(const QImage &im)
{
    if (im.isNull()) {
        return {};
    }
    return QPixmap::fromImage(
        im.scaled(kGridThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

static QPixmap scaleToGrid(const QPixmap &pm)
{
    if (pm.isNull()) {
        return {};
    }
    return pm.scaled(kGridThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

} // namespace

namespace MediaThumbnails {

QPixmap videoPreviewPixmapForLocalFile(const QString &localFilePath)
{
    if (localFilePath.isEmpty()) {
        return {};
    }
    QImage frame;
    if (FfmpegVideoThumb::extractFirstFrame(localFilePath, &frame) && !frame.isNull()) {
        return scaleToGrid(frame);
    }
#if PHOENIX_OS_WINDOWS
    const QPixmap shellPm = WinShellVideoThumb::thumbnailForFile(localFilePath);
    if (!shellPm.isNull()) {
        return scaleToGrid(shellPm);
    }
#endif
    return {};
}

QPixmap imagePreviewPixmapForLocalFile(const QString &localFilePath)
{
    if (localFilePath.isEmpty()) {
        return {};
    }
    QImage img;
    if (img.load(localFilePath) && !img.isNull()) {
        return scaleToGrid(img);
    }
#if PHOENIX_OS_WINDOWS
    if (WinWicImageDecoder::loadFileToImage(localFilePath, &img) && !img.isNull()) {
        return scaleToGrid(img);
    }
#endif
    if (FfmpegVideoThumb::decodeRasterViaFfmpeg(localFilePath, &img) && !img.isNull()) {
        return scaleToGrid(img);
    }
#if PHOENIX_OS_WINDOWS
    const QPixmap sh = WinShellVideoThumb::thumbnailForFile(localFilePath);
    if (!sh.isNull()) {
        return scaleToGrid(sh);
    }
#endif
    return {};
}

QPixmap documentPreviewPixmapForLocalFile(const QString &localFilePath)
{
    if (localFilePath.isEmpty()) {
        return {};
    }
#if PHOENIX_OS_WINDOWS
    const QPixmap shellPm = WinShellVideoThumb::thumbnailForFile(localFilePath);
    if (!shellPm.isNull()) {
        return scaleToGrid(shellPm);
    }
#endif
    return {};
}

} // namespace MediaThumbnails
