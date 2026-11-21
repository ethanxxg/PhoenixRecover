#pragma once

#include "FileNode.h"
#include "FragmentStitchPolicy.h"
#include "../disk/Partition.h"
#include "../disk/SectorReader.h"
#include <string>
#include <vector>
#include <functional>
#include <atomic>

// ── Per-file recovery outcome ──────────────────────────────────────────────
enum class RecoverStatus { Success, Partial, Failed, Skipped };

struct FileRecoverResult
{
    const FileNode* node    = nullptr;
    RecoverStatus   status  = RecoverStatus::Failed;
    std::wstring    reason;
    std::wstring    targetPath;
};

// ── Batch result ──────────────────────────────────────────────────────────
struct RecoverBatchResult
{
    uint32_t  succeeded  = 0;
    uint32_t  failed     = 0;
    uint32_t  skipped    = 0;
    bool      cancelled  = false;
    std::vector<FileRecoverResult> details;
};

// ── Recovery options (product behaviour) ───────────────────────────────────
struct RecoverOptions {
    // Format-aware FAT fragment reassembly (see FragmentStitchService).
    FragmentStitchPolicy fragmentStitch;

    // Mirrors fragmentStitch.enabled for existing call sites.
    bool formatAwareFatFragments = true;
};

// ── Callbacks ─────────────────────────────────────────────────────────────
using RecoverProgressCb =
    std::function<void(uint32_t done, uint32_t total)>;
using RecoverFileCb =
    std::function<void(const FileRecoverResult&)>;
using RecoverPauseCb =
    std::function<bool()>;

// ─────────────────────────────────────────────────────────────────────────────
// RecoverEngine – reads cluster/sector data and writes to target path
// ─────────────────────────────────────────────────────────────────────────────
class RecoverEngine
{
public:
    RecoverEngine() = default;

    void SetProgressCallback(RecoverProgressCb cb) { m_onProgress = std::move(cb); }
    void SetFileCallback     (RecoverFileCb     cb) { m_onFile     = std::move(cb); }
    void SetPauseCallback    (RecoverPauseCb    cb) { m_onPause    = std::move(cb); }
    void SetOptions(const RecoverOptions& opts)
    {
        m_options = opts;
        m_options.fragmentStitch.enabled = opts.formatAwareFatFragments;
    }
    const RecoverOptions& GetOptions() const { return m_options; }

    // Recover a single file
    FileRecoverResult RecoverFile(
        const FileNode&   node,
        const std::wstring& targetDir,
        bool              keepDirStructure,
        SectorReader&     reader,
        const Partition&  partition);

    // Recover a list of files
    RecoverBatchResult RecoverBatch(
        const std::vector<FileNode>& nodes,
        const std::wstring&          targetDir,
        bool                         keepDirStructure,
        SectorReader&                reader,
        const Partition&             partition,
        std::atomic<bool>*           cancelFlag = nullptr);

    // Validate that targetDir is NOT on the same partition as the source
    // to avoid overwriting the data we're trying to recover.
    static bool ValidateTargetPath(const std::wstring& targetDir,
                                   const Partition&    source);

    // Build the final target path for one node. When keepDirStructure is true and
    // node.originalPath is present, we try to preserve its relative hierarchy.
    static std::wstring BuildTargetPathForNode(const FileNode&      node,
                                               const std::wstring& targetDir,
                                               bool                keepDirStructure);

    // True if the first preview bytes are one contiguous volume read (offset + length).
    static bool TryPreviewSingleReadSlice(SectorReader& reader,
                                          const Partition& partition,
                                          const FileNode& node,
                                          uint32_t maxBytes,
                                          uint64_t& outVolumeByteOffset,
                                          uint32_t& outLength);

    // Read up to maxBytes of recoverable payload for preview (no file written).
    static bool ReadFileDataPrefix(SectorReader&       reader,
                                   const Partition&    partition,
                                   const FileNode&     node,
                                   uint32_t            maxBytes,
                                   std::vector<uint8_t>& out);

private:
    bool WriteRecoveredFile(const std::wstring& path,
                            SectorReader&       reader,
                            const FileNode&     node,
                            const Partition&    partition,
                            std::wstring&       outError);

    RecoverOptions    m_options;
    RecoverProgressCb m_onProgress;
    RecoverFileCb     m_onFile;
    RecoverPauseCb    m_onPause;
};
