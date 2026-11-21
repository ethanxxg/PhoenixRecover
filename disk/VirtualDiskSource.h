#pragma once

// ---------------------------------------------------------------------------
// VirtualDiskSource -- A1-1 read-only virtual disk sector mapping layer.
//
// SectorReader::OpenImage() already handles plain .img/.dd files. What was
// missing for A1-1 (open VMDK/VHD/VHDX/... as a *scan source*) is a layer that
// translates a virtual LBA into a physical file offset. This header provides
// exactly that, and ONLY that:
//
//   * strictly read-only -- it holds a `const IVirtualDiskReader&`, exposes no
//     write method, and the file-backed reader opens with std::ios::in;
//   * no Qt / no Win32 / no device handle, so it is portable and unit-testable
//     with in-memory synthetic images (MemoryVirtualDiskReader);
//   * A1-1a supported: RAW/IMG, VHD fixed, VHD dynamic
//     (BAT + per-sector bitmap, sparse blocks read as zeros);
//   * A1-1b supported: VHDX (headers -> region table -> metadata items ->
//     interleaved BAT), VMDK monolithicSparse (KDMV -> GD -> GT -> grain),
//     QCOW2 v2/v3 (L1/L2 cluster tables), VDI (flat block map);
//   * explicitly rejected rather than misread: VHD differencing / parent
//     images, VHDX differencing + 4Kn + log journal (warning), VMDK flat /
//     split / streamOptimized / compressed grains, QCOW2 with backing file /
//     encryption / compression / incompatible feature bits, VDI differencing.
//     Every rejection returns UnsupportedVariant/UnsupportedFormat with a
//     reason in Info().detail -- never a silent fall-through to RAW.
//
// Anything with no known signature is treated as RAW. Callers should validate
// the resulting geometry before scanning.
//
// COVERAGE (updated after A1-1c / A1-2): VHD dynamic and VHDX are verified
// against real Hyper-V / Virtual PC / Disk2vhd images via an independent
// Python oracle (testdata/virtual-disk-golden/, CTest VirtualDiskGolden), and
// the whole scan path is wired through VirtualDiskSectorSource.h +
// SectorReader::OpenVirtualImage() (CTest VirtualDiskSectorSource).
// REMAINING GAP: QCOW2, VDI and ordinary monolithicSparse VMDK succeed only
// on synthetic fixtures; no real-image golden samples yet. Do not claim
// real-image support for those three until ROADMAP A1-1d is complete.
// The IVirtualDiskReader contract stays unchanged for future formats.
// ---------------------------------------------------------------------------

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace phoenix {

namespace vds_detail {

inline uint16_t Be16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
inline uint32_t Be32(const uint8_t* p)
{
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
           static_cast<uint32_t>(p[3]);
}
inline uint64_t Be64(const uint8_t* p)
{
    return (static_cast<uint64_t>(Be32(p)) << 32) | Be32(p + 4);
}
inline void PutBe32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24); p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);  p[3] = static_cast<uint8_t>(v);
}
inline void PutBe64(uint8_t* p, uint64_t v)
{
    PutBe32(p, static_cast<uint32_t>(v >> 32));
    PutBe32(p + 4, static_cast<uint32_t>(v));
}
inline uint32_t Le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0])        |
           (static_cast<uint32_t>(p[1]) << 8)  |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t Le64(const uint8_t* p)
{
    return static_cast<uint64_t>(Le32(p)) |
           (static_cast<uint64_t>(Le32(p + 4)) << 32);
}
inline void PutLe32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);       p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16); p[3] = static_cast<uint8_t>(v >> 24);
}
inline void PutLe64(uint8_t* p, uint64_t v)
{
    PutLe32(p, static_cast<uint32_t>(v));
    PutLe32(p + 4, static_cast<uint32_t>(v >> 32));
}
inline uint64_t RoundUp(uint64_t v, uint64_t align)
{
    return ((v + align - 1) / align) * align;
}
inline bool IsAllZero(const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n; ++i) if (p[i] != 0) return false;
    return true;
}
} // namespace vds_detail

enum class VirtualDiskFormat : uint8_t
{
    Unknown    = 0,
    Raw        = 1,
    VhdFixed   = 2,
    VhdDynamic = 3,
    Vhdx       = 4,
    Vmdk       = 5,
    Qcow2      = 6,
    Vdi        = 7
};

inline const char* VirtualDiskFormatName(VirtualDiskFormat f)
{
    switch (f) {
    case VirtualDiskFormat::Unknown:    return "unknown";
    case VirtualDiskFormat::Raw:        return "raw/IMG";
    case VirtualDiskFormat::VhdFixed:   return "VHD fixed";
    case VirtualDiskFormat::VhdDynamic: return "VHD dynamic";
    case VirtualDiskFormat::Vhdx:       return "VHDX";
    case VirtualDiskFormat::Vmdk:       return "VMDK";
    case VirtualDiskFormat::Qcow2:      return "QCOW2";
    case VirtualDiskFormat::Vdi:        return "VDI";
    }
    return "unknown";
}

enum class VirtualDiskOpenResult : uint8_t
{
    Ok = 0,
    EmptySource,
    TooSmall,
    ReadFailed,
    UnsupportedFormat,
    BadFooter,
    BadGeometry,
    BadDynamicHeader,
    BadBat,
    Truncated,
    UnsupportedVariant,   // recognised container, unsupported sub-type
    BadHeader,
    BadRegionTable,
    BadMetadata,
    BadDescriptor
};

