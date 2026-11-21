#include "RepairFfmpeg.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace {

QString findBundledFfmpegExe()
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

bool runFfmpeg(const QString& ffExe, const QStringList& args, int timeoutMs = 300000)
{
    QProcess proc;
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(ffExe, args);
    if (!proc.waitForStarted(5000)) {
        return false;
    }
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(3000);
        return false;
    }
    return proc.exitCode() == 0;
}

} // namespace

QString RepairFfmpeg::executablePath()
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

bool RepairFfmpeg::remuxVideo(const QString& inputPath,
                              const QString& outputPath,
                              const QString& containerHint)
{
    const QString ff = executablePath();
    if (ff.isEmpty()) {
        return false;
    }

    QStringList args = {
        QStringLiteral("-y"),
        QStringLiteral("-err_detect"), QStringLiteral("ignore_err"),
        QStringLiteral("-i"), inputPath,
        QStringLiteral("-c"), QStringLiteral("copy"),
    };

    const QString hint = containerHint.toLower();
    if (hint == QStringLiteral("mp4") || hint == QStringLiteral("mov")
        || hint == QStringLiteral("m4v") || outputPath.endsWith(QStringLiteral(".mp4"), Qt::CaseInsensitive)
        || outputPath.endsWith(QStringLiteral(".mov"), Qt::CaseInsensitive)) {
        args << QStringLiteral("-movflags") << QStringLiteral("+faststart");
    }

    args << outputPath;
    return runFfmpeg(ff, args) && QFileInfo(outputPath).size() > 0;
}

bool RepairFfmpeg::transcodeImage(const QString& inputPath, const QString& outputPath)
{
    const QString ff = executablePath();
    if (ff.isEmpty()) {
        return false;
    }

    const QStringList args = {
        QStringLiteral("-y"),
        QStringLiteral("-err_detect"), QStringLiteral("ignore_err"),
        QStringLiteral("-i"), inputPath,
        outputPath,
    };
    return runFfmpeg(ff, args, 120000) && QFileInfo(outputPath).size() > 0;
}

bool RepairFfmpeg::exportRawPreviewJpeg(const QString& inputPath, const QString& outputJpegPath)
{
    const QString ff = executablePath();
    if (ff.isEmpty()) {
        return false;
    }

    const QStringList args = {
        QStringLiteral("-y"),
        QStringLiteral("-err_detect"), QStringLiteral("ignore_err"),
        QStringLiteral("-i"), inputPath,
        QStringLiteral("-frames:v"), QStringLiteral("1"),
        QStringLiteral("-q:v"), QStringLiteral("2"),
        outputJpegPath,
    };
    return runFfmpeg(ff, args, 120000) && QFileInfo(outputJpegPath).size() > 0;
}
