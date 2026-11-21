#include "crash/CrashHandler.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

int main()
{
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "phoenix_crash_test";
    fs::create_directories(root);

    const fs::path log = root / "sample.log";
    {
        FILE* f = nullptr;
        _wfopen_s(&f, log.c_str(), L"wb");
        if (f) {
            const char* msg = "sample log for crash bundle\n";
            fwrite(msg, 1, strlen(msg), f);
            fclose(f);
        }
    }

    bool ok = CrashHandler::CreateDiagnosticBundle(L"unit_test", log.wstring(), root.wstring(), nullptr);
    if (!ok) {
        fprintf(stderr, "FAIL: CreateDiagnosticBundle returned false\n");
        return 1;
    }

    bool hasMeta = false, hasDump = false, hasLog = false;
    for (auto& e : fs::directory_iterator(root)) {
        auto ext = e.path().extension().wstring();
        if (ext == L".txt") hasMeta = true;
        if (ext == L".dmp") hasDump = true;
        if (ext == L".log") hasLog = true;
    }

    if (!hasMeta || !hasDump || !hasLog) {
        fprintf(stderr, "FAIL: expected txt/dmp/log outputs in crash bundle\n");
        return 1;
    }

    fprintf(stdout, "PASS: crash diagnostic bundle generated\n");
    return 0;
}