inline const char* VirtualDiskOpenDetail(VirtualDiskOpenResult r)
{
    switch (r) {
    case VirtualDiskOpenResult::Ok:                 return "ok";
    case VirtualDiskOpenResult::EmptySource:        return "source is empty";
    case VirtualDiskOpenResult::TooSmall:           return "source smaller than one sector";
    case VirtualDiskOpenResult::ReadFailed:         return "read failed while parsing metadata";
    case VirtualDiskOpenResult::UnsupportedFormat:  return "format detected but read-only mapping is not implemented";
    case VirtualDiskOpenResult::UnsupportedVariant: return "container recognised but this sub-type is not supported";
    case VirtualDiskOpenResult::BadFooter:          return "invalid VHD footer";
    case VirtualDiskOpenResult::BadGeometry:        return "invalid virtual disk geometry";
    case VirtualDiskOpenResult::BadDynamicHeader:   return "invalid VHD dynamic header";
    case VirtualDiskOpenResult::BadBat:             return "invalid block allocation table";
    case VirtualDiskOpenResult::BadHeader:          return "invalid container header";
    case VirtualDiskOpenResult::BadRegionTable:     return "invalid VHDX region table";
    case VirtualDiskOpenResult::BadMetadata:        return "invalid metadata table / item";
    case VirtualDiskOpenResult::BadDescriptor:      return "invalid VMDK descriptor";
    case VirtualDiskOpenResult::Truncated:          return "file is truncated relative to declared size";
    }
    return "unknown";
}

struct VirtualDiskInfo
{
    VirtualDiskFormat format      = VirtualDiskFormat::Unknown;
    uint64_t          virtualBytes = 0;
    uint32_t          sectorSize   = 512;
    uint64_t          blockSize    = 0;    // dynamic VHD: bytes per block
    uint32_t          batEntries   = 0;    // dynamic VHD: BAT entry count
    std::string       detail;
    std::string       warnings;            // non-fatal integrity notes for the caller
};

// Read-only byte source. Implementations must never mutate the backing data.
class IVirtualDiskReader
{
public:
    virtual ~IVirtualDiskReader() = default;
    virtual uint64_t Size() const = 0;
    virtual bool Read(uint64_t offset, void* dst, size_t length) const = 0;
};

namespace vds_detail {

inline bool ReadU32(const IVirtualDiskReader* r, uint64_t off,
                    uint32_t& out, bool bigEndian)
{
    uint8_t b[4] = {};
    if (!r->Read(off, b, 4)) return false;
    out = bigEndian ? Be32(b) : Le32(b);
    return true;
}
inline bool ReadU64(const IVirtualDiskReader* r, uint64_t off,
                    uint64_t& out, bool bigEndian)
{
    uint8_t b[8] = {};
    if (!r->Read(off, b, 8)) return false;
    out = bigEndian ? Be64(b) : Le64(b);
    return true;
}

} // namespace vds_detail

class MemoryVirtualDiskReader : public IVirtualDiskReader
{
public:
    MemoryVirtualDiskReader() = default;
    explicit MemoryVirtualDiskReader(std::vector<uint8_t> data)
        : m_data(std::move(data)) {}

    std::vector<uint8_t>& Data() { return m_data; }
    const std::vector<uint8_t>& Data() const { return m_data; }

    uint64_t Size() const override { return static_cast<uint64_t>(m_data.size()); }

    bool Read(uint64_t offset, void* dst, size_t length) const override
    {
        if (length == 0) return true;
        const uint64_t n = static_cast<uint64_t>(m_data.size());
        if (offset > n || static_cast<uint64_t>(length) > n - offset) return false;
        std::memcpy(dst, m_data.data() + static_cast<size_t>(offset), length);
        return true;
    }

private:
    std::vector<uint8_t> m_data;
};

class FileVirtualDiskReader : public IVirtualDiskReader
{
public:
    // Opens strictly read-only. Never writes, never opens a device.
    bool Open(const std::string& path)
    {
        m_stream.close();
        m_stream.clear();
        m_stream.open(path, std::ios::in | std::ios::binary);
        if (!m_stream.is_open()) return false;
        m_stream.seekg(0, std::ios::end);
        const std::streamoff end = m_stream.tellg();
        if (end < 0) { m_stream.close(); return false; }
        m_size = static_cast<uint64_t>(end);
        return true;
    }
    bool IsOpen() const { return m_stream.is_open(); }
    void Close() { m_stream.close(); m_size = 0; }

    uint64_t Size() const override { return m_size; }

    bool Read(uint64_t offset, void* dst, size_t length) const override
    {
        if (length == 0) return true;
        if (!m_stream.is_open() || offset > m_size ||
            static_cast<uint64_t>(length) > m_size - offset) {
            return false;
        }
        m_stream.clear();
        m_stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!m_stream) return false;
        m_stream.read(static_cast<char*>(dst),
                      static_cast<std::streamsize>(length));
        return static_cast<size_t>(m_stream.gcount()) == length;
    }

private:
    mutable std::ifstream m_stream;
    uint64_t m_size = 0;
};

class VirtualDiskSource
{
public:
    explicit VirtualDiskSource(const IVirtualDiskReader& reader)
        : m_reader(&reader) {}

    VirtualDiskOpenResult Open()
    {
        Close();
        const uint64_t fileSize = m_reader->Size();
        if (fileSize == 0) return VirtualDiskOpenResult::EmptySource;
        if (fileSize < 512) return VirtualDiskOpenResult::TooSmall;
        m_fileSize = fileSize;

        // ---- container signatures (512 bytes covers every header we need) ----
        uint8_t head[512] = {};
        if (!m_reader->Read(0, head, sizeof(head))) {
            return VirtualDiskOpenResult::ReadFailed;
        }
        if (std::memcmp(head, "vhdxfile", 8) == 0) return OpenVhdx();
        if (std::memcmp(head, "QFI\xfb", 4) == 0)  return OpenQcow2(head);
        if (std::memcmp(head, "KDMV", 4) == 0)     return OpenVmdkSparse(head);
        if (std::memcmp(head, "# Disk DescriptorFile", 21) == 0)
            return OpenVmdkDescriptor();
        if ((head[0x40] == 0x7f && head[0x41] == 0x10 &&
             head[0x42] == 0xda && head[0x43] == 0xbe) ||
            (head[0x40] == 0xbe && head[0x41] == 0xda &&
             head[0x42] == 0x10 && head[0x43] == 0x7f))
            return OpenVdi(head);

        // ---- VHD: 512-byte "conectix" footer at the very end of the file ----
        uint8_t footer[512] = {};
        if (!m_reader->Read(fileSize - 512, footer, sizeof(footer))) {
            return VirtualDiskOpenResult::ReadFailed;
        }
        if (std::memcmp(footer, "conectix", 8) == 0) {
            return OpenVhd(footer);
        }

        // ---- no signature: plain raw/IMG image ----
        m_info.format       = VirtualDiskFormat::Raw;
        m_info.virtualBytes = (fileSize / 512) * 512;
        m_info.sectorSize   = 512;
        m_info.detail       = "no virtual disk signature; treated as raw/IMG";
        m_open = true;
        return VirtualDiskOpenResult::Ok;
    }

