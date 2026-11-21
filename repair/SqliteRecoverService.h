#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// SQLite 3 page-level recovery (no Qt). Used by SqliteRepair and tests.
namespace SqliteRecoverService {

struct RecoverOptions {
    // Stop at first invalid page (truncate tail). If false, skip bad pages (best-effort).
    bool stopAtFirstBadPage = true;
    // Search for header magic within the first N bytes (carved RAW).
    size_t maxHeaderSearchBytes = 64 * 1024;
};

struct RecoverReport {
    bool     success      = false;
    uint32_t pageSize     = 0;
    uint32_t pagesCopied  = 0;
    uint32_t pagesSkipped = 0;
    size_t   sourceOffset = 0; // byte offset of DB header in input
    std::wstring message;
};

static constexpr size_t kHeaderSize = 100;

bool IsSqliteMagic(const uint8_t* data, size_t len);
uint32_t ReadPageSizeFromHeader(const uint8_t* header100);
bool IsValidPageType(uint8_t pageType);

// Recover into a compacted DB (valid prefix pages, renumbered 1..N).
RecoverReport RecoverDatabase(const uint8_t* input,
                              size_t inputLen,
                              std::vector<uint8_t>& outputDb,
                              const RecoverOptions& opt = {});

RecoverReport RecoverFile(const std::wstring& inputPath,
                          const std::wstring& outputPath,
                          const RecoverOptions& opt = {});

} // namespace SqliteRecoverService
