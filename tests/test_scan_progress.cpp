// ScanProgress / UnifiedScanConfig 结构体字段 单元测试
#include <cstdint>
#include <cstdlib>
#include <string>

#include "scan/ScanEngine.h"
#include "scan/UnifiedVolumeScanner.h"

#define CHECK(cond) \
    do { \
        if (!(cond)) return 1; \
    } while (0)

int main()
{
    // ── ScanProgress defaults ────────────────────────────────────────────────
    {
        ScanProgress sp;
        CHECK(sp.scannedBytes == 0);
        CHECK(sp.totalBytes   == 0);
        CHECK(sp.foundCount   == 0);
        CHECK(sp.percent      == 0);
        CHECK(sp.speedMBps    == 0.0);
        CHECK(sp.stageLabel.empty());
    }

    // ── ScanProgress manual values ───────────────────────────────────────────
    {
        ScanProgress sp;
        sp.scannedBytes = 1024 * 1024;
        sp.totalBytes   = 10ULL * 1024 * 1024;
        sp.foundCount   = 42;
        sp.percent      = 10;
        sp.speedMBps    = 120.5;
        sp.stageLabel   = L"阶段 1/3 元数据解析";

        CHECK(sp.scannedBytes == 1048576);
        CHECK(sp.totalBytes   == 10485760);
        CHECK(sp.foundCount   == 42);
        CHECK(sp.percent      == 10);
        CHECK(sp.speedMBps    > 120.0);
        CHECK(!sp.stageLabel.empty());
    }

    // ── ScanResult defaults ──────────────────────────────────────────────────
    {
        ScanResult sr;
        CHECK(sr.files.empty());
        CHECK(sr.scannedBytes  == 0);
        CHECK(sr.foundCount    == 0);
        CHECK(sr.totalRecords  == 0);
        CHECK(sr.inUseCount    == 0);
        CHECK(sr.cancelled     == false);
        CHECK(sr.errorMsg.empty());
        CHECK(sr.duration.count() == 0);
    }

    // ── UnifiedScanConfig defaults ───────────────────────────────────────────
    {
        UnifiedScanConfig cfg;
        CHECK(cfg.skipDeepScan == false);
        CHECK(cfg.skipRawScan  == false);
        CHECK(cfg.maxCarveFileSize == 100ULL * 1024 * 1024);
        CHECK(cfg.targetExtensions.empty());
    }

    // ── UnifiedScanConfig custom values ──────────────────────────────────────
    {
        UnifiedScanConfig cfg;
        cfg.skipDeepScan     = true;
        cfg.skipRawScan      = true;
        cfg.maxCarveFileSize = 50ULL * 1024 * 1024;
        cfg.targetExtensions = { L"jpg", L"png", L"pdf" };

        CHECK(cfg.skipDeepScan == true);
        CHECK(cfg.skipRawScan  == true);
        CHECK(cfg.maxCarveFileSize == 52428800);
        CHECK(cfg.targetExtensions.size() == 3);
        CHECK(cfg.targetExtensions[0] == L"jpg");
        CHECK(cfg.targetExtensions[2] == L"pdf");
    }

    return 0;
}