    bool IsOpen() const { return m_open; }
    const VirtualDiskInfo& Info() const { return m_info; }
    VirtualDiskFormat Format() const { return m_info.format; }

    uint64_t SectorCount() const
    {
        return m_info.virtualBytes / m_info.sectorSize;
    }

    // Reads `count` consecutive virtual sectors into dst (>= count * 512).
    // Sparse / unallocated sectors are zero-filled. Returns false on any
    // out-of-range request or metadata read failure; no partial state change.
    bool ReadSectors(uint64_t lba, uint32_t count, uint8_t* dst) const
    {
        if (!m_open || dst == nullptr || count == 0) return false;
        const uint64_t total = SectorCount();
        if (lba > total || static_cast<uint64_t>(count) > total - lba) {
            return false;
        }
        const uint64_t bytes = static_cast<uint64_t>(count) * 512;
        const uint64_t fileOffset = lba * 512;

        if (m_info.format == VirtualDiskFormat::Raw ||
            m_info.format == VirtualDiskFormat::VhdFixed) {
            const uint64_t dataLimit = (m_info.format == VirtualDiskFormat::VhdFixed)
                                       ? m_fileSize - 512 : m_fileSize;
            if (fileOffset > dataLimit || bytes > dataLimit - fileOffset) {
                return false;
            }
            return m_reader->Read(fileOffset, dst, static_cast<size_t>(bytes));
        }

        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t virtualSector = lba + i;
            uint8_t* out = dst + static_cast<size_t>(i) * 512;
            bool ok = false;
            switch (m_info.format) {
            case VirtualDiskFormat::VhdDynamic: ok = ReadVhdSector(virtualSector, out); break;
            case VirtualDiskFormat::Vhdx:       ok = ReadVhdxSector(virtualSector, out); break;
            case VirtualDiskFormat::Vmdk:       ok = ReadVmdkSector(virtualSector, out); break;
            case VirtualDiskFormat::Qcow2:      ok = ReadQcow2Sector(virtualSector, out); break;
            case VirtualDiskFormat::Vdi:        ok = ReadVdiSector(virtualSector, out); break;
            default: return false;
            }
            if (!ok) return false;
        }
        return true;
    }

    void Close()
    {
        m_open = false;
        m_info = VirtualDiskInfo();
        m_bat.clear();
        m_fileSize = 0;
        m_sectorsPerBlock = 0;
        m_bitmapSectors = 0;
        m_vhdxBatOffset = m_vhdxBatLength = 0;
        m_vhdxBlockSize = m_vhdxChunkRatio = 0;
        m_vmdkGrainSize = m_vmdkGdOffsetBytes = 0;
        m_vmdkGtesPerGt = m_vmdkFlags = m_vmdkVersion = 0;
        m_qcowClusterSize = m_qcowL1Offset = m_qcowL1Size = 0;
        m_vdiBlockMapOffset = m_vdiDataOffset = m_vdiBlockSize = 0;
        m_vdiBlockExtra = m_vdiBlockStride = 0;
        m_vdiBlocks = 0;
    }

