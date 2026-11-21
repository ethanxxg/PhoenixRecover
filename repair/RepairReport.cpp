#include "RepairReport.h"

#include <QFile>
#include <QTextStream>

QString RepairReport::exportToText(const QVector<RepairJobRecord> &jobs)
{
    QString out;
    QTextStream ts(&out);
    ts << QStringLiteral("PhoenixRecover 文件修复报告\n");
    ts << QStringLiteral("========================================\n");
    for (const auto &job : jobs) {
        ts << QStringLiteral("\n文件: ") << job.inputPath << '\n';
        if (!job.formatName.isEmpty()) {
            ts << QStringLiteral("格式: ") << job.formatName << '\n';
        }
        ts << QStringLiteral("输出: ") << job.outputPath << '\n';
        ts << QStringLiteral("结果: ")
           << (job.result.success ? QStringLiteral("成功") : QStringLiteral("失败"))
           << '\n';
        ts << QStringLiteral("自检: ")
           << (job.result.validated ? QStringLiteral("通过") : QStringLiteral("未通过/未执行"))
           << '\n';
        if (!job.result.validationDetail.isEmpty()) {
            ts << QStringLiteral("校验详情: ") << job.result.validationDetail << '\n';
        }
        ts << QStringLiteral("说明: ") << job.result.message << '\n';
        ts << QStringLiteral("原始大小: ") << job.result.originalSize << '\n';
        ts << QStringLiteral("修复后大小: ") << job.result.repairedSize << '\n';
    }
    ts << QStringLiteral("\n汇总: 成功 ")
       << successCount(jobs) << QStringLiteral(" / 总计 ") << jobs.size() << '\n';
    return out;
}

bool RepairReport::exportToCsv(const QVector<RepairJobRecord> &jobs, const QString &filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    QTextStream ts(&f);
    ts.setCodec("UTF-8");
    ts << "input,output,format,success,validated,validation_detail,message,original_bytes,repaired_bytes\n";
    for (const auto &job : jobs) {
        auto esc = [](const QString &s) {
            QString q = s;
            q.replace('"', QStringLiteral("\"\""));
            return QStringLiteral("\"") + q + QStringLiteral("\"");
        };
        ts << esc(job.inputPath) << ','
           << esc(job.outputPath) << ','
           << esc(job.formatName) << ','
           << (job.result.success ? "1" : "0") << ','
           << (job.result.validated ? "1" : "0") << ','
           << esc(job.result.validationDetail) << ','
           << esc(job.result.message) << ','
           << job.result.originalSize << ','
           << job.result.repairedSize << '\n';
    }
    return true;
}

int RepairReport::successCount(const QVector<RepairJobRecord> &jobs)
{
    int n = 0;
    for (const auto &job : jobs) {
        if (job.result.success) {
            ++n;
        }
    }
    return n;
}
