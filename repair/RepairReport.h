#pragma once

#include "RepairTypes.h"

#include <QString>
#include <QVector>

struct RepairJobRecord
{
    QString inputPath;
    QString outputPath;
    QString formatName;
    RepairResult result;
};

class RepairReport
{
public:
    static QString exportToText(const QVector<RepairJobRecord> &jobs);
    static bool exportToCsv(const QVector<RepairJobRecord> &jobs, const QString &filePath);

    static int successCount(const QVector<RepairJobRecord> &jobs);
};
