#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../common/Platform.h"
#include "BadSectorMap.h"
#include "VirtualDiskSectorSource.h"

#if PHOENIX_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

// ---------------------------------------------------------------------------
// ReadPolicy
//
// Controls how SectorReader behaves when a read fails.
//
//   maxRetries     – How many times to retry a single failing sector.
//                    Each retry is a fresh ReadFile call.
//   timeoutMs      – Maximum time (in ms) to wait for one sector read.
//                    0 means no timeout (synchronous / blocking).
//   retryDelayMs   – Time to sleep between consecutive retries (ms).
//   skipBadSectors – If true (default), bad sectors are zero-filled and
//                    execution continues; ReadSectors() still returns true
//                    and the caller inspects GetBadSectors() afterwards.
//                    If false, ReadSectors() returns false on first bad sector.
// ---------------------------------------------------------------------------
struct ReadPolicy
{
    uint32_t maxRetries     = 3;
    uint32_t timeoutMs      = 8'000;  // 8 s per sector attempt
    uint32_t retryDelayMs   = 50;     // 50 ms between retries
    bool     skipBadSectors = true;
};

// ---------------------------------------------------------------------------
// SectorReader
//
// Read-only interface to a physical disk, logical volume, or disk image.
//
// On Windows the handle is opened with FILE_FLAG_OVERLAPPED so that every
// ReadFile can be bounded by a timeout (see ReadPolicy).  On Unix platforms
// POSIX APIs (open/pread/close) are used instead.
//
// When a sector is unreadable after all retries it is zero-filled, marked in
// the BadSectorMap, and scanning continues – unless skipBadSectors = false.
// ---------------------------------------------------------------------------
class SectorReader
{
public:
    using SectorReadCallback = std::function<void(uint64_t sectorsRead, uint64_t totalSectors)>;

    SectorReader();
    ~SectorReader();

    SectorReader(const SectorReader&)            = delete;
    SectorReader& operator=(const SectorReader&) = delete;

    // ── Open ──────────────────────────────────────────────────────────────

    // Open \\.\PhysicalDriveN (Windows) or /dev/sdX (Linux) or /dev/rdiskN (macOS)
    bool Open(int diskIndex);

    // Open a disk image (.img / .dd). Plain raw images only: preserves the
    // exact file length, including a partial trailing sector.
    bool OpenImage(const std::wstring& imagePath);

    // A1-2: open a *container* image (VHD/VHDX/VMDK/QCOW2/VDI) as a read-only
    // scan source. Virtual LBAs are translated by VirtualDiskSource. Rejects
    // plain raw images (use OpenImage) and every unsupported container
    // variant; `outDetail` receives the parser's reason / warnings.
    // This path never opens a device and never requests write access.
    bool OpenVirtualImage(const std::wstring& imagePath,
                          std::string* outDetail = nullptr);

    bool IsVirtualImage() const { return m_virtual != nullptr; }
    const phoenix::VirtualDiskInfo& GetVirtualDiskInfo() const;

    // Open a logical volume by drive letter (Windows: L"D:") or mount path
    bool OpenVolume(const std::wstring& driveLetter);

    void Close();

    // ── Read ──────────────────────────────────────────────────────────────

    // Read 'sectorCount' consecutive sectors starting at 'sectorIndex'.
    // 'buffer' must be at least sectorCount * GetSectorSize() bytes.
    //
    // With skipBadSectors = true (default):
    //   • Always returns true (unreadable sectors are zero-filled).
    //   • Check GetBadSectors().Count() > 0 afterwards.
    // With skipBadSectors = false:
    //   • Returns false immediately on the first unrecoverable sector.
    bool ReadSectors(uint64_t sectorIndex, void* buffer,
                     uint32_t sectorCount = 1);

    // Read arbitrary bytes; internally aligns to sectors.
    bool ReadBytes(uint64_t byteOffset, void* buffer, uint32_t length);

    // ── Opt-in speculative read-ahead (A1-12 / real-disk batch recovery) ───
    //
    // 0 (default) = DISABLED: ReadBytes() is byte-for-byte and behaviourally
    // identical to the original implementation (including retries, bad-sector
    // marking, statistics and progress callbacks).
    //
    // > 0 = a sliding, sector-aligned window of that size (rounded down to a
    // 4 KiB multiple). Small ReadBytes() calls are served from the window,
    // which is filled by ONE large TryRead. If the fill fails for any reason
    // (bad sector, short read at end of disk, device error) the call
    // transparently falls through to the original path, so bad-sector and
    // retry semantics are never weakened. Intended for the headless tool on
    // healthy sequential workloads; GUI and all CTests keep the default.
    void     SetReadAheadWindow(uint32_t bytes);
    uint32_t GetReadAheadWindow() const { return m_readAheadWindow; }
    uint64_t GetReadAheadHits()  const { return m_readAheadHits;  }
    uint64_t GetReadAheadFills() const { return m_readAheadFills; }

