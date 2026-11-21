#pragma once

#include "ScanEngine.h"
#include "SignatureDB.h"

// ---------------------------------------------------------------------------
// RawScan – file-signature (file-carving) scan.
//           Does not depend on any file system structure.
//           Works on RAW partitions and as a last-resort recovery method.
// ---------------------------------------------------------------------------
class RawScan : public ScanEngine
{
public:
    // Optionally inject a custom signature database; otherwise uses the built-in one.
    explicit RawScan(SignatureDB* db = nullptr);

    // A1-10: worker count used by the 2-arg Run(). 1 (default) keeps the exact
    // pre-A1-10 serial code path, so every existing caller is unaffected;
    // values > 1 delegate to the parallel overload below. 0 normalises to 1
    // here ("auto" is resolved one layer up, in CarveModuleService).
    void SetThreadCount(size_t n) { m_threadCount = n ? n : 1; }
    size_t GetThreadCount() const { return m_threadCount; }

    // W8 / CAR-002: optional strict structural check on the bytes following a
    // matched magic.  DEFAULT OFF — turning it on changes which RAW nodes are
    // emitted, so it must be enabled explicitly by a caller that accepts the
    // recall/precision trade-off.  See PERFORMANCE_BASELINE.md §10.
    void SetStrictHeaderValidation(bool on) { m_strictHeaderValidation = on; }
    bool GetStrictHeaderValidation() const { return m_strictHeaderValidation; }

    ScanResult Run(SectorReader& reader, const Partition& partition) override;

    // Parallel variant: splits the partition into per-thread ranges and scans
    // them concurrently (one SectorReader + SignatureDB per thread).  Produces
    // the same file set as the serial Run(); used to speed up large images.
    // Each worker receives a private DB cloned from the EFFECTIVE signature set
    // (m_db), so custom / extension-filtered databases are honoured.
    ScanResult Run(SectorReader& reader, const Partition& partition, size_t numThreads);

private:
    // Core scan loop over [startSector, endSector).  Appends discovered nodes
    // to 'out' (the per-thread / per-run result vector) and advances
    // 'fileIdCounter'.  'db' is matched against explicitly so parallel workers
    // can each supply a private SignatureDB instance (ScanBatch is not
    // thread-safe).  For startSector > 0 the 512-byte prefix that logically
    // precedes the range is primed from the reader, exactly mirroring the
    // serial scan's inter-batch overlap; startSector == 0 uses an all-zero
    // prefix like the original first batch.
    bool ScanRangeCore(SectorReader& reader, const Partition& partition,
                       SignatureDB* db, uint64_t totalBytes,
                       uint64_t startSector, uint64_t endSector,
                       std::vector<FileNode>& out, uint64_t& fileIdCounter);

    // Search forward from 'startOffset' for the footer signature.
    // Returns the offset just past the footer, or startOffset + maxSize on failure.
    uint64_t FindFooter(SectorReader& reader, uint64_t startOffset,
                        const FileSignature& sig, uint64_t maxOffset);

    // Lightweight structural validation for RAW files based on their type.
    // Only uses small reads around the header/footer; does NOT attempt
    // fragment reassembly.  Returns a best-effort RecoverabilityLevel.
    RecoverabilityLevel AssessRawStructure(SectorReader& reader,
                                           const Partition& partition,
                                           const FileNode& node,
                                           const FileSignature& sig);

    void AddFragmentNodes(SectorReader& reader, uint64_t partByteEnd,
                          std::vector<FileNode>& files, uint64_t& fileIdCounter);

    void RescanMissingGraphicHeaders(SectorReader& reader, const Partition& partition,
                                     uint64_t partByteStart, uint64_t partByteEnd,
                                     std::vector<FileNode>& files, uint64_t& fileIdCounter);

    void RescanNestedGraphicHeaders(SectorReader& reader, const Partition& partition,
                                    uint64_t partByteEnd, std::vector<FileNode>& files,
                                    uint64_t& fileIdCounter);

    SignatureDB  m_builtIn;
    SignatureDB* m_db;
    size_t       m_threadCount = 1;   // 1 = serial (default)
    bool         m_strictHeaderValidation = false; // W8, default OFF
};
