#include <QtWidgets/QApplication>
#include <QtCore/QFileInfo>
#include <QtCore/QDir>
#include <QtCore/QString>
#include <QtGui/QPixmap>

#include "../media/MediaThumbnails.h"

static int skip(const char* why)
{
    fprintf(stdout, "SKIP: %s\n", why);
    return 0;
}

static bool exists(const QString& p)
{
    return QFileInfo::exists(p) && QFileInfo(p).isFile();
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    const QString root = QDir::currentPath();
    const QString jpg = QDir(root).filePath(QStringLiteral("testdata/media/xiph-poster_big.jpg"));
    const QString mp4 = QDir(root).filePath(QStringLiteral("testdata/media/sample-5s.mp4"));

    if (!exists(jpg) || !exists(mp4)) {
        return skip("missing testdata/media samples; run testdata/tools/Fetch-MediaSamples.ps1");
    }

    const QPixmap a = MediaThumbnails::imagePreviewPixmapForLocalFile(jpg);
    if (a.isNull() || a.width() <= 0 || a.height() <= 0) {
        fprintf(stderr, "imagePreviewPixmapForLocalFile returned null pixmap\n");
        return 1;
    }

    const QPixmap b = MediaThumbnails::videoPreviewPixmapForLocalFile(mp4);
    if (b.isNull() || b.width() <= 0 || b.height() <= 0) {
        fprintf(stderr, "videoPreviewPixmapForLocalFile returned null pixmap\n");
        return 1;
    }

    return 0;
}

