#include "../common/Platform.h"
#if PHOENIX_OS_LINUX_DESKTOP || PHOENIX_OS_MACOS

#include "SectorReader.h"
#include "../log/Logger.h"

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <thread>
#include <chrono>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#if PHOENIX_OS_LINUX_DESKTOP
#include <sys/ioctl.h>
#include <linux/fs.h>      // BLKSSZGET, BLKGETSIZE64
#endif

#if PHOENIX_OS_MACOS
#include <sys/ioctl.h>
#include <sys/disk.h>       // DKIOCGETBLOCKSIZE, DKIOCGETBLOCKCOUNT
#endif

// ─────────────────────────────────────────────────────────────────────────────
SectorReader::SectorReader()
    : m_fd(-1)
    , m_isDevice(false)
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

std::string WideToNarrow(const std::wstring& ws)
{
    std::string out;
    out.reserve(ws.size());
    for (wchar_t wc : ws) {
        if (wc < 0x80)
            out.push_back(static_cast<char>(wc));
        else
            out.push_back('?');
    }
    return out;
}

std::wstring NarrowToWide(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}
} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
void SectorReader::Close()
{
    m_pCancel = nullptr;
    m_virtual.reset();   // A1-2: release the read-only container source first
    if (m_fd != -1)
    {
        ::close(m_fd);
        m_fd = -1;
    }
    m_totalSectors  = 0;
    m_isDevice      = false;
    m_sectorsRead   = 0;
    m_retryCount    = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Open  /dev/sdX (Linux) or /dev/rdiskN (macOS)
// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::Open(int diskIndex)
{
    Close();

#if PHOENIX_OS_LINUX_DESKTOP
    std::string path = "/dev/sd" + std::string(1, 'a' + static_cast<char>(diskIndex));
    int flags = O_RDONLY;
#if defined(O_DIRECT)
    flags |= O_DIRECT;
#endif
#elif PHOENIX_OS_MACOS
    std::string path = "/dev/rdisk" + std::to_string(diskIndex);
    int flags = O_RDONLY;
#endif

    m_fd = ::open(path.c_str(), flags);
    if (m_fd < 0)
    {
        m_lastErrno = errno;
        LOG_ERROR(L"SectorReader::Open failed – "
                + NarrowToWide(path)
                + L"  errno=" + std::to_wstring(m_lastErrno));
        m_fd = -1;
        return false;
    }

    m_isDevice = true;

    bool ok = QueryGeometry();
    LOG_INFO(L"SectorReader::Open ok – " + NarrowToWide(path)
           + L"  sectorSize=" + std::to_wstring(m_sectorSize)
           + L"  totalSectors=" + std::to_wstring(m_totalSectors));
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// OpenImage  –  disk image files
// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::OpenImage(const std::wstring& imagePath)
{
    Close();
    std::string path = WideToNarrow(imagePath);

    m_fd = ::open(path.c_str(), O_RDONLY);
    if (m_fd < 0)
    {
        m_lastErrno = errno;
        LOG_ERROR(L"SectorReader::OpenImage failed: " + imagePath);
        m_fd = -1;
        return false;
    }

    m_isDevice   = false;
    m_sectorSize = 512;

    struct stat st;
    if (fstat(m_fd, &st) == 0)
        m_totalSectors = static_cast<uint64_t>(st.st_size) / m_sectorSize;

    LOG_INFO(L"SectorReader::OpenImage ok – " + imagePath
           + L"  totalSectors=" + std::to_wstring(m_totalSectors));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// OpenVolume  /dev/sdX1 (Linux) or /dev/rdiskNsM (macOS)
// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::OpenVolume(const std::wstring& driveLetter)
{
    Close();
    std::string path = WideToNarrow(driveLetter);

#if PHOENIX_OS_LINUX_DESKTOP
    int flags = O_RDONLY;
#if defined(O_DIRECT)
    flags |= O_DIRECT;
#endif
#elif PHOENIX_OS_MACOS
    int flags = O_RDONLY;
#endif

    m_fd = ::open(path.c_str(), flags);
    if (m_fd < 0)
    {
        m_lastErrno = errno;
        LOG_ERROR(L"SectorReader::OpenVolume failed: " + driveLetter
                + L"  errno=" + std::to_wstring(m_lastErrno));
        m_fd = -1;
        return false;
    }

    m_isDevice = true;

    bool ok = QueryGeometry();
    LOG_INFO(L"SectorReader::OpenVolume ok – " + driveLetter
           + L"  sectorSize=" + std::to_wstring(m_sectorSize)
           + L"  totalSectors=" + std::to_wstring(m_totalSectors));
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::QueryGeometry()
{
    if (!m_isDevice) {
        struct stat st;
        if (fstat(m_fd, &st) == 0) {
            m_sectorSize   = 512;
            m_totalSectors = static_cast<uint64_t>(st.st_size) / m_sectorSize;
        }
        return true;
    }

#if PHOENIX_OS_LINUX_DESKTOP
    // Logical sector size
    unsigned int ssz = 0;
    if (ioctl(m_fd, BLKSSZGET, &ssz) == 0 && ssz > 0)
        m_sectorSize = ssz;
    else
        m_sectorSize = 512;

    // Total size in bytes
    uint64_t totalBytes = 0;
    if (ioctl(m_fd, BLKGETSIZE64, &totalBytes) == 0)
        m_totalSectors = totalBytes / m_sectorSize;
#elif PHOENIX_OS_MACOS
    uint32_t blockSize = 0;
    if (ioctl(m_fd, DKIOCGETBLOCKSIZE, &blockSize) == 0 && blockSize > 0)
        m_sectorSize = blockSize;
    else
        m_sectorSize = 512;

    uint64_t blockCount = 0;
    if (ioctl(m_fd, DKIOCGETBLOCKCOUNT, &blockCount) == 0)
        m_totalSectors = blockCount;
#endif

    if (m_totalSectors == 0) {
        LOG_WARN(L"SectorReader: could not query geometry – using defaults");
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// TryRead  –  single attempt using pread (handles both device and image)
// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::TryRead(uint64_t lba, void* buf, uint32_t sectorCount)
{
    uint64_t byteOffset = lba * m_sectorSize;
    size_t   toRead     = static_cast<size_t>(sectorCount) * m_sectorSize;

    // For O_DIRECT devices, buffer must be aligned to sector size.
    // For image files or macOS (no O_DIRECT), we can read directly.
    void* readBuf = buf;
    void* alignedBuf = nullptr;

#if PHOENIX_OS_LINUX_DESKTOP
    if (m_isDevice) {
        if (posix_memalign(&alignedBuf, m_sectorSize, toRead) != 0) {
            m_lastErrno = errno;
            return false;
        }
        readBuf = alignedBuf;
    }
#endif

    ssize_t result = ::pread(m_fd, readBuf, toRead, static_cast<off_t>(byteOffset));

    if (alignedBuf) {
        if (result > 0)
            memcpy(buf, alignedBuf, static_cast<size_t>(result));
        free(alignedBuf);
    }

    if (result < 0) {
        m_lastErrno = errno;
        return false;
    }

    m_lastErrno = 0;
    return (static_cast<size_t>(result) == toRead);
}

// ─────────────────────────────────────────────────────────────────────────────
// ReadSectors  –  public API with retry + bad-sector handling
// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::ReadSectors(uint64_t sectorIndex, void* buffer,
                                uint32_t sectorCount)
{
    // A1-2: container images have no file descriptor in the native path;
    // delegate to the virtual sector source before any native fd check.
    if (m_virtual) return ReadVirtualSectors(sectorIndex, buffer, sectorCount);
    if (m_fd == -1) return false;
    if (sectorCount == 0) return true;

    if (m_pCancel && m_pCancel->load()) {
        m_lastErrno = ECANCELED;
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
    LOG_DEBUG(L"SectorReader: bulk read failed at LBA="
            + std::to_wstring(sectorIndex)
            + L"  count=" + std::to_wstring(sectorCount)
            + L"  errno=" + std::to_wstring(m_lastErrno)
            + L"  → falling back to per-sector retry");

    bool anyBad = false;

    for (uint32_t i = 0; i < sectorCount; ++i)
    {
        if (m_pCancel && m_pCancel->load()) {
            m_lastErrno = ECANCELED;
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
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(m_policy.retryDelayMs));

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
            anyBad = true;
            m_badSectors.MarkBad(lba);

            {
                std::wostringstream ss;
                ss << L"SectorReader: BAD SECTOR  LBA=" << lba
                   << L"  errno=" << m_lastErrno
                   << L"  total_bad=" << m_badSectors.Count();
                LOG_WARN(ss.str());
            }

            if (!m_policy.skipBadSectors)
                return false;

            memset(sector, 0, m_sectorSize);
            ++m_sectorsRead;
            notifySectorProgress(m_onSectorRead, m_sectorsRead, m_totalSectors);
        }
    }

    return m_policy.skipBadSectors || !anyBad;
}

// ─────────────────────────────────────────────────────────────────────────────
bool SectorReader::ReadBytes(uint64_t byteOffset, void* buffer, uint32_t length)
{
    if (!IsOpen()) return false;   // true for virtual images too (A1-2)
    if (length == 0) return true;

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

#endif // PHOENIX_OS_LINUX_DESKTOP || PHOENIX_OS_MACOS
