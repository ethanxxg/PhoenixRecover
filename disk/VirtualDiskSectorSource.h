#pragma once

// ---------------------------------------------------------------------------
// VirtualDiskSectorSource -- A1-2 bridge between VirtualDiskSource and
// SectorReader.
//
// VirtualDiskSource (A1-1) translates a virtual LBA into a physical file
// offset, but the whole scan pipeline (PartitionProbe, UnifiedVolumeScanner,
// RecoverEngine, previews) consumes `SectorReader&`. This header supplies the
// missing piece: a platform-correct, STRICTLY READ-ONLY byte source plus a
// small facade that SectorReader can hold by value/unique_ptr and delegate
// ReadSectors() to.
//
//   * Windows: CreateFileW(GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING).
//     No GENERIC_WRITE, no device path, no write API is ever called. Wide
//     paths are preserved (no ANSI/UTF-8 folding), so non-ASCII file names
//     work. FILE_SHARE_READ (without SHARE_WRITE) prevents another process
//     from mutating the image while it is a scan source (S1).
//   * Unix: std::ifstream in std::ios::in | std::ios::binary, via the existing
//     FileVirtualDiskReader.
//   * VirtualDiskSource::Open() owns all container parsing; unsupported
//     variants are surfaced verbatim through `detail` instead of being
//     reinterpreted as a raw disk.
//
// Header-only on purpose: no new .cpp, no CMake source-list change. It does
// include <Windows.h> on Windows, so include it only where SectorReader's
// platform types are already acceptable.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../common/Platform.h"

#if PHOENIX_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

#include "VirtualDiskSource.h"

namespace phoenix {

#if PHOENIX_OS_WINDOWS

// Read-only Win32 file reader implementing the VirtualDiskSource byte-source
// contract. Never opens a device, never requests write access.
class Win32ReadOnlyVirtualDiskReader : public IVirtualDiskReader
{
public:
    Win32ReadOnlyVirtualDiskReader() = default;
    ~Win32ReadOnlyVirtualDiskReader() override { Close(); }

    Win32ReadOnlyVirtualDiskReader(const Win32ReadOnlyVirtualDiskReader&) = delete;
    Win32ReadOnlyVirtualDiskReader& operator=(const Win32ReadOnlyVirtualDiskReader&) = delete;

    bool Open(const std::wstring& path)
    {
        Close();
        if (path.empty()) return false;
        HANDLE h = ::CreateFileW(path.c_str(),
                                 GENERIC_READ,
                                 FILE_SHARE_READ,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
                                 nullptr);
        if (h == INVALID_HANDLE_VALUE || h == nullptr) return false;
        LARGE_INTEGER sz = {};
        if (!::GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
            ::CloseHandle(h);
            return false;
        }
        m_handle = h;
        m_size   = static_cast<uint64_t>(sz.QuadPart);
        return true;
    }

    bool IsOpen() const { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }
    void Close()
    {
        if (IsOpen()) ::CloseHandle(m_handle);
        m_handle = nullptr;
        m_size   = 0;
    }

    uint64_t Size() const override { return m_size; }

    bool Read(uint64_t offset, void* dst, size_t length) const override
    {
        if (length == 0) return true;
        if (!IsOpen() || dst == nullptr || offset > m_size ||
            static_cast<uint64_t>(length) > m_size - offset) {
            return false;
        }
        LARGE_INTEGER pos = {};
        pos.QuadPart = static_cast<LONGLONG>(offset);
        if (!::SetFilePointerEx(m_handle, pos, nullptr, FILE_BEGIN)) return false;

        uint8_t* out = static_cast<uint8_t*>(dst);
        size_t   left = length;
        while (left > 0) {
            const size_t step = left > 0xFFFFFFFFULL ? 0xFFFFFFFFULL : left;
            const DWORD chunk = static_cast<DWORD>(step);
            DWORD done = 0;
            if (!::ReadFile(m_handle, out, chunk, &done, nullptr) || done != chunk) {
                return false;
            }
            out  += chunk;
            left -= chunk;
        }
        return true;
    }

private:
    HANDLE   m_handle = nullptr;
    uint64_t m_size   = 0;
};

#endif // PHOENIX_OS_WINDOWS


// ---------------------------------------------------------------------------
// VirtualDiskSectorSource
//
// Owns the byte reader and the parser together (VirtualDiskSource keeps a
// non-owning pointer to the reader, so both must share a lifetime). Provides
// the tiny surface SectorReader needs: open, geometry, info, sector reads.
// ---------------------------------------------------------------------------
class VirtualDiskSectorSource
{
public:
    VirtualDiskSectorSource() = default;
    ~VirtualDiskSectorSource() { Close(); }

