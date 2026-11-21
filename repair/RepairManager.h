#pragma once

#include "IFileRepair.h"
#include <vector>
#include <memory>

class RepairManager
{
public:
    RepairManager();

    void registerRepairer(std::unique_ptr<IFileRepair> repairer);

    IFileRepair* findRepairer(const QString& filePath) const;

    RepairResult repairFile(const QString& inputPath,
                            const QString& outputPath,
                            std::atomic<bool>* cancel = nullptr);

    QStringList supportedFormats() const;

private:
    std::vector<std::unique_ptr<IFileRepair>> repairers_;
};
