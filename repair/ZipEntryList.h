#pragma once

#include <QByteArray>
#include <QStringList>

class QFile;

QStringList listZipLocalEntryNames(QFile& input);

QStringList listZipLocalEntryNamesFromPath(const QString& zipPath);
