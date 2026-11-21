#pragma once

#include "../recover/FileNode.h"
#include "../disk/Partition.h"
#include "../common/Types.h"
#include <vector>
#include <functional>
#include <atomic>
#include <chrono>

class SectorReader;

// ── Scan progress snapshot ────────────────────────────────────────────────
struct ScanProgress
{
    uint64_t scannedBytes  = 0;
    uint64_t totalBytes    = 0;
    uint32_t foundCount    = 0;
    int      percent       = 0;   // 0..100
    double   speedMBps     = 0.0;
    std::wstring stageLabel;      // 例如：阶段 1/1 元数据解析
};

// ── Scan result ───────────────────────────────────────────────────────────
struct ScanResult
{
    std::vector<FileNode> files;
    uint64_t scannedBytes   = 0;
    uint32_t foundCount     = 0;   // number of deleted/lost files
    uint32_t totalRecords   = 0;   // total MFT/directory records parsed
    uint32_t inUseCount     = 0;   // normal (in-use) records seen
    bool     cancelled      = false;
    std::wstring errorMsg;
    std::chrono::milliseconds duration{0};
};

// ── Callbacks ─────────────────────────────────────────────────────────────
using ProgressCallback  = std::function<void(const ScanProgress&)>;
using FileFoundCallback = std::function<void(const FileNode&)>;
using CancelCallback    = std::function<bool()>; // Return true to cancel

// ── Abstract scan engine base ─────────────────────────────────────────────
class ScanEngine
{
public:
    virtual ~ScanEngine() = default;

    // Set callbacks before calling Run()
    void SetProgressCallback (ProgressCallback  cb) { m_onProgress  = std::move(cb); }
    void SetFileFoundCallback(FileFoundCallback cb) { m_onFileFound = std::move(cb); }
    void SetCancelCallback   (CancelCallback    cb) { m_cancelCb    = std::move(cb); }

    // Synchronous scan; blocks until complete or cancelled.
    virtual ScanResult Run(SectorReader& reader, const Partition& partition) = 0;

    // Request cancellation from another thread
    void Cancel() { m_cancelled.store(true); }

protected:
    bool IsCancelled() const
    {
        if (m_cancelled.load()) return true;
        if (m_cancelCb && m_cancelCb()) { m_cancelled.store(true); return true; }
        return false;
    }

    void ReportProgress(uint64_t scanned, uint64_t total, uint32_t found)
    {
        if (!m_onProgress) return;
        ScanProgress p;
        p.scannedBytes = scanned;
        p.totalBytes   = total;
        p.foundCount   = found;
        p.percent = (total > 0) ? static_cast<int>(scanned * 100 / total) : 0;
        m_onProgress(p);
    }

    void ReportFile(const FileNode& node)
    {
        if (m_onFileFound) m_onFileFound(node);
    }

    ProgressCallback          m_onProgress;
    FileFoundCallback         m_onFileFound;
    CancelCallback            m_cancelCb;
    mutable std::atomic<bool> m_cancelled{false};
};
