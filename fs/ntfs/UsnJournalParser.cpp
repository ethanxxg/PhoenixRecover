#include "UsnJournalParser.h"
#include "../../common/Platform.h"
#include "../../log/Logger.h"

#if PHOENIX_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>

#include <atomic>
#include <unordered_map>
#include <sstream>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// UsnJournalParser::Scan
//
// Uses FSCTL_QUERY_USN_JOURNAL + FSCTL_READ_USN_JOURNAL to read every change
// record from the NTFS USN change journal on a mounted volume.
//
// We collapse multiple records for the same FRN into one UsnRecord: accumulated
// reason flags (allReasons) plus the data from the newest record (highest USN).
// ─────────────────────────────────────────────────────────────────────────────
bool UsnJournalParser::Scan(
        const std::wstring& volumePath,
        std::vector<UsnRecord>& outRecords,
        std::function<void(uint64_t, uint64_t)> onProgress,
        const std::atomic<bool>* cancelFlag)
{
    m_rawRecords = 0;
    m_uniqueFrns = 0;
    m_journalId  = 0;
    m_firstUsn   = 0;
    m_nextUsn    = 0;
    m_maxSize    = 0;
    m_lastError.clear();
    outRecords.clear();

    // ── Step 1: open the volume ───────────────────────────────────────────
    HANDLE hVol = CreateFileW(
        volumePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, 0, nullptr);

    if (hVol == INVALID_HANDLE_VALUE)
    {
        DWORD gle = ::GetLastError();
        std::wostringstream ss;
        ss << L"CreateFile(" << volumePath << L") failed, error=" << gle;
        m_lastError = ss.str();
        LOG_ERROR(m_lastError);
        return false;
    }

    // ── Step 2: query journal metadata ───────────────────────────────────
    // Use the SDK's canonical aliases rather than the version-suffixed names.
    // MinGW and older Windows SDKs expose these structures under the base
    // names, while MSVC exposes both variants.
    USN_JOURNAL_DATA jdata = {};
    DWORD br = 0;

    if (!DeviceIoControl(hVol, FSCTL_QUERY_USN_JOURNAL,
                         nullptr, 0,
                         &jdata, sizeof(jdata),
                         &br, nullptr))
    {
        DWORD gle = ::GetLastError();
        std::wostringstream ss;
        ss << L"FSCTL_QUERY_USN_JOURNAL failed on " << volumePath
           << L", error=" << gle;
        if (gle == ERROR_JOURNAL_NOT_ACTIVE)
            ss << L" (journal not active)";
        else if (gle == ERROR_JOURNAL_DELETE_IN_PROGRESS)
            ss << L" (journal deletion in progress)";
        m_lastError = ss.str();
        LOG_WARN(m_lastError);
        CloseHandle(hVol);
        return false;
    }

    m_journalId = jdata.UsnJournalID;
    m_firstUsn  = static_cast<uint64_t>(jdata.FirstUsn);
    m_nextUsn   = static_cast<uint64_t>(jdata.NextUsn);
    m_maxSize   = jdata.MaximumSize;

    {
        std::wostringstream ss;
        ss << L"UsnJournalParser: journal on " << volumePath
           << L"  ID=" << m_journalId
           << L"  FirstUSN=" << m_firstUsn
           << L"  NextUSN=" << m_nextUsn
           << L"  MaxSize=" << (m_maxSize >> 20) << L" MB";
        LOG_INFO(ss.str());
    }

    // Only scan the most recent 32 MB of the journal.
    // Older records are rarely useful for recovery and the full journal can be
    // 128 MB+, causing a multi-minute wait on large system drives.
    constexpr uint64_t kMaxScanBytes = 32ULL * 1024 * 1024;
    USN startUsn = jdata.FirstUsn;
    if (m_nextUsn > m_firstUsn)
    {
        uint64_t journalSize = static_cast<uint64_t>(m_nextUsn - m_firstUsn);
        if (journalSize > kMaxScanBytes)
        {
            startUsn = static_cast<USN>(
                m_nextUsn - static_cast<int64_t>(kMaxScanBytes));
            LOG_INFO(L"UsnJournalParser: journal too large ("
                   + std::to_wstring(journalSize >> 20)
                   + L" MB), limiting scan to last 32 MB");
        }
    }

    const uint64_t totalBytes =
        (static_cast<uint64_t>(m_nextUsn) > static_cast<uint64_t>(startUsn))
        ? static_cast<uint64_t>(m_nextUsn - startUsn) : 0;

    // ── Step 3: read all journal records ─────────────────────────────────
    // Map: FRN (low 48 bits) -> accumulated UsnRecord
    std::unordered_map<uint64_t, UsnRecord> recMap;
    recMap.reserve(128 * 1024);

    READ_USN_JOURNAL_DATA readData = {};
    readData.StartUsn          = startUsn;
    readData.ReasonMask        = 0xFFFFFFFF;
    readData.ReturnOnlyOnClose = FALSE;
    readData.Timeout           = 0;
    readData.BytesToWaitFor    = 0;
    readData.UsnJournalID      = jdata.UsnJournalID;

    constexpr DWORD kBufSize = 128 * 1024;
    std::vector<BYTE> buf(kBufSize);

    for (;;)
    {
        // Honour cancellation request from the main thread
        if (cancelFlag && cancelFlag->load()) {
            LOG_INFO(L"UsnJournalParser: cancelled by caller");
            break;
        }

        DWORD bytesRead = 0;
        BOOL ok = DeviceIoControl(
            hVol, FSCTL_READ_USN_JOURNAL,
            &readData, sizeof(readData),
            buf.data(), kBufSize,
            &bytesRead, nullptr);

        if (!ok)
        {
            DWORD gle2 = ::GetLastError();
            if (gle2 == ERROR_HANDLE_EOF || gle2 == ERROR_NO_MORE_FILES)
                break; // normal end-of-journal

            std::wostringstream ss;
            ss << L"FSCTL_READ_USN_JOURNAL failed, error=" << gle2;
            m_lastError = ss.str();
            LOG_WARN(m_lastError);
            break;
        }

        if (bytesRead < sizeof(USN)) break;

        // First 8 bytes = next StartUsn to use
        USN nextUsn = *reinterpret_cast<USN*>(buf.data());
        if (nextUsn == readData.StartUsn) break; // no progress
        readData.StartUsn = nextUsn;

        // Parse records after the 8-byte USN prefix.
        // USN_RECORD is the SDK-compatible common header for V2/V3 records.
        DWORD offset = sizeof(USN);
        while (offset + static_cast<DWORD>(sizeof(USN_RECORD)) <= bytesRead)
        {
            const USN_RECORD* r = reinterpret_cast<const USN_RECORD*>(
                buf.data() + offset);

            if (r->RecordLength < sizeof(USN_RECORD))
                break;
            if (offset + r->RecordLength > bytesRead)
                break;

            // V2 and V3 share the same layout up to FileNameOffset
            if (r->MajorVersion == 2 || r->MajorVersion == 3)
            {
                // Low 48 bits = MFT record number; high 16 bits = sequence
                uint64_t frn  = r->FileReferenceNumber       & 0x0000FFFFFFFFFFFFull;
                uint64_t pfrn = r->ParentFileReferenceNumber & 0x0000FFFFFFFFFFFFull;

                // Decode filename (UTF-16LE, FileNameLength is in bytes)
                std::wstring fname;
                if (r->FileNameLength > 0 &&
                    static_cast<DWORD>(r->FileNameOffset) + r->FileNameLength
                        <= r->RecordLength)
                {
                    fname.assign(r->FileName, r->FileNameLength / sizeof(WCHAR));
                }

                ++m_rawRecords;

                UsnRecord& entry = recMap[frn];
                if (entry.fileFrn == 0)
                    entry.fileFrn = frn;

                // OR all reason flags seen for this FRN
                entry.allReasons |= r->Reason;

                // Keep fields from the NEWEST entry (highest USN value)
                if (r->Usn >= entry.usn)
                {
                    entry.usn            = r->Usn;
                    entry.parentFrn      = pfrn;
                    entry.lastReason     = r->Reason;
                    entry.timestamp      = r->TimeStamp.QuadPart;
                    entry.fileAttributes = r->FileAttributes;
                    if (!fname.empty())
                        entry.name       = fname;
                }
            }

            offset += r->RecordLength;
        }

        // Progress callback
        if (onProgress && totalBytes > 0)
        {
            uint64_t done = static_cast<uint64_t>(nextUsn - jdata.FirstUsn);
            onProgress(done, totalBytes);
        }
    }

    CloseHandle(hVol);

    m_uniqueFrns = static_cast<uint64_t>(recMap.size());

    // Transfer map to output vector (C++14 compatible)
    outRecords.reserve(recMap.size());
    for (auto& it : recMap) {
        // recMap value type is moveable; move into the output vector.
        outRecords.push_back(std::move(it.second));
    }

    {
        std::wostringstream ss;
        ss << L"UsnJournalParser: done"
           << L"  rawRecords=" << m_rawRecords
           << L"  uniqueFRNs=" << m_uniqueFrns;
        LOG_INFO(ss.str());
    }

    return true;
}

#else // !PHOENIX_OS_WINDOWS

bool UsnJournalParser::Scan(const std::wstring& volumePath, std::vector<UsnRecord>& outRecords,
                            std::function<void(uint64_t, uint64_t)> onProgress,
                            const std::atomic<bool>* cancelFlag)
{
    (void)volumePath;
    (void)onProgress;
    (void)cancelFlag;
    m_rawRecords = 0;
    m_uniqueFrns = 0;
    m_journalId  = 0;
    m_firstUsn   = 0;
    m_nextUsn    = 0;
    m_maxSize    = 0;
    m_lastError  = L"USN journal scan is only available on Windows";
    outRecords.clear();
    return false;
}

#endif
