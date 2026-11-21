#include "../common/Platform.h"
#if PHOENIX_OS_WINDOWS

// CancelIoEx() (used by Close/TryReadOverlapped) requires Vista+. MSVC's SDK
// defaults high enough; only MinGW 7.3 needs the target version stated
// explicitly. Scoped to MinGW so the MSVC build keeps its normal SDK target.
#if defined(__MINGW32__) && !defined(_WIN32_WINNT)
#define _WIN32_WINNT 0x0600
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>

#include "SectorReader.h"
#include "../log/Logger.h"

#include <cassert>
#include <cstring>
#include <sstream>

// ─────────────────────────────────────────────────────────────────────────────
SectorReader::SectorReader()
    : m_hDevice(INVALID_HANDLE_VALUE)
    , m_ioEvent(nullptr)
    , m_useOverlapped(false)
{}

SectorReader::~SectorReader()
{
    Close();
}

namespace {
void notifySectorProgress(const SectorReader::SectorReadCallback &cb,
                          uint64_t sectorsRead,
                          uint64_t totalSectors)
{
    if (cb) {
        cb(sectorsRead, totalSectors);
    }
}
}

// ─────────────────────────────────────────────────────────────────────────────
void SectorReader::Close()
{
    m_pCancel = nullptr;
    m_virtual.reset();   // A1-2: release the read-only container source first
    if (m_hDevice != INVALID_HANDLE_VALUE)
    {
        // Cancel any pending I/O before closing
        CancelIoEx(m_hDevice, nullptr);
        CloseHandle(m_hDevice);
        m_hDevice = INVALID_HANDLE_VALUE;
    }
    if (m_ioEvent)
    {
        CloseHandle(m_ioEvent);
        m_ioEvent = nullptr;
    }
    m_totalSectors   = 0;
    m_imageFileSize  = 0;
    m_imagePath.clear();
    m_useOverlapped  = false;
    m_sectorsRead    = 0;
    m_retryCount     = 0;
    // A1-12: never let a window filled from a previous device/image satisfy
    // a read on a newly opened source.
    m_readAheadBase  = 0;
    m_readAheadLen   = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Open  \\.\PhysicalDriveN
// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::Open(int diskIndex)
{
    Close();
    std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(diskIndex);

    // FILE_FLAG_OVERLAPPED enables async reads with per-sector timeout
    m_hDevice = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (m_hDevice == INVALID_HANDLE_VALUE)
    {
        m_lastWinError = ::GetLastError();
        LOG_ERROR(L"SectorReader::Open failed – PhysicalDrive"
                + std::to_wstring(diskIndex)
                + L"  err=" + std::to_wstring(m_lastWinError));
        return false;
    }

    // Create the I/O completion event (manual reset)
    m_ioEvent       = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_useOverlapped = true;

    bool ok = QueryGeometry();
    LOG_INFO(L"SectorReader::Open ok – PhysicalDrive" + std::to_wstring(diskIndex)
           + L"  sectorSize=" + std::to_wstring(m_sectorSize)
           + L"  totalSectors=" + std::to_wstring(m_totalSectors)
           + L"  overlapped=true");
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// OpenImage  –  disk image files  (no OVERLAPPED, no timeout needed)
// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::OpenImage(const std::wstring& imagePath)
{
    Close();
    m_hDevice = CreateFileW(
        imagePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);

    if (m_hDevice == INVALID_HANDLE_VALUE)
    {
        m_lastWinError = ::GetLastError();
        LOG_ERROR(L"SectorReader::OpenImage failed: " + imagePath);
        return false;
    }

    // Image files are synchronous – no OVERLAPPED needed
    m_useOverlapped = false;
    m_sectorSize    = 512;
    LARGE_INTEGER sz{};
    GetFileSizeEx(m_hDevice, &sz);
    m_imageFileSize  = static_cast<uint64_t>(sz.QuadPart);
    m_totalSectors   = m_imageFileSize / m_sectorSize;
    m_imagePath      = imagePath;

    LOG_INFO(L"SectorReader::OpenImage ok – " + imagePath
           + L"  bytes=" + std::to_wstring(m_imageFileSize)
           + L"  totalSectors=" + std::to_wstring(m_totalSectors));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// OpenVolume  \\.\C:
// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::OpenVolume(const std::wstring& driveLetter)
{
    Close();
    std::wstring path = L"\\\\.\\" + driveLetter;
    if (!path.empty() && path.back() == L'\\') path.pop_back();

    m_hDevice = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED,
        nullptr);

    if (m_hDevice == INVALID_HANDLE_VALUE)
    {
        m_lastWinError = ::GetLastError();
        LOG_ERROR(L"SectorReader::OpenVolume failed: " + driveLetter
                + L"  err=" + std::to_wstring(m_lastWinError));
        return false;
    }

    m_ioEvent       = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_useOverlapped = true;

    bool ok = QueryGeometry();
    LOG_INFO(L"SectorReader::OpenVolume ok – " + driveLetter
           + L"  sectorSize=" + std::to_wstring(m_sectorSize)
           + L"  totalSectors=" + std::to_wstring(m_totalSectors)
           + L"  overlapped=true");
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::QueryGeometry()
{
    DWORD returned = 0;

    DISK_GEOMETRY_EX geoEx{};
    if (DeviceIoControl(m_hDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                        nullptr, 0, &geoEx, sizeof(geoEx), &returned, nullptr))
    {
        m_sectorSize   = geoEx.Geometry.BytesPerSector;
        m_totalSectors = static_cast<uint64_t>(geoEx.DiskSize.QuadPart) / m_sectorSize;
        return true;
    }

    DISK_GEOMETRY geo{};
    if (DeviceIoControl(m_hDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                        nullptr, 0, &geo, sizeof(geo), &returned, nullptr))
    {
        m_sectorSize   = geo.BytesPerSector;
        m_totalSectors = static_cast<uint64_t>(geo.Cylinders.QuadPart)
                       * geo.TracksPerCylinder * geo.SectorsPerTrack;
        return true;
    }

    LARGE_INTEGER sz{};
    if (GetFileSizeEx(m_hDevice, &sz))
    {
        m_sectorSize   = 512;
        m_totalSectors = static_cast<uint64_t>(sz.QuadPart) / m_sectorSize;
        return true;
    }

    m_lastWinError = ::GetLastError();
    LOG_WARN(L"SectorReader: could not query geometry – using defaults");
    return true; // Not fatal; reads will reveal real errors
}

// ─────────────────────────────────────────────────────────────────────────────
// TryReadOverlapped  –  single attempt with timeout (for physical disks)
// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::TryReadOverlapped(uint64_t lba, void* buf,
                                      uint32_t sectorCount)
{
    assert(m_ioEvent != nullptr);
    ResetEvent(m_ioEvent);

    uint64_t byteOffset = lba * m_sectorSize;
    DWORD    toRead     = sectorCount * m_sectorSize;

    OVERLAPPED ov     = {};
    ov.hEvent         = m_ioEvent;
    ov.Offset         = static_cast<DWORD>(byteOffset & 0xFFFFFFFFull);
    ov.OffsetHigh     = static_cast<DWORD>(byteOffset >> 32);

    DWORD bytesRead = 0;
    BOOL  ok        = ReadFile(m_hDevice, buf, toRead, nullptr, &ov);
    DWORD err       = ::GetLastError();

    if (!ok && err != ERROR_IO_PENDING)
    {
        m_lastWinError = err;
        return false;
    }

    // ── Wait with timeout ─────────────────────────────────────────────────
    DWORD timeout = (m_policy.timeoutMs > 0) ? m_policy.timeoutMs : INFINITE;
    DWORD wait    = WaitForSingleObject(m_ioEvent, timeout);

    if (wait == WAIT_TIMEOUT)
    {
        // Cancel the pending I/O and drain the event
        CancelIoEx(m_hDevice, &ov);
        // Wait (briefly) for cancellation to complete so the OVERLAPPED is safe
        DWORD dummy = 0;
        GetOverlappedResult(m_hDevice, &ov, &dummy, TRUE /*wait*/);
        m_lastWinError = ERROR_SEM_TIMEOUT;
        return false;
    }

    if (wait != WAIT_OBJECT_0)
    {
        m_lastWinError = ::GetLastError();
        return false;
    }

    ok = GetOverlappedResult(m_hDevice, &ov, &bytesRead, FALSE);
    if (!ok)
    {
        m_lastWinError = ::GetLastError();
        return false;
    }

    m_lastWinError = ERROR_SUCCESS;
    return (bytesRead == toRead);
}

// ─────────────────────────────────────────────────────────────────────────────
// TryReadSync  –  synchronous read (for image files)
// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::TryReadSync(uint64_t lba, void* buf, uint32_t sectorCount)
{
    const uint64_t byteOff = lba * m_sectorSize;
    if (m_imageFileSize > 0 && byteOff >= m_imageFileSize)
        return false;

    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(byteOff);

    if (!SetFilePointerEx(m_hDevice, li, nullptr, FILE_BEGIN))
    {
        m_lastWinError = ::GetLastError();
        return false;
    }

    const uint32_t fullBytes = sectorCount * m_sectorSize;
    uint32_t toRead = fullBytes;
    if (m_imageFileSize > 0) {
        const uint64_t remain = m_imageFileSize - byteOff;
        if (remain < toRead)
            toRead = static_cast<uint32_t>(remain);
    }

    DWORD bytesRead = 0;
    BOOL  ok        = ReadFile(m_hDevice, buf, toRead, &bytesRead, nullptr);
    if (!ok)
    {
        m_lastWinError = ::GetLastError();
        LOG_DEBUG(L"SectorReader::TryReadSync ReadFile failed"
                  L" lba=" + std::to_wstring(lba) +
                  L" sectors=" + std::to_wstring(sectorCount) +
                  L" bytes=" + std::to_wstring(toRead) +
                  L" err=" + std::to_wstring(m_lastWinError));
        return false;
    }

    // A sector read is only successful when the complete requested range was
    // obtained.  Treating a short read as success can make a truncated image
    // look like a valid filesystem and, more importantly, can hide the exact
    // point at which recovery data became unavailable.  Zero-fill only after
    // the caller has explicitly opted into bad-sector continuation in
    // ReadSectors().
    if (bytesRead != fullBytes)
    {
        m_lastWinError = (bytesRead == 0) ? ERROR_HANDLE_EOF : ERROR_PARTIAL_COPY;
        LOG_DEBUG(L"SectorReader::TryReadSync short read"
                  L" lba=" + std::to_wstring(lba) +
                  L" sectors=" + std::to_wstring(sectorCount) +
                  L" requested=" + std::to_wstring(fullBytes) +
                  L" actual=" + std::to_wstring(bytesRead) +
                  L" err=" + std::to_wstring(m_lastWinError));
        return false;
    }

    m_lastWinError = ERROR_SUCCESS;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// TryRead  –  dispatch to overlapped or sync
// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::TryRead(uint64_t lba, void* buf, uint32_t sectorCount)
{
    return m_useOverlapped
         ? TryReadOverlapped(lba, buf, sectorCount)
         : TryReadSync      (lba, buf, sectorCount);
}

// ─────────────────────────────────────────────────────────────────────────────
// ReadSectors  –  public API with retry + bad-sector handling
// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::ReadSectors(uint64_t sectorIndex, void* buffer,
                                uint32_t sectorCount)
{
    // A1-2: container images have no device handle; delegate to the virtual
    // sector source before any native handle check.
    if (m_virtual) return ReadVirtualSectors(sectorIndex, buffer, sectorCount);
    if (m_hDevice == INVALID_HANDLE_VALUE) return false;
    if (sectorCount == 0) return true;

    if (m_pCancel && m_pCancel->load()) {
        m_lastWinError = ERROR_OPERATION_ABORTED;
        return false;
    }

    uint8_t* buf = static_cast<uint8_t*>(buffer);

    // ── Fast path: try reading the whole range at once ────────────────────
    if (TryRead(sectorIndex, buf, sectorCount))
    {
        m_sectorsRead += sectorCount;
        notifySectorProgress(m_onSectorRead, m_sectorsRead, m_totalSectors);
        return true;
    }

    // ── Slow path: sector-by-sector with retry ────────────────────────────
    // Only triggered when the bulk read fails (saves overhead for healthy disks).
    LOG_DEBUG(L"SectorReader: bulk read failed at LBA="
            + std::to_wstring(sectorIndex)
            + L"  count=" + std::to_wstring(sectorCount)
            + L"  err=" + std::to_wstring(m_lastWinError)
            + L"  → falling back to per-sector retry");

    bool anyBad = false;

    for (uint32_t i = 0; i < sectorCount; ++i)
    {
        if (m_pCancel && m_pCancel->load()) {
            m_lastWinError = ERROR_OPERATION_ABORTED;
            return false;
        }
        uint64_t  lba    = sectorIndex + i;
        uint8_t*  sector = buf + static_cast<size_t>(i) * m_sectorSize;
        bool      ok     = false;

        for (uint32_t attempt = 0; attempt <= m_policy.maxRetries; ++attempt)
        {
            if (attempt > 0)
            {
                ++m_retryCount;
                if (m_policy.retryDelayMs > 0)
                    Sleep(m_policy.retryDelayMs);

                LOG_DEBUG(L"SectorReader: retry " + std::to_wstring(attempt)
                        + L"/" + std::to_wstring(m_policy.maxRetries)
                        + L"  LBA=" + std::to_wstring(lba));
            }

            if (TryRead(lba, sector, 1))
            {
                ok = true;
                break;
            }
        }

        if (ok)
        {
            ++m_sectorsRead;
            notifySectorProgress(m_onSectorRead, m_sectorsRead, m_totalSectors);
        }
        else
        {
            // ── Unrecoverable sector ──────────────────────────────────────
            anyBad = true;
            m_badSectors.MarkBad(lba);

            {
                std::wostringstream ss;
                ss << L"SectorReader: BAD SECTOR  LBA=" << lba
                   << L"  err=" << m_lastWinError
                   << L"  total_bad=" << m_badSectors.Count();
                LOG_WARN(ss.str());
            }

            if (!m_policy.skipBadSectors)
                return false; // Strict mode: abort on first bad sector

            // Lenient mode: zero-fill and continue
            memset(sector, 0, m_sectorSize);
            ++m_sectorsRead; // Count it as "processed"
            notifySectorProgress(m_onSectorRead, m_sectorsRead, m_totalSectors);
        }
    }

    // In skipBadSectors mode we always return true
    // (caller checks GetBadSectors() for details)
    return m_policy.skipBadSectors || !anyBad;
}

// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::ReadBytes(uint64_t byteOffset, void* buffer, uint32_t length)
{
    if (!IsOpen()) return false;   // true for virtual images too (A1-2)
    if (length == 0) return true;

    // A1-12: opt-in speculative read-ahead. Pure cache semantics: on a hit we
    // copy from a window filled by ONE large TryRead; on a miss we attempt a
    // single window fill; on ANY fill failure we fall through to the original
    // path so bad-sector retries, statistics and progress are never altered.
    // m_readAheadWindow == 0 (default) bypasses this block entirely.
    if (m_readAheadWindow != 0 && m_readAheadPtr != nullptr && buffer != nullptr &&
        length <= m_readAheadWindow / 2)
    {
        const uint64_t end = byteOffset + length;

        if (m_readAheadLen != 0 &&
            byteOffset >= m_readAheadBase &&
            end <= m_readAheadBase + m_readAheadLen)
        {
            std::memcpy(buffer,
                        m_readAheadPtr + static_cast<size_t>(byteOffset - m_readAheadBase),
                        length);
            ++m_readAheadHits;
            return true;
        }

        const uint64_t base    = (byteOffset / m_sectorSize) * m_sectorSize;
        const uint64_t baseLBA = base / m_sectorSize;
        uint32_t sectors = m_readAheadWindow / m_sectorSize;
        if (m_totalSectors != 0) {
            if (baseLBA >= m_totalSectors)
                sectors = 0;
            else if (baseLBA + sectors > m_totalSectors)
                sectors = static_cast<uint32_t>(m_totalSectors - baseLBA);
        }

        if (sectors != 0 && TryRead(baseLBA, m_readAheadPtr, sectors))
        {
            m_readAheadBase = base;
            m_readAheadLen  = static_cast<size_t>(sectors) * m_sectorSize;
            ++m_readAheadFills;
            if (byteOffset >= m_readAheadBase &&
                end <= m_readAheadBase + m_readAheadLen)
            {
                std::memcpy(buffer,
                            m_readAheadPtr + static_cast<size_t>(byteOffset - m_readAheadBase),
                            length);
                return true;
            }
        }

        // Transparent fallback: a failing/short speculative read must never
        // change observable behaviour (retries, BadSectorMap, statistics).
        m_readAheadLen = 0;
    }

    uint64_t alignedOffset  = (byteOffset / m_sectorSize) * m_sectorSize;
    uint32_t offsetInSector = static_cast<uint32_t>(byteOffset - alignedOffset);
    uint32_t sectorsNeeded  = (offsetInSector + length + m_sectorSize - 1) / m_sectorSize;

    std::vector<uint8_t> sectorBuf(
        static_cast<size_t>(sectorsNeeded) * m_sectorSize);

    if (!ReadSectors(alignedOffset / m_sectorSize, sectorBuf.data(), sectorsNeeded))
        return false;

    memcpy(buffer, sectorBuf.data() + offsetInSector, length);
    return true;
}

#endif // PHOENIX_OS_WINDOWS