    // ── Policy & stats ────────────────────────────────────────────────────

    void              SetReadPolicy(const ReadPolicy& p) { m_policy = p; }
    const ReadPolicy& GetReadPolicy()                    const { return m_policy; }

    const BadSectorMap& GetBadSectors()     const { return m_badSectors; }
    void                ClearBadSectors()         { m_badSectors.Clear(); }
    uint64_t            GetBadSectorCount() const { return m_badSectors.Count(); }

    // Total sectors successfully read (including zero-filled bad ones)
    uint64_t GetSectorsRead()  const { return m_sectorsRead;  }
    // Total retries performed across all reads
    uint64_t GetRetryCount()   const { return m_retryCount;   }

    // ── Device info ───────────────────────────────────────────────────────

#if PHOENIX_OS_WINDOWS
    bool     IsOpen()          const {
        return m_hDevice != INVALID_HANDLE_VALUE || m_virtual != nullptr;
    }
    DWORD    GetLastWinError() const { return m_lastWinError; }
#else
    bool     IsOpen()          const {
        return m_fd != -1 || m_virtual != nullptr;
    }
    int      GetLastErrno()    const { return m_lastErrno; }
#endif

    uint32_t GetSectorSize()   const { return m_sectorSize;   }
    uint64_t GetTotalSectors() const { return m_totalSectors; }
    // For disk images, exact file byte length (may include a partial final sector).
    uint64_t GetTotalBytes()   const {
        return m_imageFileSize > 0 ? m_imageFileSize : m_totalSectors * m_sectorSize;
    }

    // Path of the image file last opened via OpenImage().  Empty for device /
    // volume / virtual-disk sources.  Used to spin up per-thread readers for
    // parallel RAW scanning without sharing a single handle's mutable state.
    const std::wstring& GetImagePath() const { return m_imagePath; }

    void SetSectorReadCallback(SectorReadCallback cb) { m_onSectorRead = std::move(cb); }

    void SetCancelFlag(std::atomic<bool> *flag) { m_pCancel = flag; }

private:
    bool QueryGeometry();

    // A1-2 virtual-image read path. Mirrors the native ReadSectors contract
    // (cancel check, bulk fast path, per-sector retry, zero-fill + BadSectorMap
    // on unreadable sectors, progress callback) without touching any handle.
    bool ReadVirtualSectors(uint64_t sectorIndex, void* buffer,
                            uint32_t sectorCount);
    void NotifyVirtualProgress();

#if PHOENIX_OS_WINDOWS
    bool TryRead(uint64_t lba, void* buf, uint32_t sectorCount);
    bool TryReadOverlapped(uint64_t lba, void* buf, uint32_t sectorCount);
    bool TryReadSync(uint64_t lba, void* buf, uint32_t sectorCount);

    // ── Handles (Windows) ─────────────────────────────────────────────────
    HANDLE   m_hDevice      = INVALID_HANDLE_VALUE;
    HANDLE   m_ioEvent      = nullptr;
    bool     m_useOverlapped = false;
    DWORD    m_lastWinError = 0;
#else
    bool TryRead(uint64_t lba, void* buf, uint32_t sectorCount);

    // ── File descriptor (Unix) ────────────────────────────────────────────
    int      m_fd           = -1;
    bool     m_isDevice     = false;  // true for block devices, false for image files
    int      m_lastErrno    = 0;
#endif

    // ── Geometry (shared) ─────────────────────────────────────────────────
    uint32_t m_sectorSize   = 512;
    uint64_t m_totalSectors = 0;
    uint64_t m_imageFileSize = 0; // non-zero for .dd/.img (exact file length)
    std::wstring m_imagePath;     // set by OpenImage(); enables per-thread cloning

    // ── Configuration ─────────────────────────────────────────────────────
    ReadPolicy m_policy;

    // ── Statistics ────────────────────────────────────────────────────────
    uint64_t m_sectorsRead = 0;
    uint64_t m_retryCount  = 0;

    // ── Opt-in read-ahead window (disabled by default) ────────────────────
    // Storage has 4096 bytes of slack so the aligned pointer handed to
    // FILE_FLAG_NO_BUFFERING reads is guaranteed 4096-byte aligned.
    uint32_t m_readAheadWindow = 0;
    uint64_t m_readAheadBase   = 0;   // byte offset of window start
    size_t   m_readAheadLen    = 0;   // valid bytes in window
    uint64_t m_readAheadHits   = 0;
    uint64_t m_readAheadFills  = 0;
    std::vector<uint8_t> m_readAheadStorage;
    uint8_t* m_readAheadPtr = nullptr;

