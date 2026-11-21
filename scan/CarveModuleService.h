#pragma once

#include "ScanEngine.h"
#include "SignatureDB.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Low-coupling entry for file-signature carving (phase-3 / RAW recovery).
namespace CarveModuleService {

struct CarveConfig {
    uint64_t maxFileSize = 100ULL * 1024 * 1024;
    std::vector<std::wstring> targetExtensions; // empty = all signatures
    SignatureDB* customDb = nullptr;            // nullptr = built-in RawScan database

    // A1-10: RAW-carving worker count. 1 (default) = serial, preserving the
    // original behaviour; 0 = auto (hardware concurrency, capped at 32);
    // N > 1 = explicit, clamped to [1, 32]. Sources that cannot be cloned
    // (devices / virtual disks) automatically fall back to a single thread.
    size_t numThreads = 1;
};

ScanResult Run(SectorReader& reader,
               const Partition& partition,
               const CarveConfig& config,
               const std::atomic<bool>* cancelFlag = nullptr,
               ProgressCallback onProgress = nullptr,
               FileFoundCallback onFileFound = nullptr);

} // namespace CarveModuleService
