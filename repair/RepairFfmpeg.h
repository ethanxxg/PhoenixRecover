#pragma once

#include <QString>
#include <QStringList>

class RepairFfmpeg
{
public:
    static QString executablePath();

    static bool remuxVideo(const QString& inputPath,
                           const QString& outputPath,
                           const QString& containerHint = QString());

    static bool transcodeImage(const QString& inputPath, const QString& outputPath);

    static bool exportRawPreviewJpeg(const QString& inputPath, const QString& outputJpegPath);
};
