#include "FfmpegVideoThumb.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QStandardPaths>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryFile>
#include <QtGui/QImage>

namespace {

static QString findBundledFfmpegExe()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString c1 = appDir.filePath(QStringLiteral("ffmpeg.exe"));
    if (QFileInfo::exists(c1)) {
        return c1;
    }
    QDir d(QCoreApplication::applicationDirPath());
    if (d.cdUp() && d.cdUp() && d.cd(QStringLiteral("third_party/ffmpeg/bin"))) {
        const QString c2 = d.filePath(QStringLiteral("ffmpeg.exe"));
        if (QFileInfo::exists(c2)) {
            return c2;
        }
    }
    return {};
}

static QString resolveFfmpegExecutablePath()
{
    QString ff = QString::fromLocal8Bit(qgetenv("PHOENIX_FFMPEG"));
    if (ff.isEmpty() || !QFileInfo::exists(ff)) {
        ff = findBundledFfmpegExe();
    }
    if (ff.isEmpty() || !QFileInfo::exists(ff)) {
        ff = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    }
    if (ff.isEmpty() || !QFileInfo::exists(ff)) {
        const QString pathEnv = QString::fromLocal8Bit(qgetenv("PATH"));
        const QStringList pathDirs = pathEnv.split(QChar(';'), QString::SkipEmptyParts);
        ff = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"), pathDirs);
    }
    if (ff.isEmpty() || !QFileInfo::exists(ff)) {
        return {};
    }
    return QDir::toNativeSeparators(QFileInfo(ff).absoluteFilePath());
}

static bool runFfmpeg(const QString &ffExe, const QStringList &args)
{
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.setProgram(ffExe);
    proc.setArguments(args);
    proc.start();
    if (!proc.waitForFinished(60000)) {
        proc.kill();
        return false;
    }
    return proc.exitCode() == 0;
}

} // namespace

namespace FfmpegVideoThumb {

bool decodeRasterViaFfmpeg(const QString &localFilePath, QImage *outImage)
{
    if (!outImage || localFilePath.isEmpty()) {
        return false;
    }
    *outImage = QImage();

    const QString ffExe = resolveFfmpegExecutablePath();
    if (ffExe.isEmpty()) {
        return false;
    }
    const QString localNative = QDir::toNativeSeparators(localFilePath);
    if (localNative.isEmpty() || !QFileInfo::exists(localFilePath)) {
        return false;
    }

    QTemporaryFile outPng(QDir::tempPath() + QStringLiteral("/phxrv_ffimg_XXXXXX.png"));
    outPng.setAutoRemove(true);
    if (!outPng.open()) {
        return false;
    }
    const QString outPath = QDir::toNativeSeparators(outPng.fileName());
    outPng.close();

    QStringList base;
    base << QStringLiteral("-hide_banner") << QStringLiteral("-loglevel")
         << QStringLiteral("error");
    base << QStringLiteral("-y") << QStringLiteral("-i") << localNative;
    {
        QStringList a = base;
        a << QStringLiteral("-frames:v") << QStringLiteral("1") << QStringLiteral("-q:v")
          << QStringLiteral("2");
        a << outPath;
        if (runFfmpeg(ffExe, a) && outImage->load(outPath) && !outImage->isNull()) {
            return true;
        }
    }
    {
        QStringList a = base;
        a << QStringLiteral("-an");
        a << QStringLiteral("-frames:v") << QStringLiteral("1") << QStringLiteral("-q:v")
          << QStringLiteral("2");
        a << outPath;
        if (runFfmpeg(ffExe, a) && outImage->load(outPath) && !outImage->isNull()) {
            return true;
        }
    }
    return false;
}

bool extractFirstFrame(const QString &localFilePath, QImage *outImage)
{
    if (!outImage || localFilePath.isEmpty()) {
        return false;
    }
    *outImage = QImage();

    const QString ffExe = resolveFfmpegExecutablePath();
    if (ffExe.isEmpty()) {
        return false;
    }

    const QString localNative = QDir::toNativeSeparators(localFilePath);
    if (localNative.isEmpty() || !QFileInfo::exists(localFilePath)) {
        return false;
    }

    QTemporaryFile outPng(QDir::tempPath() + QStringLiteral("/phxrv_ffmpeg_XXXXXX.png"));
    outPng.setAutoRemove(true);
    if (!outPng.open()) {
        return false;
    }
    const QString outPath = QDir::toNativeSeparators(outPng.fileName());
    outPng.close();

    QStringList base;
    base << QStringLiteral("-hide_banner") << QStringLiteral("-loglevel")
         << QStringLiteral("error");
    base << QStringLiteral("-y");
    base << QStringLiteral("-i") << localNative;
    {
        QStringList a = base;
        a << QStringLiteral("-map") << QStringLiteral("0:v:0");
        a << QStringLiteral("-frames:v") << QStringLiteral("1") << QStringLiteral("-q:v")
          << QStringLiteral("3");
        a << outPath;
        if (runFfmpeg(ffExe, a) && outImage->load(outPath) && !outImage->isNull()) {
            return true;
        }
    }
    {
        QStringList a = base;
        a << QStringLiteral("-frames:v") << QStringLiteral("1") << QStringLiteral("-q:v")
          << QStringLiteral("3");
        a << outPath;
        if (runFfmpeg(ffExe, a) && outImage->load(outPath) && !outImage->isNull()) {
            return true;
        }
    }
    return false;
}

} // namespace FfmpegVideoThumb
