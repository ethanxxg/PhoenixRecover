#include "CarveModuleService.h"

#include "RawScan.h"

#include <algorithm>
#include <memory>
#include <thread>

namespace CarveModuleService {

namespace {

// A1-10: resolve the requested worker count into a safe, bounded value.
//   0 = "auto" (hardware concurrency, capped at 32)
//   1 = serial (the default — preserves the pre-A1-10 behaviour exactly)
//   N = explicit, clamped to [1, 32] and to the machine's available cores.
// A hard ceiling matters here because every worker owns its own SectorReader
// handle; a bad QSettings value must not spawn thousands of threads.
size_t ResolveThreadCount(size_t requested)
{
    const unsigned hw = std::thread::hardware_concurrency();
    size_t cap = hw > 0 ? static_cast<size_t>(hw) : static_cast<size_t>(4);
    cap = (std::min)(cap, static_cast<size_t>(32));

    if (requested == 0)
        return cap > 1 ? cap : 1;
    return (std::min)(requested, cap);
}

} // namespace

ScanResult Run(SectorReader& reader,
               const Partition& partition,
               const CarveConfig& config,
               const std::atomic<bool>* cancelFlag,
               ProgressCallback onProgress,
               FileFoundCallback onFileFound)
{
    SignatureDB builtIn;
    SignatureDB* effectiveDb = config.customDb ? config.customDb : &builtIn;

    // A1-10: the previous implementation built `filteredDb` with the default
    // ctor (which already contains all 513 built-ins) and then *appended* the
    // matching signatures — so `targetExtensions` never actually filtered
    // anything.  Use the exact-signature-set ctor so the effective DB contains
    // only the requested extensions, in original DB order (tie-break matters).
    std::unique_ptr<SignatureDB> filteredDb;
    if (!config.targetExtensions.empty()) {
        const SignatureDB& source = config.customDb ? *config.customDb : builtIn;
        std::vector<FileSignature> selected;
        for (const auto& sig : source.GetAll()) {
            for (const auto& ext : config.targetExtensions) {
                if (_wcsicmp(sig.extension.c_str(), ext.c_str()) == 0) {
                    selected.push_back(sig);
                    break;
                }
            }
        }
        filteredDb = std::make_unique<SignatureDB>(selected);
        effectiveDb = filteredDb.get();
    }

    RawScan engine(effectiveDb);
    engine.SetThreadCount(ResolveThreadCount(config.numThreads));
    if (cancelFlag) {
        engine.SetCancelCallback([cancelFlag]() { return cancelFlag->load(); });
    }
    if (onProgress) {
        engine.SetProgressCallback(std::move(onProgress));
    }
    if (onFileFound) {
        engine.SetFileFoundCallback([config, cb = std::move(onFileFound)](const FileNode& n) {
            if (n.size > config.maxFileSize) {
                return;
            }
            cb(n);
        });
    }

    // Run() is the virtual 2-arg override; it now honours the thread count set
    // above and delegates to the parallel overload when > 1.  All file/progress
    // callbacks are still emitted on this (calling) thread after the merge.
    return engine.Run(reader, partition);
}

} // namespace CarveModuleService
