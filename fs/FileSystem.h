#pragma once

#include "../recover/FileNode.h"
#include "../disk/Partition.h"
#include <vector>
#include <functional>
#include <string>
#include <atomic>

class SectorReader;

// ── Callback for real-time file discovery ──────────────────────────────────
using FileFoundCallback = std::function<void(const FileNode&)>;

// ---------------------------------------------------------------------------
// FileSystem – abstract interface for all file system parsers.
//   Implementations: NtfsParser, FatParser, ExfatParser
// ---------------------------------------------------------------------------
class FileSystem
{
public:
    virtual ~FileSystem() = default;

    // Attach reader + partition; perform any one-time validation (boot sector).
    // Returns false if the partition is not recognised by this parser.
    virtual bool Open(SectorReader& reader, const Partition& partition) = 0;

    // Read file system metadata (MFT, FAT directory tree, exFAT cluster bitmap…)
    // and build the internal file list.
    // 'onProgress'  is called periodically with [0..100].
    // 'cancelFlag'  is checked regularly; if true the scan should abort early.
    virtual bool ScanMetadata(
        std::function<void(int /*pct*/)>  onProgress = nullptr,
        std::atomic<bool>*               cancelFlag  = nullptr) = 0;

    // Returns all discovered FileNodes (deleted + normal).
    virtual std::vector<FileNode> GetAllFiles() const = 0;

    // Human-readable name, e.g. "NTFS"
    virtual std::wstring GetFsTypeName() const = 0;

    // True if Open() succeeded and the FS is structurally valid
    virtual bool IsValid() const = 0;

    // Real-time count of discovered nodes so far (during ScanMetadata).
    // Default returns 0; override in parsers for live progress display.
    virtual uint32_t GetCurrentFileCount() const { return 0; }

    // After ScanMetadata: total MFT/directory records visited (all valid records).
    virtual uint32_t GetTotalRecordCount() const { return 0; }

    // After ScanMetadata: records that were in-use (normal files/dirs).
    virtual uint32_t GetInUseCount() const { return 0; }

    // Set callback for real-time node discovery (called immediately when a node is found).
    // Default implementation does nothing; override in parsers to enable real-time reporting.
    virtual void SetFileFoundCallback(FileFoundCallback cb) { (void)cb; }
};