    // ── Bad sector tracking ───────────────────────────────────────────────
    BadSectorMap m_badSectors;
    SectorReadCallback m_onSectorRead;
    std::atomic<bool> *m_pCancel = nullptr;

    // Non-null only when opened through OpenVirtualImage(). Lifetime owner of
    // both the read-only byte reader and the container parser.
    std::unique_ptr<phoenix::VirtualDiskSectorSource> m_virtual;
};

// ---------------------------------------------------------------------------
// A1-2 inline implementation (header-only: no new translation unit, no CMake
// source-list change). Kept out of the class body for readability.
// ---------------------------------------------------------------------------

inline const phoenix::VirtualDiskInfo& SectorReader::GetVirtualDiskInfo() const
{
    static const phoenix::VirtualDiskInfo kEmpty{};
    return m_virtual ? m_virtual->Info() : kEmpty;
}

inline void SectorReader::SetReadAheadWindow(uint32_t bytes)
{
    // The window must be a whole number of sectors AND of 4096-byte pages so
    // that FILE_FLAG_NO_BUFFERING reads are always correctly aligned.
    m_readAheadWindow = (bytes / 4096u) * 4096u;
    m_readAheadBase   = 0;
    m_readAheadLen    = 0;
    m_readAheadHits   = 0;
    m_readAheadFills  = 0;
    m_readAheadPtr    = nullptr;
    m_readAheadStorage.clear();
    if (m_readAheadWindow == 0) return;

    m_readAheadStorage.resize(static_cast<size_t>(m_readAheadWindow) + 4096u);
    std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(m_readAheadStorage.data());
    std::uintptr_t aligned = (raw + 4095u) & ~static_cast<std::uintptr_t>(4095u);
    m_readAheadPtr = reinterpret_cast<uint8_t*>(aligned);
}

inline bool SectorReader::OpenVirtualImage(const std::wstring& imagePath,
                                          std::string* outDetail)
{
    Close();
    auto source = std::make_unique<phoenix::VirtualDiskSectorSource>();
    std::string detail;
    if (!source->Open(imagePath, &detail)) {
        if (outDetail) *outDetail = detail;
        return false;
    }
    // A raw image has no container metadata. Routing it through the virtual
    // path would round its size down to whole sectors and silently drop a
    // partial trailing sector, so refuse it and let the caller use OpenImage.
    if (source->Format() == phoenix::VirtualDiskFormat::Raw) {
        if (outDetail) {
            *outDetail = "plain raw image detected; use OpenImage() instead "
                         "of the virtual-disk path";
        }
        return false;
    }
    const uint64_t sectors = source->SectorCount();
    if (sectors == 0) {
        if (outDetail) *outDetail = "virtual disk has zero sectors";
        return false;
    }

    m_sectorSize    = 512;   // VirtualDiskSource exposes 512-byte sectors only
    m_totalSectors  = sectors;
    m_imageFileSize = 0;     // GetTotalBytes() falls back to sectors * 512
    m_virtual       = std::move(source);
    if (outDetail) *outDetail = detail;
    return true;
}

inline void SectorReader::NotifyVirtualProgress()
{
    if (m_onSectorRead) {
        m_onSectorRead(m_sectorsRead, m_totalSectors);
    }
}

inline bool SectorReader::ReadVirtualSectors(uint64_t sectorIndex, void* buffer,
                                             uint32_t sectorCount)
{
    if (!m_virtual || buffer == nullptr) return false;
    if (sectorCount == 0) return true;
    if (m_pCancel && m_pCancel->load()) return false;

    uint8_t* buf = static_cast<uint8_t*>(buffer);

    // Fast path: the parser handles the whole range in one call.
    if (m_virtual->ReadSectors(sectorIndex, buf, sectorCount)) {
        m_sectorsRead += sectorCount;
        NotifyVirtualProgress();
        return true;
    }

    // Slow path: isolate the failing sector(s), retry, then zero-fill.
    bool anyBad = false;
    for (uint32_t i = 0; i < sectorCount; ++i) {
        if (m_pCancel && m_pCancel->load()) return false;
        const uint64_t lba = sectorIndex + i;
        uint8_t* sector = buf + static_cast<size_t>(i) * m_sectorSize;
        bool ok = false;
        for (uint32_t attempt = 0; attempt <= m_policy.maxRetries; ++attempt) {
            if (attempt > 0) ++m_retryCount;
            if (m_virtual->ReadSectors(lba, sector, 1)) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            anyBad = true;
            m_badSectors.MarkBad(lba);
            if (!m_policy.skipBadSectors) return false;
            std::memset(sector, 0, m_sectorSize);
        }
        ++m_sectorsRead;
        NotifyVirtualProgress();
    }
    return m_policy.skipBadSectors || !anyBad;
}
