#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include <atomic>

// ---------------------------------------------------------------------------
// UsnRecord – one entry extracted from the NTFS $UsnJrnl:$J stream.
//
// Each entry describes the LAST known state of one file/directory based on
// the most recent USN record for that FRN (File Reference Number).
// ---------------------------------------------------------------------------
struct UsnRecord
{
    uint64_t     fileFrn    = 0; // File Reference Number (MFT record idx, low 48 bits)
    uint64_t     parentFrn  = 0; // Parent directory FRN
    std::wstring name;           // File name (UTF-16LE, from USN record)
    int64_t      timestamp  = 0; // Windows FILETIME (100-ns intervals since 1601)
    uint32_t     allReasons = 0; // OR of every reason flag ever seen for this FRN
    uint32_t     lastReason = 0; // Reason flags in the NEWEST USN record
    uint32_t     fileAttributes = 0; // WIN32 file attributes from newest record
    int64_t      usn        = 0; // USN of the newest record for this FRN
};

// ---------------------------------------------------------------------------
// USN reason flag constants  (mirrors winioctl.h – avoids including it in .h)
// ---------------------------------------------------------------------------
namespace UsnReason
{
    constexpr uint32_t DATA_OVERWRITE       = 0x00000001;
    constexpr uint32_t DATA_EXTEND          = 0x00000002;
    constexpr uint32_t DATA_TRUNCATION      = 0x00000004;
    // Avoid the Win32 macro names FILE_CREATE/FILE_DELETE.  This header is
    // included after Windows.h in some translation units, where those macros
    // can rewrite a qualified identifier and break compilation.
    constexpr uint32_t FILE_CREATE_REASON  = 0x00000100;
    constexpr uint32_t FILE_DELETE_REASON  = 0x00000200;
    constexpr uint32_t EA_CHANGE            = 0x00000400;
    constexpr uint32_t SECURITY_CHANGE      = 0x00000800;
    constexpr uint32_t RENAME_OLD_NAME      = 0x00001000;
    constexpr uint32_t RENAME_NEW_NAME      = 0x00002000;
    constexpr uint32_t INDEXABLE_CHANGE     = 0x00004000;
    constexpr uint32_t BASIC_INFO_CHANGE    = 0x00008000;
    constexpr uint32_t HARD_LINK_CHANGE     = 0x00010000;
    constexpr uint32_t COMPRESSION_CHANGE   = 0x00020000;
    constexpr uint32_t ENCRYPTION_CHANGE    = 0x00040000;
    constexpr uint32_t OBJECT_ID_CHANGE     = 0x00080000;
    constexpr uint32_t REPARSE_POINT_CHANGE = 0x00100000;
    constexpr uint32_t STREAM_CHANGE        = 0x00200000;
    constexpr uint32_t TRANSACTED_CHANGE    = 0x00400000;
    constexpr uint32_t CLOSE                = 0x80000000;
}

// ---------------------------------------------------------------------------
// UsnJournalParser
//
// Uses Windows FSCTL API to read the $UsnJrnl:$J data stream on a mounted
// NTFS volume.  Produces one UsnRecord per unique FRN seen in the journal,
// collapsing multiple entries for the same FRN into one.
//
// Usage:
//   UsnJournalParser parser;
//   std::vector<UsnRecord> recs;
//   parser.Scan(L"\\\\.\\C:", recs, progressFn);
// ---------------------------------------------------------------------------
class UsnJournalParser
{
public:
    // Scan the USN change journal.
    // volumePath – e.g. L"\\\\.\\C:" or L"\\\\.\\HarddiskVolume3"
    // outRecords – filled with one entry per unique FRN
    // onProgress – optional callback(bytesProcessed, totalBytes)
    // Returns false if the journal is not accessible.
    bool Scan(const std::wstring& volumePath,
              std::vector<UsnRecord>& outRecords,
              std::function<void(uint64_t /*done*/, uint64_t /*total*/)>
                  onProgress = nullptr,
              const std::atomic<bool>* cancelFlag = nullptr);

    // ── Stats after Scan() ────────────────────────────────────────────────
    uint64_t     GetRawRecordsRead() const { return m_rawRecords;  }
    uint64_t     GetUniqueFrns()     const { return m_uniqueFrns;  }
    std::wstring GetLastError()      const { return m_lastError;   }

    // Journal metadata (valid after successful Scan)
    uint64_t     GetJournalId()      const { return m_journalId;   }
    uint64_t     GetFirstUsn()       const { return m_firstUsn;    }
    uint64_t     GetNextUsn()        const { return m_nextUsn;     }
    uint64_t     GetMaxSizeBytes()   const { return m_maxSize;     }

private:
    uint64_t     m_rawRecords = 0;
    uint64_t     m_uniqueFrns = 0;
    uint64_t     m_journalId  = 0;
    uint64_t     m_firstUsn   = 0;
    uint64_t     m_nextUsn    = 0;
    uint64_t     m_maxSize    = 0;
    std::wstring m_lastError;
};
