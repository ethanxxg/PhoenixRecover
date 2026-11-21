#pragma once

#include <QString>
#include <cstdint>
#include <atomic>

struct RepairResult
{
    bool    success      = false;
    bool    validated    = false;
    QString message;
    QString validationDetail;
    qint64  originalSize = 0;
    qint64  repairedSize = 0;
};
