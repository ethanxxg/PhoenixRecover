#include "UnifiedVolumeScanner.h"

#include "CarveGraphMergeService.h"
#include "CarveModuleService.h"
#include "EmbeddedJpegExtract.h"
#include "VolumeSectorScanner.h"
#include "../log/Logger.h"

// ─────────────────────────────────────────────────────────────────────────────
// FNV-1a 风格去重键
// ─────────────────────────────────────────────────────────────────────────────
uint64_t UnifiedVolumeScanner::DeduplicationKey(const FileNode &node)
{
    uint64_t offset = node.rawOffset;
    if (offset == 0 && !node.clusters.empty()) {
        offset = node.clusters[0];
    }
    uint64_t size = node.size;

    uint64_t h = 14695981039346656037ULL;
    h ^= offset; h *= 1099511628211ULL;
    h ^= size;   h *= 1099511628211ULL;
    return h;
}

// ─────────────────────────────────────────────────────────────────────────────
// 去重插入：返回 true 表示已接受（未重复）
// ─────────────────────────────────────────────────────────────────────────────
bool UnifiedVolumeScanner::TryInsertDeduplicated(FileNode &node)
{
    uint64_t key = DeduplicationKey(node);
    if (!deduplicationKeys_.insert(key).second) {
        return false;
    }

    if (!node.originalPath.empty()) {
        auto it = pathIndex_.find(node.originalPath);
        if (it != pathIndex_.end()) {
            return false;
        }
        pathIndex_[node.originalPath] = mergedFiles_.size();
    }

    mergedFiles_.push_back(std::move(node));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// 全局进度计算
// ─────────────────────────────────────────────────────────────────────────────
int UnifiedVolumeScanner::ComputeGlobalPercent(int phase, int localPercent)
{
    static constexpr int kBase[]   = { 0, 30, 70 };
    static constexpr int kWeight[] = { 30, 40, 30 };
    if (phase < 0 || phase > 2) return localPercent;
    return kBase[phase] + (localPercent * kWeight[phase]) / 100;
}

// ─────────────────────────────────────────────────────────────────────────────
// 进度回调适配：将子引擎本地进度映射到全局进度
// ─────────────────────────────────────────────────────────────────────────────
void UnifiedVolumeScanner::WrapProgressCallback(int phase, const ScanProgress &local)
{
    if (!onProgress_) return;

    ScanProgress global;
    global.scannedBytes = local.scannedBytes;
    global.totalBytes   = local.totalBytes;
    global.foundCount   = static_cast<uint32_t>(mergedFiles_.size());
    global.percent      = ComputeGlobalPercent(phase, local.percent);
    global.speedMBps    = local.speedMBps;

    static constexpr const wchar_t *kLabels[] = {
        L"阶段 1/3 元数据解析",
        L"阶段 2/3 深度扫描",
        L"阶段 3/3 签名雕刻"
    };
    global.stageLabel = kLabels[phase];

    onProgress_(global);
}

// ─────────────────────────────────────────────────────────────────────────────
// Run：三阶段流水线主入口
// ─────────────────────────────────────────────────────────────────────────────
ScanResult UnifiedVolumeScanner::Run(SectorReader &reader, const Partition &partition)
{
    deduplicationKeys_.clear();
    pathIndex_.clear();
    mergedFiles_.clear();

    ScanResult result;
    auto isCancelled = [this]() {
        return cancelFlag_ && cancelFlag_->load();
    };

    LOG_INFO(L"UnifiedVolumeScanner::Run – 三阶段流水线启动"
             L" skipDeep=" + std::to_wstring(config_.skipDeepScan ? 1 : 0) +
             L" skipRaw=" + std::to_wstring(config_.skipRawScan ? 1 : 0));

    // 初始进度通知
    if (onProgress_) {
        ScanProgress boot;
        boot.stageLabel = L"阶段 1/3 元数据解析";
        boot.percent = 0;
        boot.scannedBytes = 0;
        boot.totalBytes = partition.sizeBytes;
        onProgress_(boot);
    }

    // ── 阶段 1: FS 元数据解析 ────────────────────────────────────────────────
    if (!isCancelled()) {
        RunPhase1(reader, partition, result);
    }

    // ── 阶段 2: 深度扫描 ─────────────────────────────────────────────────────
    if (!isCancelled() && !config_.skipDeepScan) {
        RunPhase2(reader, partition, result);
    }

    // ── 阶段 3: 签名雕刻 ─────────────────────────────────────────────────────
    if (!isCancelled() && !config_.skipRawScan) {
        RunPhase3(reader, partition, result);
    }

    // 汇总
    result.cancelled = isCancelled();
    result.files = std::move(mergedFiles_);
    result.foundCount = static_cast<uint32_t>(result.files.size());

    // 最终进度通知
    if (onProgress_) {
        ScanProgress done;
        done.stageLabel = L"扫描完成";
        done.percent = 100;
        done.foundCount = result.foundCount;
        done.scannedBytes = result.scannedBytes;
        done.totalBytes = partition.sizeBytes;
        onProgress_(done);
    }

    LOG_INFO(L"UnifiedVolumeScanner::Run – 完成"
             L" files=" + std::to_wstring(result.foundCount) +
             L" cancelled=" + std::to_wstring(result.cancelled ? 1 : 0));

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// 阶段 1: FS 元数据解析（VolumeSectorScanner）
// ─────────────────────────────────────────────────────────────────────────────
void UnifiedVolumeScanner::RunPhase1(SectorReader &reader, const Partition &partition,
                                      ScanResult &result)
{
    LOG_INFO(L"UnifiedVolumeScanner – 阶段 1 开始");

    VolumeSectorScanner scanner;
    scanner.SetCancelFlag(cancelFlag_);
    scanner.SetFragmentStitchPolicy(config_.fragmentStitch);
    scanner.SetProgressCallback([this](const ScanProgress &p) {
        WrapProgressCallback(0, p);
    });
    scanner.SetFileFoundCallback([this](const FileNode &n) {
        FileNode copy = n;
        if (TryInsertDeduplicated(copy) && onFileFound_) {
            onFileFound_(n);
        }
    });

    ScanResult phase = scanner.Run(reader, partition);
    result.scannedBytes += phase.scannedBytes;
    result.totalRecords += phase.totalRecords;
    result.inUseCount += phase.inUseCount;

    // Container / archive embedded JPEG discovery (engine capability, not UI).
    uint64_t nextEmbedId = 1;
    for (const FileNode& n : mergedFiles_) {
        if (n.id >= nextEmbedId)
            nextEmbedId = n.id + 1;
    }
    const std::vector<FileNode> hosts = mergedFiles_;
    std::vector<FileNode> embedded =
        EmbeddedJpegExtract::ExpandFromHosts(reader, partition, hosts, nextEmbedId);
    for (FileNode& node : embedded) {
        if (TryInsertDeduplicated(node) && onFileFound_) {
            onFileFound_(node);
        }
    }

    LOG_INFO(L"UnifiedVolumeScanner – 阶段 1 结束"
             L" found=" + std::to_wstring(mergedFiles_.size()));
}

// ─────────────────────────────────────────────────────────────────────────────
// 阶段 2: 深度扫描（DeepScan）
// ─────────────────────────────────────────────────────────────────────────────
void UnifiedVolumeScanner::RunPhase2(SectorReader &reader, const Partition &partition,
                                      ScanResult &result)
{
    LOG_INFO(L"UnifiedVolumeScanner – 阶段 2 开始");

    DeepScan engine;
    engine.SetCancelCallback([this]() {
        return cancelFlag_ && cancelFlag_->load();
    });
    engine.SetProgressCallback([this](const ScanProgress &p) {
        WrapProgressCallback(1, p);
    });
    engine.SetFileFoundCallback([this](const FileNode &n) {
        FileNode copy = n;
        if (TryInsertDeduplicated(copy) && onFileFound_) {
            onFileFound_(n);
        }
    });

    ScanResult phase = engine.Run(reader, partition);
    result.scannedBytes += phase.scannedBytes;

    LOG_INFO(L"UnifiedVolumeScanner – 阶段 2 结束"
             L" totalFound=" + std::to_wstring(mergedFiles_.size()));
}

// ─────────────────────────────────────────────────────────────────────────────
// 阶段 3: 签名雕刻（RawScan）
// ─────────────────────────────────────────────────────────────────────────────
void UnifiedVolumeScanner::RunPhase3(SectorReader &reader, const Partition &partition,
                                      ScanResult &result)
{
    LOG_INFO(L"UnifiedVolumeScanner – 阶段 3 开始");

    // A1-10: single source of truth for the phase-3 config mapping (inline,
    // headless-testable). rawScanThreads flows through to RawScan here.
    CarveModuleService::CarveConfig carveCfg = MakeCarveConfig(config_);

    std::vector<FileNode> phase3Raw;
    ScanResult phase = CarveModuleService::Run(
        reader,
        partition,
        carveCfg,
        cancelFlag_,
        [this](const ScanProgress& p) { WrapProgressCallback(2, p); },
        [&phase3Raw](const FileNode& n) { phase3Raw.push_back(n); });

    const uint32_t graphMerged = CarveGraphMergeService::MergeRawCarveCandidates(
        reader, partition, phase3Raw, config_.fragmentStitch);

    for (auto& n : phase3Raw) {
        if (TryInsertDeduplicated(n) && onFileFound_) {
            onFileFound_(n);
        }
    }

    result.scannedBytes += phase.scannedBytes;

    LOG_INFO(L"UnifiedVolumeScanner – 阶段 3 结束"
             L" rawHits=" + std::to_wstring(phase.foundCount) +
             L" graphMerged=" + std::to_wstring(graphMerged) +
             L" totalFound=" + std::to_wstring(mergedFiles_.size()));
}
