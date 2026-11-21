#include "RepairManager.h"
#include "RepairValidate.h"
#include "OfficeZipRepair.h"
#include "ZipRepair.h"
#include "JpegRepair.h"
#include "PngRepair.h"
#include "GifRepair.h"
#include "Mp4Repair.h"
#include "FfmpegVideoRepair.h"
#include "FfmpegImageRepair.h"
#include "FfmpegRawRepair.h"
#include "SqliteRepair.h"

#include <QFile>
#include <QFileInfo>

RepairManager::RepairManager()
{
    registerRepairer(std::make_unique<OfficeZipRepair>());
    registerRepairer(std::make_unique<ZipRepair>());
    registerRepairer(std::make_unique<JpegRepair>());
    registerRepairer(std::make_unique<PngRepair>());
    registerRepairer(std::make_unique<GifRepair>());
    registerRepairer(std::make_unique<Mp4Repair>());
    registerRepairer(std::make_unique<FfmpegVideoRepair>());
    registerRepairer(std::make_unique<FfmpegImageRepair>());
    registerRepairer(std::make_unique<FfmpegRawRepair>());
    registerRepairer(std::make_unique<SqliteRepair>());
}

void RepairManager::registerRepairer(std::unique_ptr<IFileRepair> repairer)
{
    if (repairer)
        repairers_.push_back(std::move(repairer));
}

IFileRepair* RepairManager::findRepairer(const QString& filePath) const
{
    QFileInfo fi(filePath);
    QString ext = fi.suffix().toLower();

    QFile file(filePath);
    QByteArray header;
    qint64 fileSize = 0;

    if (file.open(QIODevice::ReadOnly)) {
        fileSize = file.size();
        header = file.read(4096);
        file.close();
    }

    for (auto& r : repairers_) {
        if (!r->supportedExtensions().contains(ext, Qt::CaseInsensitive))
            continue;
        if (r->canRepair(header, fileSize))
            return r.get();
    }

    for (auto& r : repairers_) {
        if (r->canRepair(header, fileSize))
            return r.get();
    }

    return nullptr;
}

RepairResult RepairManager::repairFile(const QString& inputPath,
                                       const QString& outputPath,
                                       std::atomic<bool>* cancel)
{
    IFileRepair* repairer = findRepairer(inputPath);
    if (!repairer) {
        RepairResult result;
        result.message = QStringLiteral("未找到适合的修复器");
        return result;
    }
    RepairResult result = repairer->repair(inputPath, outputPath, cancel);
    applyPostRepairValidation(result, outputPath, repairer->formatName(), inputPath);
    return result;
}

QStringList RepairManager::supportedFormats() const
{
    QStringList formats;
    for (auto& r : repairers_)
        formats.append(r->formatName());
    return formats;
}