private:
    VirtualDiskOpenResult OpenVhd(const uint8_t* footer)
    {
        using namespace vds_detail;

        const uint64_t currentSize = Be64(footer + 0x30);
        const uint64_t dataOffset  = Be64(footer + 0x10);
        const uint32_t diskType    = Be32(footer + 0x3C);

        if (currentSize == 0 || (currentSize % 512) != 0) {
            return VirtualDiskOpenResult::BadGeometry;
        }
        if (diskType == 4) {
            m_info.format = VirtualDiskFormat::VhdDynamic;
            m_info.detail = "VHD differencing disk requires a parent image";
            return VirtualDiskOpenResult::UnsupportedFormat;
        }
        if (diskType != 2 && diskType != 3) {
            return VirtualDiskOpenResult::BadFooter;
        }

        if (diskType == 2) {   // ---- fixed ----
            if (m_fileSize - 512 < currentSize) {
                return VirtualDiskOpenResult::Truncated;
            }
            m_info.format       = VirtualDiskFormat::VhdFixed;
            m_info.virtualBytes = currentSize;
            m_info.detail       = "VHD fixed: data at offset 0, 512-byte footer at EOF";
            m_open = true;
            return VirtualDiskOpenResult::Ok;
        }

        // ---- dynamic: "cxsparse" header -> BAT -> blocks ----
        if (dataOffset == 0xFFFFFFFFFFFFFFFFULL || dataOffset > m_fileSize - 512 ||
            m_fileSize - dataOffset < 1024) {
            return VirtualDiskOpenResult::BadDynamicHeader;
        }
        uint8_t dyn[1024] = {};
        if (!m_reader->Read(dataOffset, dyn, sizeof(dyn)) ||
            std::memcmp(dyn, "cxsparse", 8) != 0) {
            return VirtualDiskOpenResult::BadDynamicHeader;
        }

        const uint64_t tableOffset = Be64(dyn + 0x10);
        const uint32_t maxEntries  = Be32(dyn + 0x1C);
        const uint32_t blockSize   = Be32(dyn + 0x20);
        if (blockSize < 512 || (blockSize % 512) != 0 ||
            (blockSize & (blockSize - 1)) != 0 || maxEntries == 0) {
            return VirtualDiskOpenResult::BadDynamicHeader;
        }
        if (static_cast<uint64_t>(maxEntries) * blockSize < currentSize) {
            return VirtualDiskOpenResult::BadGeometry;
        }
        const uint64_t batBytes = static_cast<uint64_t>(maxEntries) * 4;
        if (tableOffset > m_fileSize - 512 ||
            batBytes > m_fileSize - 512 - tableOffset) {
            return VirtualDiskOpenResult::BadBat;
        }

        std::vector<uint32_t> bat(static_cast<size_t>(maxEntries));
        for (uint32_t i = 0; i < maxEntries; ++i) {
            uint8_t e[4] = {};
            if (!m_reader->Read(tableOffset + static_cast<uint64_t>(i) * 4,
                                e, 4)) {
                return VirtualDiskOpenResult::ReadFailed;
            }
            bat[i] = Be32(e);
        }

        const uint64_t sectorsPerBlock = blockSize / 512;
        const uint64_t bitmapBytes     = (sectorsPerBlock + 7) / 8; // >= 1
        const uint64_t bitmapSectors   = RoundUp(bitmapBytes, 512) / 512;

        m_sectorsPerBlock = sectorsPerBlock;
        m_bitmapSectors   = bitmapSectors;
        m_bat             = std::move(bat);
        m_info.format       = VirtualDiskFormat::VhdDynamic;
        m_info.virtualBytes = currentSize;
        m_info.blockSize    = blockSize;
        m_info.batEntries   = maxEntries;
        m_info.detail       = "VHD dynamic: BAT + per-sector bitmap, sparse blocks zero-filled";
        m_open = true;
        return VirtualDiskOpenResult::Ok;
    }

    // ================= A1-1b: extended container parsers =================

    VirtualDiskOpenResult UnsupportedVariant(const char* why)
    {
        m_info.detail = std::string(VirtualDiskFormatName(m_info.format)) +
                        " sub-type not supported: " + why;
        return VirtualDiskOpenResult::UnsupportedVariant;
    }

    bool ReadVhdSector(uint64_t virtualSector, uint8_t* out) const
    {
        const uint64_t block          = virtualSector / m_sectorsPerBlock;
        const uint64_t sectorInBlock  = virtualSector % m_sectorsPerBlock;
        if (block >= m_bat.size() || m_bat[block] == 0xFFFFFFFFu) {
            std::memset(out, 0, 512); return true;
        }
        const uint64_t blockStart = static_cast<uint64_t>(m_bat[block]) * 512;
        uint8_t bitmapByte = 0;
        if (!m_reader->Read(blockStart + sectorInBlock / 8, &bitmapByte, 1)) {
            return false;
        }
        const uint8_t mask = static_cast<uint8_t>(0x80u >> (sectorInBlock % 8));
        if ((bitmapByte & mask) == 0) { std::memset(out, 0, 512); return true; }
        return m_reader->Read(blockStart + m_bitmapSectors * 512 +
                              sectorInBlock * 512, out, 512);
    }

    // ---- VHDX: headers -> region table -> metadata items -> interleaved BAT ----
    VirtualDiskOpenResult OpenVhdx()
    {
        using namespace vds_detail;
        m_info.format = VirtualDiskFormat::Vhdx;
        if (m_fileSize < 0x50000ULL) return VirtualDiskOpenResult::Truncated;

        // Two redundant headers; the one with the highest sequence number wins.
        const uint64_t kHeaderOffsets[2] = {0x10000ULL, 0x20000ULL};
        bool haveHeader = false;
        uint64_t bestSeq = 0;
        uint8_t hdr[80] = {};
        for (int k = 0; k < 2; ++k) {
            uint8_t h[80] = {};
            if (!m_reader->Read(kHeaderOffsets[k], h, sizeof(h))) continue;
            if (std::memcmp(h, "head", 4) != 0) continue;
            const uint64_t seq = Le64(h + 8);
            if (!haveHeader || seq >= bestSeq) {
                bestSeq = seq; haveHeader = true;
                std::memcpy(hdr, h, sizeof(h));
            }
        }
        if (!haveHeader) return VirtualDiskOpenResult::BadHeader;
        // VHDXHeader: sig(4) checksum(4) sequence(8) fileWriteGuid(16)
        // dataWriteGuid(16) logGuid(16) ... -> logGuid lives at +48.
        if (!IsAllZero(hdr + 48, 16)) {
            m_info.warnings = "VHDX log journal present; metadata may predate the "
                              "last write (log replay not implemented)";
        }

        static const uint8_t kBatGuid[16] = {
            0x66,0x77,0xC2,0x2D,0x23,0xF6,0x00,0x42,
            0x9D,0x64,0x11,0x5E,0x9B,0xFD,0x4A,0x08 };
        static const uint8_t kMdGuid[16] = {
            0x06,0xA2,0x7C,0x8B,0x90,0x47,0x9A,0x4B,
            0xB8,0xFE,0x57,0x5F,0x05,0x0F,0x88,0x6E };

        uint64_t batOffset = 0, batLength = 0, mdOffset = 0, mdLength = 0;
        const uint64_t kRegionOffsets[2] = {0x30000ULL, 0x40000ULL};
        for (int k = 0; k < 2; ++k) {
            uint8_t rh[16] = {};
            if (!m_reader->Read(kRegionOffsets[k], rh, sizeof(rh))) continue;
            if (std::memcmp(rh, "regi", 4) != 0) continue;
            const uint32_t count = Le32(rh + 8);
            if (count == 0 || count > 2047) continue;
            for (uint32_t i = 0; i < count; ++i) {
                uint8_t e[32] = {};
                if (!m_reader->Read(kRegionOffsets[k] + 16 +
                                    static_cast<uint64_t>(i) * 32, e, 32)) {
                    return VirtualDiskOpenResult::ReadFailed;
                }
                const uint64_t off = Le64(e + 16);
                const uint32_t len = Le32(e + 24);
                const uint32_t req = Le32(e + 28);
                const bool isBat = std::memcmp(e, kBatGuid, 16) == 0;
                const bool isMd  = std::memcmp(e, kMdGuid, 16) == 0;
                if (!isBat && !isMd) {
                    if (req & 1u) return UnsupportedVariant("unknown required VHDX region");
                    continue;
                }
                if ((off & 0xFFFFFULL) != 0 || (len & 0xFFFFFULL) != 0 ||
                    off < 0x100000ULL || len == 0 || off > m_fileSize ||
                    len > m_fileSize - off) {
                    return VirtualDiskOpenResult::BadRegionTable;
                }
                if (isBat) { batOffset = off; batLength = len; }
                else       { mdOffset  = off; mdLength  = len; }
            }
            if (batOffset && mdOffset) break;
        }
        if (!batOffset || !mdOffset) return VirtualDiskOpenResult::BadRegionTable;

        // Metadata table: 32-byte header ("metadata" + u16 entryCount at +10),
        // then 32-byte entries (GUID, item offset, item length, flags).
        uint8_t mh[32] = {};
        if (!m_reader->Read(mdOffset, mh, sizeof(mh)) ||
            std::memcmp(mh, "metadata", 8) != 0) {
            return VirtualDiskOpenResult::BadMetadata;
        }
        const uint32_t mdEntries = static_cast<uint32_t>(mh[10]) |
                                   (static_cast<uint32_t>(mh[11]) << 8);
        if (mdEntries == 0 || mdEntries > 2047) {
            return VirtualDiskOpenResult::BadMetadata;
        }
        static const uint8_t kSizeGuid[16] = {
            0x24,0x42,0xA5,0x2F,0x1B,0xCD,0x76,0x48,
            0xB2,0x11,0x5D,0xBE,0xD8,0x3B,0xF4,0xB8 };
        static const uint8_t kFileParamsGuid[16] = {
            0x37,0x67,0xA1,0xCA,0x36,0xFA,0x43,0x4D,
            0xB3,0xB6,0x33,0xF0,0xAA,0x44,0xE7,0x6B };
        static const uint8_t kSectorSizeGuid[16] = {
            0x1D,0xBF,0x41,0x81,0x6F,0xA9,0x09,0x47,
            0xBA,0x47,0xF2,0x33,0xA8,0xFA,0xAB,0x5F };
        static const uint8_t kParentLocatorGuid[16] = {
            0x2D,0x5F,0xD3,0xA8,0x30,0xB3,0x4D,0x45,
            0xAB,0xF7,0xD3,0xD8,0x48,0x34,0xAB,0x0C };

        uint64_t virtualBytes = 0, blockSize = 0, logicalSector = 0;
        bool hasParentLocator = false, hasSize = false, hasParams = false;
        for (uint32_t i = 0; i < mdEntries; ++i) {
            uint8_t e[32] = {};
            if (!m_reader->Read(mdOffset + 32 + static_cast<uint64_t>(i) * 32,
                                e, 32)) {
                return VirtualDiskOpenResult::ReadFailed;
            }
            const uint64_t itemOff = Le32(e + 16);
            const uint32_t itemLen = Le32(e + 20);
            if (itemOff + itemLen > mdLength) {
                return VirtualDiskOpenResult::BadMetadata;
            }
            const uint64_t abs = mdOffset + itemOff;
            if (std::memcmp(e, kSizeGuid, 16) == 0) {
                if (itemLen < 8 || !ReadU64(m_reader, abs, virtualBytes, false))
                    return VirtualDiskOpenResult::BadMetadata;
                hasSize = true;
            } else if (std::memcmp(e, kFileParamsGuid, 16) == 0) {
                uint32_t bs = 0, flags = 0;
                if (itemLen < 8 || !ReadU32(m_reader, abs, bs, false) ||
                    !ReadU32(m_reader, abs + 4, flags, false))
                    return VirtualDiskOpenResult::BadMetadata;
                blockSize = bs;
                if (flags & 0x2u)
                    return UnsupportedVariant("differencing VHDX requires a parent image");
                hasParams = true;
            } else if (std::memcmp(e, kSectorSizeGuid, 16) == 0) {
                uint32_t ss = 0;
                if (itemLen >= 4 && ReadU32(m_reader, abs, ss, false))
                    logicalSector = ss;
            } else if (std::memcmp(e, kParentLocatorGuid, 16) == 0) {
                hasParentLocator = true;
            }
        }
        if (!hasSize || !hasParams) return VirtualDiskOpenResult::BadMetadata;
        if (hasParentLocator) {
            return UnsupportedVariant("differencing VHDX requires a parent image");
        }
        if (logicalSector != 0 && logicalSector != 512) {
            return UnsupportedVariant("4Kn VHDX (logical sector size 4096)");
        }
        if (virtualBytes == 0 || (virtualBytes % 512) != 0) {
            return VirtualDiskOpenResult::BadGeometry;
        }
        if (blockSize < 0x100000ULL || (blockSize & (blockSize - 1)) != 0) {
            return VirtualDiskOpenResult::BadGeometry;
        }
        const uint64_t chunkRatio = (1ULL << 32) / blockSize; // 2^23 * 512 / blockSize
        if (chunkRatio == 0) return VirtualDiskOpenResult::BadGeometry;
        const uint64_t payloadBlocks =
            (virtualBytes + blockSize - 1) / blockSize;
        const uint64_t totalBatEntries =
            payloadBlocks + (payloadBlocks - 1) / chunkRatio;
        if (totalBatEntries > batLength / 8) return VirtualDiskOpenResult::BadBat;

        m_vhdxBatOffset   = batOffset;
        m_vhdxBatLength   = batLength;
        m_vhdxBlockSize   = blockSize;
        m_vhdxChunkRatio  = chunkRatio;
        m_info.virtualBytes = virtualBytes;
        m_info.blockSize    = blockSize;
        m_info.batEntries   = static_cast<uint32_t>(totalBatEntries);
        m_info.detail       = "VHDX: region table + metadata items + interleaved BAT "
                              "(payload/sector-bitmap entries); dynamic/fixed only";
        m_open = true;
        return VirtualDiskOpenResult::Ok;
    }

    bool ReadVhdxSector(uint64_t vsec, uint8_t* out) const
    {
        const uint64_t vbyte         = vsec * 512;
        const uint64_t block         = vbyte / m_vhdxBlockSize;
        const uint64_t offsetInBlock = vbyte % m_vhdxBlockSize;
        const uint64_t batIndex      = block + block / m_vhdxChunkRatio;
        if (batIndex * 8 + 8 > m_vhdxBatLength) return false;
        uint64_t entry = 0;
        if (!vds_detail::ReadU64(m_reader, m_vhdxBatOffset + batIndex * 8,
                                 entry, false)) return false;
        const uint32_t state = static_cast<uint32_t>(entry & 0x7ULL);
        if (state == 6) {   // PAYLOAD_BLOCK_FULLY_PRESENT
            const uint64_t off = (entry >> 20) * 0x100000ULL + offsetInBlock;
            return m_reader->Read(off, out, 512);
        }
        if (state <= 3) {   // NOT_PRESENT / UNDEFINED / ZERO / UNMAPPED
            std::memset(out, 0, 512);
            return true;
        }
        return false;       // 7 = PARTIALLY_PRESENT (differencing only) / reserved
    }

    // ---- VMDK monolithicSparse: KDMV header -> GD -> GT -> grain ----
    VirtualDiskOpenResult OpenVmdkSparse(const uint8_t* head)
    {
        using namespace vds_detail;
        m_info.format = VirtualDiskFormat::Vmdk;
        const uint32_t version     = Le32(head + 4);
        const uint32_t flags       = Le32(head + 8);
        const uint64_t capacity    = Le64(head + 12);
        const uint64_t grainSize   = Le64(head + 20);
        const uint64_t descriptorOffset = Le64(head + 28);
        const uint64_t descriptorSize   = Le64(head + 36);
        const uint32_t gtesPerGt   = Le32(head + 44);
        const uint64_t gdOffset    = Le64(head + 56);
        const uint16_t compress    = static_cast<uint16_t>(head[77]) |
                                    (static_cast<uint16_t>(head[78]) << 8);

        if ((flags & (1u << 16)) != 0 || compress != 0) {
            return UnsupportedVariant("compressed grains (streamOptimized)");
        }
        if (gdOffset == 0xFFFFFFFFFFFFFFFFULL) {
            return UnsupportedVariant("streamOptimized footer-based grain directory");
        }
        if (capacity == 0 || capacity > std::numeric_limits<uint64_t>::max() / 512) {
            return VirtualDiskOpenResult::BadHeader;
        }
        if (grainSize <= 8 || (grainSize & (grainSize - 1)) != 0 ||
            gtesPerGt == 0 || gtesPerGt > 4096) {
            return VirtualDiskOpenResult::BadHeader;
        }
        if (gdOffset == 0 || gdOffset > m_fileSize / 512) {
            return VirtualDiskOpenResult::BadBat;
        }
        const uint64_t gdBytes = gdOffset * 512;
        const uint64_t neededGdEntries = capacity / grainSize / gtesPerGt + 1;
        if (neededGdEntries * 4 > m_fileSize - gdBytes) {
            return VirtualDiskOpenResult::BadBat;
        }

        // Embedded descriptor: validate that this really is a single sparse
        // extent. Flat / split extents live in companion files and are refused.
        if (descriptorOffset != 0 && descriptorSize != 0) {
            const uint64_t off = descriptorOffset * 512;
            const uint64_t len = descriptorSize * 512;
            if (off > m_fileSize || len > m_fileSize - off || len > 65536) {
                return VirtualDiskOpenResult::BadDescriptor;
            }
            std::vector<char> text(static_cast<size_t>(len));
            if (!m_reader->Read(off, text.data(), text.size())) {
                return VirtualDiskOpenResult::ReadFailed;
            }
            const std::string s(text.data(), text.size());
            if (s.find("SPARSE") == std::string::npos &&
                s.find("monolithicSparse") == std::string::npos) {
                return UnsupportedVariant("non-sparse extent in embedded descriptor");
            }
            if (s.find("FLAT") != std::string::npos) {
                return UnsupportedVariant("flat extent requires a companion data file");
            }
        }

        if (head[72] != 0) {
            m_info.warnings = "VMDK unclean shutdown flag set; grain tables may be stale";
        }
        m_vmdkVersion       = version;
        m_vmdkFlags         = flags;
        m_vmdkGrainSize     = grainSize;
        m_vmdkGtesPerGt     = gtesPerGt;
        m_vmdkGdOffsetBytes = gdBytes;
        m_info.virtualBytes = capacity * 512;
        m_info.blockSize    = grainSize * 512;
        m_info.detail       = "VMDK monolithicSparse: KDMV header + grain directory "
                              "+ grain tables; unallocated/zeroed grains read as zeros";
        m_open = true;
        return VirtualDiskOpenResult::Ok;
    }

    VirtualDiskOpenResult OpenVmdkDescriptor()
    {
        m_info.format = VirtualDiskFormat::Vmdk;
        const size_t n = static_cast<size_t>(
            std::min<uint64_t>(m_fileSize, 65536));
        std::vector<char> text(n);
        if (!m_reader->Read(0, text.data(), n)) {
            return VirtualDiskOpenResult::ReadFailed;
        }
        const std::string s(text.data(), n);
        uint32_t extents = 0;
        bool flat = false;
        std::string createType = "unknown";
        size_t pos = 0;
        while (pos < s.size()) {
            size_t e = s.find('\n', pos);
            if (e == std::string::npos) e = s.size();
            std::string line = s.substr(pos, e - pos);
            while (!line.empty() && (line[0] == ' ' || line[0] == '\t'))
                line.erase(0, 1);
            if (line.compare(0, 3, "RW ") == 0 || line.compare(0, 3, "RO ") == 0) {
                ++extents;
                if (line.find("FLAT") != std::string::npos) flat = true;
            }
            const size_t c = line.find("createType=");
            if (c != std::string::npos) {
                const size_t q1 = line.find('"', c);
                const size_t q2 = q1 == std::string::npos
                                  ? std::string::npos : line.find('"', q1 + 1);
                if (q2 != std::string::npos)
                    createType = line.substr(q1 + 1, q2 - q1 - 1);
            }
            pos = e + 1;
        }
        m_info.detail = "VMDK descriptor (createType=" + createType + ", " +
                        std::to_string(extents) + " extent" +
                        (extents == 1 ? "" : "s") +
                        (flat ? ", flat" : "") +
                        "): extent data lives in companion file(s); multi-file "
                        "mounting not implemented in A1-1b";
        return VirtualDiskOpenResult::UnsupportedFormat;
    }

    bool ReadVmdkSector(uint64_t vsec, uint8_t* out) const
    {
        const uint64_t grain   = vsec / m_vmdkGrainSize;
        const uint64_t gdIndex = grain / m_vmdkGtesPerGt;
        const uint64_t gtIndex = grain % m_vmdkGtesPerGt;
        uint32_t gtSector = 0;
        if (!vds_detail::ReadU32(m_reader,
                                 m_vmdkGdOffsetBytes + gdIndex * 4,
                                 gtSector, false)) return false;
        if (gtSector == 0) { std::memset(out, 0, 512); return true; }
        uint32_t grainSector = 0;
        if (!vds_detail::ReadU32(m_reader,
                                 static_cast<uint64_t>(gtSector) * 512 + gtIndex * 4,
                                 grainSector, false)) return false;
        if (grainSector == 0 ||
            ((m_vmdkFlags & 0x4u) && m_vmdkVersion >= 2 && grainSector == 1)) {
            std::memset(out, 0, 512);
            return true;
        }
        const uint64_t off = static_cast<uint64_t>(grainSector) * 512 +
                             (vsec % m_vmdkGrainSize) * 512;
        return m_reader->Read(off, out, 512);
    }

    // ---- QCOW2 v2/v3: big-endian header -> L1 table -> L2 table -> cluster ----
    VirtualDiskOpenResult OpenQcow2(const uint8_t* head)
    {
        using namespace vds_detail;
        m_info.format = VirtualDiskFormat::Qcow2;
        const uint32_t version       = Be32(head + 4);
        const uint64_t backingOffset = Be64(head + 8);
        const uint32_t backingSize   = Be32(head + 16);
        const uint32_t clusterBits   = Be32(head + 20);
        const uint64_t virtualBytes  = Be64(head + 24);
        const uint32_t cryptMethod   = Be32(head + 32);
        const uint32_t l1Size        = Be32(head + 36);
        const uint64_t l1Offset      = Be64(head + 40);
        const uint32_t nbSnapshots   = Be32(head + 60);

        if (version != 2 && version != 3) {
            return UnsupportedVariant("only qcow2 v2/v3 supported");
        }
        if (backingOffset != 0 || backingSize != 0) {
            return UnsupportedVariant("backing file chain requires parent image");
        }
        if (cryptMethod != 0) {
            return UnsupportedVariant("encrypted qcow2 (AES/LUKS)");
        }
        if (version >= 3 && Be64(head + 72) != 0) {
            return UnsupportedVariant("qcow2 incompatible feature bits "
                                      "(external data / extended L2 / dirty / corrupt)");
        }
        if (clusterBits < 9 || clusterBits > 21) {
            return VirtualDiskOpenResult::BadHeader;
        }
        const uint64_t clusterSize = 1ULL << clusterBits;
        if (virtualBytes == 0 || (virtualBytes % 512) != 0) {
            return VirtualDiskOpenResult::BadGeometry;
        }
        if (l1Size == 0 || l1Offset == 0 || (l1Offset % clusterSize) != 0 ||
            l1Offset > m_fileSize || static_cast<uint64_t>(l1Size) * 8 >
            m_fileSize - l1Offset) {
            return VirtualDiskOpenResult::BadBat;
        }
        if (nbSnapshots != 0) {
            m_info.warnings = "qcow2 contains snapshots; only the active L1 table is read";
        }
        m_qcowClusterSize = clusterSize;
        m_qcowL1Offset    = l1Offset;
        m_qcowL1Size      = l1Size;
        m_info.virtualBytes = virtualBytes;
        m_info.blockSize    = clusterSize;
        m_info.detail       = "QCOW2 v" + std::to_string(version) +
                              ": L1/L2 cluster tables, uncompressed, no backing file";
        m_open = true;
        return VirtualDiskOpenResult::Ok;
    }

    bool ReadQcow2Sector(uint64_t vsec, uint8_t* out) const
    {
        const uint64_t vbyte = vsec * 512;
        const uint64_t l2PerTable = m_qcowClusterSize / 8;
        const uint64_t clusterIndex = vbyte / m_qcowClusterSize;
        const uint64_t l1Index = clusterIndex / l2PerTable;
        const uint64_t l2Index = clusterIndex % l2PerTable;
        const uint64_t offsetInCluster = vbyte % m_qcowClusterSize;
        if (l1Index >= m_qcowL1Size) { std::memset(out, 0, 512); return true; }

        uint64_t l1e = 0;
        if (!vds_detail::ReadU64(m_reader, m_qcowL1Offset + l1Index * 8,
                                 l1e, true)) return false;
        const uint64_t kOffsetMask = 0x00FFFFFFFFFFFE00ULL;
        const uint64_t l2Offset = l1e & kOffsetMask;
        if (l2Offset == 0) { std::memset(out, 0, 512); return true; }

        uint64_t l2e = 0;
        if (!vds_detail::ReadU64(m_reader, l2Offset + l2Index * 8,
                                 l2e, true)) return false;
        if ((l2e & (1ULL << 63)) != 0) return false;   // compressed cluster
        const uint64_t hostOffset = l2e & kOffsetMask;
        if (hostOffset == 0) { std::memset(out, 0, 512); return true; }
        return m_reader->Read(hostOffset + offsetInCluster, out, 512);
    }

    // ---- VDI (VirtualBox): little-endian header -> flat block map ----
    VirtualDiskOpenResult OpenVdi(const uint8_t* head)
    {
        using namespace vds_detail;
        m_info.format = VirtualDiskFormat::Vdi;
        const uint32_t imageType    = Le32(head + 0x4C);
        const uint32_t mapOffset    = Le32(head + 0x154);
        const uint32_t dataOffset   = Le32(head + 0x158);
        const uint32_t sectorSize   = Le32(head + 0x168);
        const uint64_t diskSize     = Le64(head + 0x170);
        const uint32_t blockData    = Le32(head + 0x178);
        const uint32_t blockExtra   = Le32(head + 0x17C);
        const uint32_t blocksInImage= Le32(head + 0x180);

        if (imageType != 1 && imageType != 2) {
            return UnsupportedVariant("differencing / undo VDI requires a parent image");
        }
        if (sectorSize != 512) {
            return UnsupportedVariant("VDI logical sector size is not 512");
        }
        if (diskSize == 0 || (diskSize % 512) != 0) {
            return VirtualDiskOpenResult::BadGeometry;
        }
        if (blockData == 0 || (blockData % 512) != 0 ||
            (blockData & (blockData - 1)) != 0) {
            return VirtualDiskOpenResult::BadHeader;
        }
        if (blocksInImage == 0 || mapOffset > m_fileSize ||
            static_cast<uint64_t>(blocksInImage) * 4 > m_fileSize - mapOffset) {
            return VirtualDiskOpenResult::BadBat;
        }
        if (static_cast<uint64_t>(blocksInImage) * blockData < diskSize) {
            return VirtualDiskOpenResult::BadGeometry;
        }

        m_vdiBlockMapOffset = mapOffset;
        m_vdiDataOffset     = dataOffset;
        m_vdiBlockSize      = blockData;
        m_vdiBlockExtra     = blockExtra;
        m_vdiBlockStride    = static_cast<uint64_t>(blockData) + blockExtra;
        m_vdiBlocks         = blocksInImage;
        m_info.virtualBytes = diskSize;
        m_info.blockSize    = blockData;
        m_info.detail       = std::string("VDI ") +
                              (imageType == 2 ? "fixed" : "dynamic") +
                              ": flat block map, 0xFFFFFFFF/0xFFFFFFFE read as zeros";
        m_open = true;
        return VirtualDiskOpenResult::Ok;
    }

    bool ReadVdiSector(uint64_t vsec, uint8_t* out) const
    {
        const uint64_t vbyte         = vsec * 512;
        const uint64_t block         = vbyte / m_vdiBlockSize;
        const uint64_t offsetInBlock = vbyte % m_vdiBlockSize;
        if (block >= m_vdiBlocks) { std::memset(out, 0, 512); return true; }
        uint32_t phys = 0;
        if (!vds_detail::ReadU32(m_reader,
                                 m_vdiBlockMapOffset + block * 4,
                                 phys, false)) return false;
        if (phys == 0xFFFFFFFFu || phys == 0xFFFFFFFEu) {
            std::memset(out, 0, 512);
            return true;
        }
        const uint64_t off = m_vdiDataOffset +
                             static_cast<uint64_t>(phys) * m_vdiBlockStride +
                             m_vdiBlockExtra + offsetInBlock;
        return m_reader->Read(off, out, 512);
    }

    const IVirtualDiskReader* m_reader = nullptr;
    VirtualDiskInfo           m_info;
    std::vector<uint32_t>     m_bat;
    uint64_t m_fileSize        = 0;
    uint64_t m_sectorsPerBlock = 0;
    uint64_t m_bitmapSectors   = 0;
    bool     m_open            = false;

    // VHDX
    uint64_t m_vhdxBatOffset  = 0;
    uint64_t m_vhdxBatLength  = 0;
    uint64_t m_vhdxBlockSize  = 0;
    uint64_t m_vhdxChunkRatio = 0;
    // VMDK sparse
    uint64_t m_vmdkGrainSize     = 0;
    uint64_t m_vmdkGdOffsetBytes = 0;
    uint32_t m_vmdkGtesPerGt     = 0;
    uint32_t m_vmdkFlags         = 0;
    uint32_t m_vmdkVersion       = 0;
    // QCOW2
    uint64_t m_qcowClusterSize = 0;
    uint64_t m_qcowL1Offset    = 0;
    uint64_t m_qcowL1Size      = 0;
    // VDI
    uint64_t m_vdiBlockMapOffset = 0;
    uint64_t m_vdiDataOffset     = 0;
    uint64_t m_vdiBlockSize      = 0;
    uint64_t m_vdiBlockExtra     = 0;
    uint64_t m_vdiBlockStride    = 0;
    uint32_t m_vdiBlocks         = 0;
};

} // namespace phoenix
