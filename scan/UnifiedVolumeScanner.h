#pragma once

#include "ScanEngine.h"
#include "DeepScan.h"
#include "CarveModuleService.h"
#include "../recover/FragmentStitchPolicy.h"

#include <atomic>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <string>

// ── 统一扫描配置 ─────────────────────────────────────────────────────────────
struct UnifiedScanConfig
{
    bool skipDeepScan = false;
    bool skipRawScan  = false;
    uint64_t maxCarveFileSize = 100ULL * 1024 * 1024; // 100 MB
    std::vector<std::wstring> targetExtensions;       // 空 = 全部
    FragmentStitchPolicy fragmentStitch;

    // A1-10: phase-3 RAW-carving worker count. 1 (default) = serial, so the
    // shipped behaviour is unchanged; 0 = auto; N > 1 = explicit (clamped).
    // Read from QSettings "recover/rawScanThreads" by RecoveryScanWorker.
    size_t rawScanThreads = 1;
};

// Pure config mapping for phase 3. Kept inline so the wiring can be verified
// by a headless CTest without linking the whole FS-parser stack that
// UnifiedVolumeScanner::Run() pulls in.
inline CarveModuleService::CarveConfig MakeCarveConfig(const UnifiedScanConfig& c)
{
    CarveModuleService::CarveConfig cfg;
    cfg.maxFileSize       = c.maxCarveFileSize;
    cfg.targetExtensions  = c.targetExtensions;
    cfg.numThreads        = c.rawScanThreads;
    return cfg;
}

// ── 统一卷扫描器：三阶段流水线 ───────────────────────────────────────────────
class UnifiedVolumeScanner
{
public:
    void SetCancelFlag(std::atomic<bool> *flag) { cancelFlag_ = flag; }
    void SetProgressCallback(ProgressCallback cb) { onProgress_ = std::move(cb); }
    void SetFileFoundCallback(FileFoundCallback cb) { onFileFound_ = std::move(cb); }
    void SetConfig(const UnifiedScanConfig &config) { config_ = config; }

    ScanResult Run(SectorReader &reader, const Partition &partition);

private:
    // 三阶段执行方法
    void RunPhase1(SectorReader &reader, const Partition &partition, ScanResult &result);
    void RunPhase2(SectorReader &reader, const Partition &partition, ScanResult &result);
    void RunPhase3(SectorReader &reader, const Partition &partition, ScanResult &result);

    // 去重辅助
    static uint64_t DeduplicationKey(const FileNode &node);
    bool TryInsertDeduplicated(FileNode &node);

    // 进度适配
    void WrapProgressCallback(int phase, const ScanProgress &local);
    static int ComputeGlobalPercent(int phase, int localPercent);

    // ── 成员变量 ──────────────────────────────────────────────────────────────
    std::atomic<bool> *cancelFlag_ = nullptr;
    ProgressCallback onProgress_;
    FileFoundCallback onFileFound_;
    UnifiedScanConfig config_;

    // 去重状态（每次 Run() 开始时清空）
    std::unordered_set<uint64_t> deduplicationKeys_;
    std::unordered_map<std::wstring, size_t> pathIndex_;
    std::vector<FileNode> mergedFiles_;
};
