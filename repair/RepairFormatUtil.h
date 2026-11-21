#pragma once

#include <QString>

namespace RepairFormatUtil {

bool isRepairableExtension(const QString& ext);

QStringList repairableExtensionFilter();

} // namespace RepairFormatUtil