    VirtualDiskSectorSource(const VirtualDiskSectorSource&) = delete;
    VirtualDiskSectorSource& operator=(const VirtualDiskSectorSource&) = delete;

    // `detail` receives a human-readable reason on failure, and the parser's
    // informational / warning text on success. Never throws.
    bool Open(const std::wstring& path, std::string* outDetail)
    {
        Close();

#if PHOENIX_OS_WINDOWS
        auto reader = std::make_unique<Win32ReadOnlyVirtualDiskReader>();
        if (!reader->Open(path)) {
            if (outDetail) *outDetail = "cannot open virtual disk image read-only (CreateFileW failed)";
            return false;
        }
#else
        auto reader = std::make_unique<FileVirtualDiskReader>();
        if (!reader->Open(WideToNarrow(path))) {
            if (outDetail) *outDetail = "cannot open virtual disk image read-only (ifstream failed)";
            return false;
        }
#endif
        auto source = std::make_unique<VirtualDiskSource>(*reader);
        const VirtualDiskOpenResult result = source->Open();
        if (result != VirtualDiskOpenResult::Ok) {
            if (outDetail) {
                *outDetail = std::string(VirtualDiskOpenDetail(result));
                if (!source->Info().detail.empty()) {
                    *outDetail += "; " + source->Info().detail;
                }
            }
            return false;
        }
        if (source->Info().sectorSize != 512 || source->SectorCount() == 0) {
            if (outDetail) *outDetail = "unsupported virtual geometry (expected 512-byte sectors, non-zero size)";
            return false;
        }

        m_reader = std::move(reader);
        m_source = std::move(source);
        if (outDetail) {
            *outDetail = m_source->Info().detail;
            if (!m_source->Info().warnings.empty()) {
                *outDetail += "; warnings: " + m_source->Info().warnings;
            }
        }
        return true;
    }

    void Close()
    {
        m_source.reset();
        m_reader.reset();
    }

    bool IsOpen() const { return m_source && m_source->IsOpen(); }

    VirtualDiskFormat Format() const
    {
        return m_source ? m_source->Format() : VirtualDiskFormat::Unknown;
    }

    const VirtualDiskInfo& Info() const
    {
        static const VirtualDiskInfo kEmpty{};
        return m_source ? m_source->Info() : kEmpty;
    }

    uint64_t SectorCount() const { return m_source ? m_source->SectorCount() : 0; }

    // Forwards to VirtualDiskSource. `dst` must hold count * 512 bytes.
    bool ReadSectors(uint64_t lba, uint8_t* dst, uint32_t count) const
    {
        return m_source && m_source->ReadSectors(lba, count, dst);
    }

private:
#if !PHOENIX_OS_WINDOWS
    static std::string WideToNarrow(const std::wstring& ws)
    {
        std::string out;
        out.reserve(ws.size());
        for (wchar_t wc : ws) {
            out.push_back((wc >= 0 && wc <= 127) ? static_cast<char>(wc) : '?');
        }
        return out;
    }
#endif

    // Declaration order matters: m_source holds a raw pointer into m_reader,
    // so the reader must outlive the source on destruction. Members are
    // destroyed in reverse order -- m_source first, then m_reader. Close()
    // resets in the same order explicitly.
    std::unique_ptr<IVirtualDiskReader> m_reader;
    std::unique_ptr<VirtualDiskSource>  m_source;
};

// Returns true for extensions that denote a *container* format. Plain raw
// images deliberately return false: SectorReader::OpenImage() preserves the
// exact file length (including a partial trailing sector), whereas the
// virtual-disk path exposes a whole-sector geometry. Mixing the two would
// silently truncate a raw image, so callers must keep them separate.
inline bool IsVirtualDiskImagePath(const std::wstring& path)
{
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot + 1);
    for (wchar_t& c : ext) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    return ext == L"vhd" || ext == L"vhdx" || ext == L"vmdk" ||
           ext == L"qcow2" || ext == L"qcow" || ext == L"vdi" ||
           ext == L"qed" || ext == L"hdd" || ext == L"avhdx";
}

} // namespace phoenix
