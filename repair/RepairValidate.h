#pragma once

#include "RepairTypes.h"

#include <QString>

struct RepairValidationOutcome
{
    bool passed = false;
    QString detail;
};

RepairValidationOutcome validateZipContainer(const QString& path);
RepairValidationOutcome validateOfficeOpenXml(const QString& path, const QString& extension);
RepairValidationOutcome validateJpegFile(const QString& path);
RepairValidationOutcome validatePngFile(const QString& path);

void applyPostRepairValidation(RepairResult& result,
                               const QString& outputPath,
                               const QString& formatName,
                               const QString& inputPath);
