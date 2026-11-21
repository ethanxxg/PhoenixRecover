#include "repair/RepairManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstdio>
#include <filesystem>

#ifndef PHOENIX_SOURCE_DIR
#define PHOENIX_SOURCE_DIR L"."
#endif

namespace fs = std::filesystem;

static fs::path sourceRoot()
{
    return fs::path(PHOENIX_SOURCE_DIR);
}

static bool sampleExists(const fs::path &p)
{
    return fs::exists(p) && fs::file_size(p) > 0;
}

static int runRepairCase(RepairManager &mgr,
                         const fs::path &input,
                         const fs::path &output,
                         bool expectSuccess)
{
    const QString in = QString::fromStdWString(input.wstring());
    const QString out = QString::fromStdWString(output.wstring());

    QDir().mkpath(QString::fromStdWString(output.parent_path().wstring()));

    const RepairResult r = mgr.repairFile(in, out, nullptr);
    if (r.success != expectSuccess) {
        fprintf(stderr,
                "FAIL: %ls expected success=%d got %d msg=%ls\n",
                input.filename().c_str(),
                expectSuccess ? 1 : 0,
                r.success ? 1 : 0,
                r.message.toStdWString().c_str());
        return 1;
    }
    if (expectSuccess && !QFile::exists(out)) {
        fprintf(stderr, "FAIL: output missing %ls\n", output.c_str());
        return 1;
    }
    fprintf(stdout, "PASS: %ls -> %ls (%ls)\n",
            input.filename().c_str(),
            output.filename().c_str(),
            r.message.toStdWString().c_str());
    return 0;
}

int main()
{
    const fs::path root = sourceRoot() / "testdata" / "repair_samples";
    const fs::path outDir = fs::temp_directory_path() / "phoenix_repair_test_out";
    fs::create_directories(outDir);

    const fs::path truncZip = root / "truncated.zip";
    const fs::path corruptMp4 = root / "corrupted_ftyp.mp4";
    const fs::path truncJpg = root / "truncated.jpg";

    if (!sampleExists(truncZip) || !sampleExists(corruptMp4)) {
        fprintf(stdout,
                "SKIP: repair_samples not present under %ls (generate per testdata/README.md)\n",
                root.c_str());
        return 0;
    }

    RepairManager mgr;
    int code = 0;
    code |= runRepairCase(mgr,
                          truncZip,
                          outDir / "truncated_repaired.zip",
                          true);
    code |= runRepairCase(mgr,
                          corruptMp4,
                          outDir / "corrupted_ftyp_repaired.mp4",
                          true);

    if (sampleExists(truncJpg)) {
        code |= runRepairCase(mgr, truncJpg, outDir / "truncated_repaired.jpg", true);
    }

    const fs::path truncDocx = root / "truncated.docx";
    if (sampleExists(truncDocx)) {
        code |= runRepairCase(mgr, truncDocx, outDir / "truncated_repaired.docx", true);
    }

    return code;
}
