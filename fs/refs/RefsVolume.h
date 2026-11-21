#pragma once

// ---------------------------------------------------------------------------
// RefsVolume -- A1-5a ReFS read-only on-disk parser.
//
// Pure C++17, header-only, zero Qt / zero Win32 / zero writes.  The volume is
// reached through a tiny IRefsImage byte-source abstraction, so the exact same
// code works on a memory buffer, a raw image file, a virtual-disk reader or a
// BitLocker-unlocked reader.  Every read is bounds-checked; a damaged or
// unsupported structure yields an explicit status, NEVER a silent zero fill.
//
// COVERAGE (structures implemented at published byte offsets)
//   * VBR:           "ReFS\0\0\0\0"@0x03, "FSRS"@0x10, 0x200 size, sector /
//                    sectors-per-cluster geometry @0x20/0x24, version @0x28/29,
//                    checksum selector @0x2A, flags @0x2C, 64 MiB container
//                    size @0x40, ROR1+ADD self-checksum @0x16
//   * SUPB:          primary LCN 30 + backups at totalClusters-2/-3, 80-byte
//                    common page header, GUID @0x50, generation @0x68,
//                    checkpoint LCN list @0xC0/@0xC8, self-descriptor @0xD0
//   * CHKP:          two alternating copies, virtual clock @0x60, flags @0x78,
//                    page-ref size @0x5C, root count @0x90, direct / indirect
//                    root-offset list @0x94, 13 roots, self-descriptor @0x58
//   * Page ref:      48 B (CRC64) / 72 B (SHA-256) / 104 B (legacy, refused),
//                    four LCN slots, cktype @0x22, digest @0x28; a Merkle
//                    digest over the WHOLE child page after VLCN translation
//   * Page header:   signature, header version, volume signature @0x0C
//                    (= XOR of the four GUID words), self LCN @0x20, table id
//                    @0x48
//   * MSB+ B+-tree:  _SmsIndexRoot @0x50, node header at 0x50+u32@0x50,
//                    level @+0x0C, flags @+0x0D, key-index start/count/end
//                    @+0x10/+0x14/+0x20 and invariant (end-start)/4==count,
//                    16-byte row header, internal nodes carry page refs
//   * Container Tbl: root #7/#8 (physical LCN bootstrap exception), 16-byte
//                    key (container id + 0x0000000100000000 tag), 160/224-byte
//                    value, CPC @+0x18, physical start at value[len-16],
//                    VLCN -> PLCN = map[vlcn >> log2cpc] + (vlcn & (cpc-1))
//   * Object Tbl:    root #0/#5, compact 80/88/104/112-byte records, key =
//                    (0, OID), page reference at value+0x20
//   * Parent-Child:  root #4, 48-byte rows, shared 32-byte key/value with
//                    ParentOID @+0x08 and ChildOID @+0x18
//   * Directory:     type 0x30 keys (entry type, key_flags, UTF-16LE name);
//                    kf=0x01 embedded record (times @0x28..0x40, attrs @0x48,
//                    size @0x58, allocated @0x60, sub-records @0xA8);
//                    kf=0x02 index entry 72/84 B (ordinal, home backref,
//                    times, size @0x38, attrs @0x40 with directory bit
//                    0x10000000)
//   * Extents:       24-byte entries (VLCN, flags, record size, file VCN,
//                    padding, run length), sorted by VCN before reassembly
//
// REMAINING GAP (tracked as A1-5b; do NOT claim real-volume support)
//   * No REAL Windows ReFS volume has been tested. The golden corpus contains
//     SYNTHETIC volumes built from the published offsets by an independent
//     Python oracle (testdata/tools/Gen-ReFSGolden.py).
//   * SUPB/CHKP "LcnWithChecksum" self-checksum: implemented as CRC32-C over
//     one cluster with the descriptor region zeroed, per the published
//     description; the exact zeroing range needs real-volume confirmation.
//   * Embedded $DATA sub-record chain uses a flattened golden layout that
//     retains the published marker/descriptor/stream-size/disk-allocated/
//     extent-count fields. A real volume's inner-header + embedded B+-tree
//     node framing, split (kf=0x02) backing records, ADS, snapshots,
//     compression, integrity streams and hard links are NOT parsed; unknown
//     markers produce UnsupportedVariant rather than guessed data.
//   * Legacy v3.4-v3.9 object-table records and 104-byte page refs are
//     explicitly refused; 4Kn support covers the VBR geometry only (no 4Kn
//     physical media sample).
//   * Directory name -> child OID binding is not encoded on disk in a way the
//     public documentation settles; use ParentChildLinks() + ListDirectory(oid).
//   * No write path, no SectorReader wiring, no GUI wiring (user constraint:
//     "不要改 GUI，先对齐功能").
// ---------------------------------------------------------------------------

#include "RefsCrypto.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace phoenix {
namespace refs {

// ---------------------------------------------------------------------------
// Status / result types
// ---------------------------------------------------------------------------

enum class RefsStatus
{
    Ok = 0,
    NotOpen,
    NotReFS,
    BitLockerEncrypted,
    UnsupportedVersion,
    UnsupportedGeometry,
    UnsupportedVariant,
    BadVbrChecksum,
    NoValidSuperblock,
    NoValidCheckpoint,
    BadPageSignature,
    BadVolumeSignature,
    ChecksumMismatch,
    BadNodeInvariant,
    BadTree,
    BadContainerTable,
    BadObjectTable,
    Truncated,
    IoError,
    ObjectNotFound,
    DataUnavailable,
    HoleDetected
};

inline const char* RefsStatusName(RefsStatus s)
{
    switch (s) {
    case RefsStatus::Ok:                   return "Ok";
    case RefsStatus::NotOpen:              return "NotOpen";
    case RefsStatus::NotReFS:              return "NotReFS";
    case RefsStatus::BitLockerEncrypted:   return "BitLockerEncrypted";
    case RefsStatus::UnsupportedVersion:   return "UnsupportedVersion";
    case RefsStatus::UnsupportedGeometry:  return "UnsupportedGeometry";
    case RefsStatus::UnsupportedVariant:   return "UnsupportedVariant";
    case RefsStatus::BadVbrChecksum:       return "BadVbrChecksum";
    case RefsStatus::NoValidSuperblock:    return "NoValidSuperblock";
    case RefsStatus::NoValidCheckpoint:    return "NoValidCheckpoint";
    case RefsStatus::BadPageSignature:     return "BadPageSignature";
    case RefsStatus::BadVolumeSignature:   return "BadVolumeSignature";
    case RefsStatus::ChecksumMismatch:     return "ChecksumMismatch";
    case RefsStatus::BadNodeInvariant:     return "BadNodeInvariant";
    case RefsStatus::BadTree:              return "BadTree";
    case RefsStatus::BadContainerTable:    return "BadContainerTable";
    case RefsStatus::BadObjectTable:       return "BadObjectTable";
    case RefsStatus::Truncated:            return "Truncated";
    case RefsStatus::IoError:              return "IoError";
    case RefsStatus::ObjectNotFound:       return "ObjectNotFound";
    case RefsStatus::DataUnavailable:      return "DataUnavailable";
    case RefsStatus::HoleDetected:         return "HoleDetected";
    }
    return "Unknown";
}

struct RefsGeometry
{
    uint32_t bytesPerSector      = 0;
    uint32_t sectorsPerCluster   = 0;
    uint64_t clusterSize         = 0;
    uint64_t pageSize            = 0;
    uint32_t clustersPerPage     = 0;
    uint64_t totalSectors        = 0;
    uint64_t totalClusters       = 0;
    uint64_t bytesPerContainer   = 0;
    uint64_t clustersPerContainer = 0;
    uint8_t  majorVersion        = 0;
    uint8_t  minorVersion        = 0;
    uint16_t checksumSelector    = 0;
    uint32_t volumeFlags         = 0;
};

struct RefsVolumeInfo
{
    RefsGeometry geometry;
    std::array<uint8_t, 16> volumeGuid{};
    uint32_t volumeSignature    = 0;
    uint32_t pageRefSize        = 0;
    uint8_t  checksumType       = 0;   // 2 = CRC64/NVME, 4 = SHA-256
    bool     indirectRootList   = false;
    uint64_t superblockLcn      = 0;
    uint64_t superblockGeneration = 0;
    uint64_t checkpointLcn      = 0;
    uint64_t checkpointClock    = 0;
    uint32_t checkpointFlags    = 0;
    uint32_t rootCount          = 0;
    size_t   containerCount     = 0;
    size_t   objectCount        = 0;
    size_t   parentChildCount   = 0;
};

struct RefsExtent
{
    uint64_t vcn    = 0;   // file cluster index
    uint64_t vlcn   = 0;   // virtual LCN (needs container translation)
    uint32_t length = 0;   // clusters
};

struct RefsDirEntry
{
    std::wstring name;
    uint64_t parentObjectId = 0;
    uint64_t childOrdinal   = 0;
    uint64_t homeObjectId   = 0;
    uint64_t size           = 0;
    uint64_t allocated      = 0;
    uint64_t createTime     = 0;   // FILETIME
    uint64_t modifyTime     = 0;
    uint64_t metaTime       = 0;
    uint64_t accessTime     = 0;
    uint32_t attributes     = 0;
    bool     isDirectory    = false;
    bool     recordSplit    = false;   // key_flags == 0x02
    bool     dataInline     = false;
    bool     dataAvailable  = false;
    std::vector<uint8_t> inlineData;
    std::vector<RefsExtent> extents;
};

// ---------------------------------------------------------------------------
// Image sources (read-only)
// ---------------------------------------------------------------------------

class IRefsImage
{
public:
    virtual ~IRefsImage() = default;
    virtual uint64_t Size() const = 0;
    virtual bool Read(uint64_t offset, void* dst, size_t length) const = 0;
};

class MemoryRefsImage : public IRefsImage
{
public:
    explicit MemoryRefsImage(const std::vector<uint8_t>& bytes) : m_bytes(bytes) {}
    uint64_t Size() const override { return m_bytes.size(); }
    bool Read(uint64_t offset, void* dst, size_t length) const override
    {
        if (offset > m_bytes.size() || length > m_bytes.size() - offset) return false;
        if (length) std::memcpy(dst, m_bytes.data() + static_cast<size_t>(offset), length);
        return true;
    }
private:
    const std::vector<uint8_t>& m_bytes;
};

class FileRefsImage : public IRefsImage
{
public:
    explicit FileRefsImage(const std::string& path)
        : m_stream(path, std::ios::binary)
    {
        if (!m_stream) return;
        m_stream.seekg(0, std::ios::end);
        const std::streamoff end = m_stream.tellg();
        m_size = end > 0 ? static_cast<uint64_t>(end) : 0;
        m_stream.seekg(0, std::ios::beg);
    }
    bool IsOpen() const { return m_stream.is_open(); }
    uint64_t Size() const override { return m_size; }
    bool Read(uint64_t offset, void* dst, size_t length) const override
    {
        if (!m_stream.is_open() || offset > m_size || length > m_size - offset) return false;
        if (length == 0) return true;
        auto* s = const_cast<std::ifstream*>(&m_stream);
        s->clear();
        s->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!s->good()) return false;
        s->read(static_cast<char*>(dst), static_cast<std::streamsize>(length));
        return s->gcount() == static_cast<std::streamsize>(length);
    }
private:
    mutable std::ifstream m_stream;
    uint64_t m_size = 0;
};

// ---------------------------------------------------------------------------
// Little-endian readers
// ---------------------------------------------------------------------------

namespace refs_detail {

inline uint16_t Le16(const uint8_t* p)
{
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
inline uint32_t Le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t Le64(const uint8_t* p)
{
    return static_cast<uint64_t>(Le32(p)) | (static_cast<uint64_t>(Le32(p + 4)) << 32);
}
inline uint32_t XorGuidWords(const uint8_t guid[16])
{
    return Le32(guid) ^ Le32(guid + 4) ^ Le32(guid + 8) ^ Le32(guid + 12);
}
inline std::wstring Utf16ToWide(const uint8_t* p, size_t bytes)
{
    std::wstring out;
    out.reserve(bytes / 2);
    for (size_t i = 0; i + 1 < bytes; i += 2)
        out.push_back(static_cast<wchar_t>(Le16(p + i)));
    return out;
}
inline uint32_t BitLength(uint64_t v)
{
    uint32_t n = 0;
    while (v) { ++n; v >>= 1; }
    return n;
}
inline uint64_t Align8(uint64_t v) { return (v + 7u) & ~uint64_t(7u); }

} // namespace refs_detail

// ---------------------------------------------------------------------------
// RefsVolume
// ---------------------------------------------------------------------------

class RefsVolume
{
public:
    static constexpr uint64_t kSupbPrimaryLcn = 30;
    static constexpr uint64_t kRootDirectoryOid = 0x600;
    static constexpr uint32_t kDirectoryAttributeBit = 0x10000000u;
    static constexpr uint64_t kContainerTag = 0x0000000100000000ull;
    static constexpr uint32_t kDataMarkerInline  = 0x80000001u;
    static constexpr uint32_t kDataMarkerExtent  = 0x80000002u;
    static constexpr uint32_t kDataDescriptor    = 0x000E0080u;
    static constexpr uint32_t kSubRecordHeader   = 0x30;
    static constexpr uint32_t kExtentEntrySize   = 24;

    RefsVolume() = default;

    // Full read-only bootstrap: VBR -> SUPB (3 copies) -> CHKP (2 copies)
    // -> 13 roots -> container table -> object table -> parent-child table.
    RefsStatus Open(const IRefsImage& image)
    {
        Reset();
        m_image = &image;
        RefsStatus s = ReadVolumeBootRecord();
        if (s != RefsStatus::Ok) return Fail(s, "VBR validation failed");
        s = ReadSuperblocks();
        if (s != RefsStatus::Ok) return Fail(s, "no usable SUPB copy");
        s = ReadCheckpoints();
        if (s != RefsStatus::Ok) return Fail(s, "no usable CHKP copy");
        // The Container Table must come first: roots 7/8 use real LCNs and are
        // the only way to translate the remaining (virtual) root references.
        s = LoadContainerTable();
        if (s != RefsStatus::Ok) return Fail(s, "container table unreadable");
        s = VerifyRootPages();
        if (s != RefsStatus::Ok) return s;   // VerifyRootPages sets a detailed error
        s = LoadObjectTable();
        if (s != RefsStatus::Ok) return Fail(s, "object table unreadable");
        s = LoadParentChild();
        if (s != RefsStatus::Ok) return Fail(s, "parent-child table unreadable");
        m_open = true;
        return Ok();
    }

    bool IsOpen() const { return m_open; }
    const RefsVolumeInfo& Info() const { return m_info; }
    RefsStatus LastStatus() const { return m_status; }
    const std::string& LastError() const { return m_error; }

    // Enumerate one directory's B+-tree.  Files carry their data location
    // (inline bytes or extents); directories are marked with isDirectory.
    std::vector<RefsDirEntry> ListDirectory(uint64_t objectId) const
    {
        std::vector<RefsDirEntry> out;
        if (!m_open) { Fail(RefsStatus::NotOpen, "volume is not open"); return out; }
        auto it = m_objectRoots.find(objectId);
        if (it == m_objectRoots.end()) {
            Fail(RefsStatus::ObjectNotFound,
                 "OID " + std::to_string(objectId) + " has no object-table entry");
            return out;
        }
        RefsStatus s = WalkTree(it->second, true,
            [&](const uint8_t* key, size_t keyLen,
                const uint8_t* val, size_t valLen) -> RefsStatus {
                if (keyLen < 4 || refs_detail::Le16(key) != 0x30u)
                    return RefsStatus::Ok;   // reverse index / other row types
                RefsDirEntry e;
                const RefsStatus r = ParseDirEntry(objectId, key, keyLen, val, valLen, e);
                if (r == RefsStatus::Ok) out.push_back(std::move(e));
                return r;
            });
        if (s != RefsStatus::Ok) { Fail(s, "directory B+-tree walk failed"); return {}; }
        return Ok(std::move(out));
    }

    const std::vector<std::pair<uint64_t, uint64_t>>& ParentChildLinks() const
    {
        return m_parentChild;
    }

    // Every OID that has an object-table root page.  Callers that need to walk
    // the whole tree use this instead of trusting directory entries to carry a
    // usable child identity (see the kf=0x02 ordinal note below).
    std::vector<uint64_t> ObjectIds() const
    {
        std::vector<uint64_t> ids;
        ids.reserve(m_objectRoots.size());
        for (const auto& kv : m_objectRoots) ids.push_back(kv.first);
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    // Resolve a virtual LCN to a physical LCN through the Container Table.
    bool TranslateVlcn(uint64_t vlcn, uint64_t& plcn) const
    {
        // Called during Open() as well as afterwards: readiness is the loaded
        // Container Table, not the fully-completed m_open flag.
        if (m_containerCpc == 0 || m_containerMap.empty()) return false;
        const uint32_t shift = refs_detail::BitLength(m_containerCpc);
        const uint64_t cid = (shift >= 64) ? 0 : (vlcn >> shift);
        auto it = m_containerMap.find(cid);
        if (it == m_containerMap.end()) return false;
        plcn = it->second + (vlcn & (m_containerCpc - 1));
        return plcn < m_info.geometry.totalClusters;
    }

    // Reassemble file bytes.  A gap in the extent map is a hard error: the
    // parser must never disguise missing allocation as zero-filled content.
    bool ReadFile(const RefsDirEntry& entry, uint64_t offset,
                  void* dst, size_t length, size_t* outRead) const
    {
        if (outRead) *outRead = 0;
        if (!m_open) { Fail(RefsStatus::NotOpen, "volume is not open"); return false; }
        if (!entry.dataAvailable) {
            Fail(RefsStatus::DataUnavailable,
                 "file record has no decodable $DATA sub-record");
            return false;
        }
        if (offset >= entry.size || length == 0) return true;
        const uint64_t want = std::min<uint64_t>(length, entry.size - offset);
        uint8_t* out = static_cast<uint8_t*>(dst);

        if (entry.dataInline) {
            if (offset + want > entry.inlineData.size()) {
                Fail(RefsStatus::Truncated, "inline $DATA is shorter than the file size");
                return false;
            }
            std::memcpy(out, entry.inlineData.data() + static_cast<size_t>(offset),
                        static_cast<size_t>(want));
            if (outRead) *outRead = static_cast<size_t>(want);
            return true;
        }

        std::vector<RefsExtent> extents = entry.extents;
        std::sort(extents.begin(), extents.end(),
                  [](const RefsExtent& a, const RefsExtent& b) { return a.vcn < b.vcn; });
        const uint64_t cluster = m_info.geometry.clusterSize;
        if (cluster == 0) { Fail(RefsStatus::UnsupportedGeometry, "zero cluster size"); return false; }

        uint64_t done = 0;
        std::vector<uint8_t> sector(cluster);
        while (done < want) {
            const uint64_t pos = offset + done;
            const uint64_t vcn = pos / cluster;
            const uint64_t inCluster = pos % cluster;
            const RefsExtent* found = nullptr;
            for (const auto& e : extents) {
                if (vcn >= e.vcn && vcn < static_cast<uint64_t>(e.vcn) + e.length) {
                    found = &e;
                    break;
                }
            }
            if (!found) {
                Fail(RefsStatus::HoleDetected,
                     "no extent covers file cluster " + std::to_string(vcn));
                return false;
            }
            uint64_t plcn = 0;
            if (!TranslateVlcn(found->vlcn + (vcn - found->vcn), plcn)) {
                Fail(RefsStatus::HoleDetected,
                     "container table cannot translate VLCN " +
                     std::to_string(found->vlcn + (vcn - found->vcn)));
                return false;
            }
            if (!m_image->Read(plcn * cluster, sector.data(), static_cast<size_t>(cluster))) {
                Fail(RefsStatus::Truncated, "read past end of image while resolving extent");
                return false;
            }
            const uint64_t chunk = std::min<uint64_t>(cluster - inCluster, want - done);
            std::memcpy(out + done, sector.data() + static_cast<size_t>(inCluster),
                        static_cast<size_t>(chunk));
            done += chunk;
        }
        if (outRead) *outRead = static_cast<size_t>(want);
        return true;
    }

private:
    // -- shared state ------------------------------------------------------
    struct PageRef
    {
        uint64_t lcn[4] = {0, 0, 0, 0};
        uint16_t flags = 0;
        uint8_t  cktype = 0;
        uint8_t  dataOffset = 0;
        uint32_t dataLength = 0;
        std::array<uint8_t, 32> digest{};
    };
    struct SuperblockCandidate
    {
        uint64_t lcn = 0;
        uint64_t generation = 0;
        uint64_t chkpLcn[2] = {0, 0};
    };
    struct CheckpointCandidate
    {
        uint64_t lcn = 0;
        uint64_t clock = 0;
        uint32_t flags = 0;
        bool indirect = false;
        std::vector<PageRef> roots;
    };

    using RowVisitor = std::function<RefsStatus(const uint8_t*, size_t,
                                                const uint8_t*, size_t)>;

    void Reset()
    {
        m_image = nullptr;
        m_open = false;
        m_status = RefsStatus::NotOpen;
        m_error.clear();
        m_info = RefsVolumeInfo{};
        m_roots.clear();
        m_containerMap.clear();
        m_objectRoots.clear();
        m_parentChild.clear();
        m_containerCpc = 0;
        m_supb = SuperblockCandidate{};
        m_chkp = CheckpointCandidate{};
    }

    RefsStatus Ok() const { m_status = RefsStatus::Ok; m_error.clear(); return m_status; }
    template <typename T> T Ok(T value) const
    {
        m_status = RefsStatus::Ok; m_error.clear(); return value;
    }
    RefsStatus Fail(RefsStatus s, std::string msg) const
    {
        m_status = s;
        m_error = std::move(msg);
        return s;
    }
    bool FailBool(RefsStatus s, std::string msg) const
    {
        Fail(s, std::move(msg));
        return false;
    }

    bool ReadAt(uint64_t offset, void* dst, size_t length) const
    {
        return m_image && m_image->Read(offset, dst, length);
    }

    // -- VBR ---------------------------------------------------------------
    RefsStatus ReadVolumeBootRecord()
    {
        uint8_t vbr[512] = {};
        if (!ReadAt(0, vbr, sizeof(vbr))) return RefsStatus::IoError;
        if (std::memcmp(vbr + 0x03, "-FVE-FS-", 8) == 0)
            return RefsStatus::BitLockerEncrypted;   // unlock via A1-3 first
        if (std::memcmp(vbr + 0x03, "ReFS\0\0\0\0", 8) != 0)
            return RefsStatus::NotReFS;
        if (std::memcmp(vbr + 0x10, "FSRS", 4) != 0) return RefsStatus::NotReFS;
        if (refs_detail::Le16(vbr + 0x14) != 0x200u) return RefsStatus::NotReFS;
        if (VbrChecksum(vbr) != refs_detail::Le16(vbr + 0x16))
            return RefsStatus::BadVbrChecksum;

        RefsGeometry& g = m_info.geometry;
        g.bytesPerSector    = refs_detail::Le32(vbr + 0x20);
        g.sectorsPerCluster = refs_detail::Le32(vbr + 0x24);
        g.totalSectors      = refs_detail::Le64(vbr + 0x18);
        g.bytesPerContainer = refs_detail::Le64(vbr + 0x40);
        g.majorVersion      = vbr[0x28];
        g.minorVersion      = vbr[0x29];
        g.checksumSelector  = refs_detail::Le16(vbr + 0x2A);
        g.volumeFlags       = refs_detail::Le32(vbr + 0x2C);
        std::memcpy(m_info.volumeGuid.data(), vbr + 0x48, 16);
        m_info.volumeSignature = refs_detail::XorGuidWords(m_info.volumeGuid.data());

        if (g.majorVersion != 3 || g.minorVersion < 10 || g.minorVersion > 14)
            return RefsStatus::UnsupportedVersion;   // legacy v3.4..v3.9 -> A1-5b
        if (g.bytesPerSector == 0 || g.sectorsPerCluster == 0 ||
            g.bytesPerSector > 65536 || g.sectorsPerCluster > 256 ||
            g.bytesPerContainer == 0)
            return RefsStatus::UnsupportedGeometry;
        g.clusterSize = static_cast<uint64_t>(g.bytesPerSector) * g.sectorsPerCluster;
        if (g.clusterSize < 512 || g.clusterSize > 65536 ||
            (g.clusterSize & (g.clusterSize - 1)) != 0)
            return RefsStatus::UnsupportedGeometry;
        g.pageSize = (g.clusterSize == 65536) ? 65536 : 16384;
        g.clustersPerPage = static_cast<uint32_t>(g.pageSize / g.clusterSize);
        g.clustersPerContainer = g.bytesPerContainer / g.clusterSize;
        if (g.clustersPerContainer == 0 ||
            (g.clustersPerContainer & (g.clustersPerContainer - 1)) != 0)
            return RefsStatus::UnsupportedGeometry;
        if (!m_image || m_image->Size() < g.clusterSize) return RefsStatus::Truncated;
        g.totalClusters = m_image->Size() / g.clusterSize;
        // The VBR's declared sector count must agree with the image size.
        if (g.totalSectors == 0 ||
            g.totalSectors > g.totalClusters * g.sectorsPerCluster + g.sectorsPerCluster)
            return RefsStatus::UnsupportedGeometry;

        // CHKP +0x5C is authoritative for the page-reference size; VBR +0x2A
        // only records the format-time algorithm (upgrades never rewrite it).
        return RefsStatus::Ok;
    }

    // -- SUPB --------------------------------------------------------------
    bool VerifySelfChecksum(const uint8_t* block, size_t blockBytes,
                            uint32_t descOff, uint32_t descLen) const
    {
        const uint64_t cluster = m_info.geometry.clusterSize;
        if (descOff == 0 || descLen == 0 || descOff + descLen > cluster ||
            blockBytes < cluster || descOff + 0x2C > cluster)
            return false;
        if (block[descOff + 0x22] != 1 || block[descOff + 0x23] != 0x08 ||
            refs_detail::Le32(block + descOff + 0x24) != 4)
            return false;
        std::vector<uint8_t> tmp(block, block + static_cast<size_t>(cluster));
        std::fill(tmp.begin() + descOff, tmp.begin() + descOff + descLen,
                  static_cast<uint8_t>(0));
        return Crc32C(tmp.data(), tmp.size()) ==
               refs_detail::Le32(block + descOff + 0x28);
    }

    RefsStatus ReadSuperblocks()
    {
        const RefsGeometry& g = m_info.geometry;
        if (g.totalClusters < 40) return RefsStatus::UnsupportedGeometry;
        const uint64_t lcns[3] = {
            kSupbPrimaryLcn, g.totalClusters - 2, g.totalClusters - 3
        };
        std::vector<uint8_t> block(static_cast<size_t>(g.clusterSize));
        bool any = false;
        for (uint64_t lcn : lcns) {
            if (!ReadAt(lcn * g.clusterSize, block.data(), block.size())) continue;
            const uint8_t* b = block.data();
            if (std::memcmp(b, "SUPB", 4) != 0) continue;
            if (refs_detail::Le32(b + 0x04) != 2u) continue;
            if (refs_detail::Le32(b + 0x0C) != m_info.volumeSignature) continue;
            if (refs_detail::Le64(b + 0x20) != lcn) continue;
            const uint32_t descOff = refs_detail::Le32(b + 0x78);
            const uint32_t descLen = refs_detail::Le32(b + 0x7C);
            if (!VerifySelfChecksum(b, block.size(), descOff, descLen)) continue;
            const uint32_t refsOff = refs_detail::Le32(b + 0x70);
            const uint32_t refsCnt = refs_detail::Le32(b + 0x74);
            if (refsCnt != 2 || static_cast<uint64_t>(refsOff) + 16 > g.clusterSize) continue;
            SuperblockCandidate c;
            c.lcn = lcn;
            c.generation = refs_detail::Le64(b + 0x68);
            c.chkpLcn[0] = refs_detail::Le64(b + refsOff);
            c.chkpLcn[1] = refs_detail::Le64(b + refsOff + 8);
            if (!any || c.generation > m_supb.generation) m_supb = c;
            any = true;
        }
        if (!any) return RefsStatus::NoValidSuperblock;
        m_info.superblockLcn = m_supb.lcn;
        m_info.superblockGeneration = m_supb.generation;
        return RefsStatus::Ok;
    }

    // -- CHKP --------------------------------------------------------------
    bool ParsePageRef(const uint8_t* p, size_t avail, PageRef& out) const
    {
        const size_t size = m_info.pageRefSize;
        if (size == 0 || avail < size || size < 0x30) return false;
        for (int i = 0; i < 4; ++i) out.lcn[i] = refs_detail::Le64(p + 8 * i);
        out.flags      = refs_detail::Le16(p + 0x20);
        out.cktype     = p[0x22];
        out.dataOffset = p[0x23];
        out.dataLength = refs_detail::Le32(p + 0x24);
        out.digest.fill(0);
        if (out.dataLength == 0 || out.dataLength > 32 ||
            0x28 + out.dataLength > size) return false;
        std::memcpy(out.digest.data(), p + 0x28, out.dataLength);
        return true;
    }

    RefsStatus ReadCheckpoints()
    {
        const RefsGeometry& g = m_info.geometry;
        std::vector<uint8_t> page(static_cast<size_t>(g.pageSize));
        bool any = false;
        for (int k = 0; k < 2; ++k) {
            const uint64_t lcn = m_supb.chkpLcn[k];
            if (lcn == 0 || lcn + g.clustersPerPage > g.totalClusters) continue;
            if (!ReadAt(lcn * g.clusterSize, page.data(), page.size())) continue;
            const uint8_t* b = page.data();
            if (std::memcmp(b, "CHKP", 4) != 0) continue;
            if (refs_detail::Le32(b + 0x04) != 2u) continue;
            if (refs_detail::Le32(b + 0x0C) != m_info.volumeSignature) continue;
            if (refs_detail::Le64(b + 0x20) != lcn) continue;
            const uint32_t refSize = refs_detail::Le32(b + 0x5C);
            if (refSize != 0x30 && refSize != 0x48) continue;   // 0x68 legacy refused
            m_info.pageRefSize = refSize;
            const uint32_t descOff = refs_detail::Le32(b + 0x58);
            if (descOff == 0 || descOff + refSize > g.pageSize) continue;
            if (!VerifySelfChecksum(b, page.size(), descOff, refSize)) continue;
            const uint32_t rootCount = refs_detail::Le32(b + 0x90);
            if (rootCount != 13) continue;
            const uint32_t flags = refs_detail::Le32(b + 0x78);
            const bool indirect = (flags & 0x0200u) != 0;
            const uint32_t arrayOff = indirect ? refs_detail::Le32(b + 0x94) : 0x94;
            if (arrayOff + 13u * 4u > g.pageSize) continue;

            CheckpointCandidate c;
            c.lcn = lcn;
            c.clock = refs_detail::Le64(b + 0x60);
            c.flags = flags;
            c.indirect = indirect;
            c.roots.reserve(13);
            bool ok = true;
            for (uint32_t i = 0; i < 13; ++i) {
                const uint32_t refOff = refs_detail::Le32(b + arrayOff + 4u * i);
                if (refOff + refSize > g.pageSize) { ok = false; break; }
                PageRef pr;
                if (!ParsePageRef(b + refOff, static_cast<size_t>(g.pageSize - refOff), pr)) {
                    ok = false; break;
                }
                c.roots.push_back(pr);
            }
            if (!ok || c.roots.size() != 13) continue;
            if (!any || c.clock > m_chkp.clock) m_chkp = c;
            any = true;
        }
        if (!any) return RefsStatus::NoValidCheckpoint;
        m_info.checkpointLcn = m_chkp.lcn;
        m_info.checkpointClock = m_chkp.clock;
        m_info.checkpointFlags = m_chkp.flags;
        m_info.indirectRootList = m_chkp.indirect;
        m_info.rootCount = 13;
        // cktype from the first root; all roots on a volume share it.
        m_info.checksumType = m_chkp.roots[0].cktype;
        m_roots = m_chkp.roots;
        return RefsStatus::Ok;
    }

    // -- pages / trees -----------------------------------------------------
    bool ResolveLcn(uint64_t lcn, bool virtualAddr, uint64_t& plcn) const
    {
        if (virtualAddr) return TranslateVlcn(lcn, plcn);
        if (lcn >= m_info.geometry.totalClusters) return false;
        plcn = lcn;
        return true;
    }

    RefsStatus ReadPage(const PageRef& ref, bool virtualAddr,
                        const char* expectedSig,
                        std::vector<uint8_t>& out) const
    {
        const RefsGeometry& g = m_info.geometry;
        out.clear();
        out.reserve(static_cast<size_t>(g.pageSize));
        std::vector<uint8_t> cluster(static_cast<size_t>(g.clusterSize));
        for (uint32_t i = 0; i < g.clustersPerPage; ++i) {
            if (g.clustersPerPage > 1 && ref.lcn[i] != ref.lcn[0] + i)
                return Fail(RefsStatus::BadTree,
                            "page-reference LCN slot " + std::to_string(i) +
                            " is not consecutive (base " + std::to_string(ref.lcn[0]) +
                            ", slot " + std::to_string(ref.lcn[i]) + ")");
            uint64_t plcn = 0;
            if (!ResolveLcn(ref.lcn[i], virtualAddr, plcn))
                return Fail(RefsStatus::BadTree,
                            "cannot resolve " + std::string(virtualAddr ? "virtual " : "physical ") +
                            "LCN " + std::to_string(ref.lcn[i]) + " (cpc=" +
                            std::to_string(m_containerCpc) + ", map=" +
                            std::to_string(m_containerMap.size()) + ")");
            if (!ReadAt(plcn * g.clusterSize, cluster.data(), cluster.size()))
                return RefsStatus::Truncated;
            out.insert(out.end(), cluster.begin(), cluster.end());
        }
        if (out.size() < 0x50) return RefsStatus::Truncated;
        if (expectedSig && std::memcmp(out.data(), expectedSig, 4) != 0)
            return RefsStatus::BadPageSignature;
        if (refs_detail::Le32(out.data() + 0x04) != 2u)
            return RefsStatus::BadPageSignature;
        if (refs_detail::Le32(out.data() + 0x0C) != m_info.volumeSignature)
            return RefsStatus::BadVolumeSignature;
        if (refs_detail::Le64(out.data() + 0x20) != ref.lcn[0])
            return RefsStatus::BadPageSignature;

        if (ref.cktype == 2) {
            if (ref.dataLength != 8) return RefsStatus::UnsupportedVariant;
            if (Crc64Nvme(out.data(), out.size()) !=
                refs_detail::Le64(ref.digest.data()))
                return RefsStatus::ChecksumMismatch;
        } else if (ref.cktype == 4) {
            if (ref.dataLength != 32) return RefsStatus::UnsupportedVariant;
            const Hash32 h = Sha256Of(out.data(), out.size());
            if (std::memcmp(h.data(), ref.digest.data(), 32) != 0)
                return RefsStatus::ChecksumMismatch;
        } else if (ref.cktype == 0) {
            // Pre-3.10 driver used a stub verifier; digests were written but
            // not checked. We still refuse to treat an unknown type as valid.
            return RefsStatus::UnsupportedVariant;
        } else {
            return RefsStatus::UnsupportedVariant;
        }
        return RefsStatus::Ok;
    }

    RefsStatus WalkTree(const PageRef& root, bool virtualAddr,
                        const RowVisitor& visit) const
    {
        std::unordered_set<uint64_t> visited;
        return WalkTreeNode(root, virtualAddr, visit, visited, 0);
    }

    RefsStatus WalkTreeNode(const PageRef& ref, bool virtualAddr,
                            const RowVisitor& visit,
                            std::unordered_set<uint64_t>& visited,
                            uint32_t depth) const
    {
        if (depth > 12) return RefsStatus::BadTree;
        if (!visited.insert(ref.lcn[0]).second) return RefsStatus::BadTree;
        std::vector<uint8_t> page;
        const RefsStatus rs = ReadPage(ref, virtualAddr, "MSB+", page);
        if (rs != RefsStatus::Ok) return rs;
        const uint8_t* p = page.data();
        const size_t n = page.size();
        if (n < 0x50 + 4) return RefsStatus::Truncated;
        const uint32_t nhOff = refs_detail::Le32(p + 0x50);
        const size_t nh = 0x50u + nhOff;
        if (nh + 0x28 > n) return RefsStatus::BadTree;

        const uint8_t level = p[nh + 0x0C];
        const uint32_t idxStart = refs_detail::Le32(p + nh + 0x10);
        const uint32_t count    = refs_detail::Le32(p + nh + 0x14);
        const uint32_t idxEnd   = refs_detail::Le32(p + nh + 0x20);
        if (idxEnd < idxStart || idxEnd - idxStart != count * 4u)
            return RefsStatus::BadNodeInvariant;
        if (count > 4096 || nh + idxEnd > n) return RefsStatus::BadTree;

        for (uint32_t i = 0; i < count; ++i) {
            const size_t ent = nh + idxStart + 4ull * i;
            if (ent + 4 > n) return RefsStatus::BadTree;
            const uint16_t rowOff = refs_detail::Le16(p + ent);
            const size_t row = nh + rowOff;
            if (row + 0x10 > n) return RefsStatus::BadTree;
            const uint32_t rowSize = refs_detail::Le32(p + row);
            const uint16_t keyOff  = refs_detail::Le16(p + row + 0x04);
            const uint16_t keyLen  = refs_detail::Le16(p + row + 0x06);
            const uint16_t valOff  = refs_detail::Le16(p + row + 0x0A);
            const uint16_t valLen  = refs_detail::Le16(p + row + 0x0C);
            if (rowSize < 0x10 || row + rowSize > n) return RefsStatus::BadTree;
            if (static_cast<uint64_t>(keyOff) + keyLen > rowSize ||
                static_cast<uint64_t>(valOff) + valLen > rowSize)
                return RefsStatus::BadTree;
            const uint8_t* key = p + row + keyOff;
            const uint8_t* val = p + row + valOff;
            if (level == 0) {
                const RefsStatus r = visit(key, keyLen, val, valLen);
                if (r != RefsStatus::Ok) return r;
            } else {
                if (valLen < m_info.pageRefSize) return RefsStatus::BadTree;
                PageRef child;
                if (!ParsePageRef(val, valLen, child)) return RefsStatus::BadTree;
                const RefsStatus r = WalkTreeNode(child, virtualAddr, visit, visited, depth + 1);
                if (r != RefsStatus::Ok) return r;
            }
        }
        return RefsStatus::Ok;
    }

    RefsStatus VerifyRootPages() const
    {
        if (m_roots.size() != 13) return RefsStatus::NoValidCheckpoint;
        // Failover pairs: object table (0,5) and container table (7,8).
        auto verifyOne = [&](size_t i) -> RefsStatus {
            const bool virtualAddr = !(i == 7 || i == 8 || i == 12);
            std::vector<uint8_t> page;
            return ReadPage(m_roots[i], virtualAddr, "MSB+", page);
        };
        for (size_t i = 0; i < 13; ++i) {
            if (i == 5 || i == 8) continue;   // handled as the second of a pair
            RefsStatus a = RefsStatus::Ok, b = RefsStatus::Ok;
            if (i == 0) {
                a = verifyOne(0); b = verifyOne(5);
                if (a == RefsStatus::Ok || b == RefsStatus::Ok) continue;
            } else if (i == 7) {
                a = verifyOne(7); b = verifyOne(8);
                if (a == RefsStatus::Ok || b == RefsStatus::Ok) continue;
            } else {
                a = verifyOne(i);
                if (a == RefsStatus::Ok) continue;
            }
            return Fail(RefsStatus::ChecksumMismatch,
                        "root #" + std::to_string(i) +
                        " page verification failed (" + RefsStatusName(a) +
                        (b != RefsStatus::Ok ? std::string("/") + RefsStatusName(b) : "") +
                        "): " + m_error);
        }
        return RefsStatus::Ok;
    }

    // -- global tables -----------------------------------------------------
    RefsStatus LoadContainerTable()
    {
        // Bootstrap exception: roots 7/8 use real (physical) LCNs.
        RefsStatus last = RefsStatus::BadContainerTable;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const size_t idx = (attempt == 0) ? 7u : 8u;
            std::unordered_map<uint64_t, uint64_t> map;
            uint64_t cpc = 0;
            const RefsStatus s = WalkTree(m_roots[idx], false,
                [&](const uint8_t* key, size_t keyLen,
                    const uint8_t* val, size_t valLen) -> RefsStatus {
                    if (keyLen < 16 || valLen < 48) return RefsStatus::BadContainerTable;
                    const uint64_t cid = refs_detail::Le64(key);
                    if (refs_detail::Le64(key + 8) != kContainerTag)
                        return RefsStatus::BadContainerTable;
                    if (valLen != 160 && valLen != 224) return RefsStatus::UnsupportedVariant;
                    const uint32_t rowCpc = refs_detail::Le32(val + 0x18);
                    // Physical start is position-dependent: value[len-16].
                    const uint64_t phys = refs_detail::Le64(val + valLen - 16);
                    if (rowCpc == 0 || (rowCpc & (rowCpc - 1)) != 0 || phys == 0)
                        return RefsStatus::BadContainerTable;
                    if (cpc == 0) cpc = rowCpc;
                    if (rowCpc != cpc) return RefsStatus::BadContainerTable;
                    map[cid] = phys;
                    return RefsStatus::Ok;
                });
            if (s != RefsStatus::Ok) { last = s; continue; }
            if (map.empty()) { last = RefsStatus::BadContainerTable; continue; }
            m_containerMap = std::move(map);
            m_containerCpc = cpc;
            m_info.containerCount = m_containerMap.size();
            return RefsStatus::Ok;
        }
        return last;
    }

    RefsStatus LoadObjectTable()
    {
        RefsStatus last = RefsStatus::BadObjectTable;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const size_t idx = (attempt == 0) ? 0u : 5u;
            std::unordered_map<uint64_t, PageRef> roots;
            const RefsStatus s = WalkTree(m_roots[idx], true,
                [&](const uint8_t* key, size_t keyLen,
                    const uint8_t* val, size_t valLen) -> RefsStatus {
                    if (keyLen < 16) return RefsStatus::BadObjectTable;
                    const uint64_t oid = refs_detail::Le64(key + 8);
                    if (oid == 0) return RefsStatus::BadObjectTable;
                    if (valLen >= 240) return RefsStatus::UnsupportedVariant; // legacy v3.4
                    if (valLen < 0x20 + m_info.pageRefSize) return RefsStatus::BadObjectTable;
                    if (refs_detail::Le32(val) != 2u) return RefsStatus::UnsupportedVariant;
                    PageRef pr;
                    if (!ParsePageRef(val + 0x20, valLen - 0x20, pr))
                        return RefsStatus::BadObjectTable;
                    roots[oid] = pr;
                    return RefsStatus::Ok;
                });
            if (s != RefsStatus::Ok) { last = s; continue; }
            if (roots.empty()) { last = RefsStatus::BadObjectTable; continue; }
            m_objectRoots = std::move(roots);
            m_info.objectCount = m_objectRoots.size();
            return RefsStatus::Ok;
        }
        return last;
    }

    RefsStatus LoadParentChild()
    {
        std::vector<std::pair<uint64_t, uint64_t>> links;
        const RefsStatus s = WalkTree(m_roots[4], true,
            [&](const uint8_t* key, size_t keyLen,
                const uint8_t* /*val*/, size_t /*valLen*/) -> RefsStatus {
                if (keyLen != 32) return RefsStatus::BadTree;
                links.emplace_back(refs_detail::Le64(key + 0x08),
                                   refs_detail::Le64(key + 0x18));
                return RefsStatus::Ok;
            });
        if (s != RefsStatus::Ok) return s;
        m_parentChild = std::move(links);
        m_info.parentChildCount = m_parentChild.size();
        return RefsStatus::Ok;
    }

    // -- directory entries -------------------------------------------------
    RefsStatus ParseDirEntry(uint64_t parentOid,
                             const uint8_t* key, size_t keyLen,
                             const uint8_t* val, size_t valLen,
                             RefsDirEntry& out) const
    {
        const uint16_t keyFlags = refs_detail::Le16(key + 0x02);
        if ((keyLen - 4) % 2 != 0) return RefsStatus::BadTree;
        out = RefsDirEntry{};
        out.parentObjectId = parentOid;
        out.name = refs_detail::Utf16ToWide(key + 4, keyLen - 4);
        out.recordSplit = (keyFlags == 0x02);

        if (keyFlags == 0x01) {
            if (valLen < 0xA8) return RefsStatus::BadTree;
            out.createTime = refs_detail::Le64(val + 0x28);
            out.modifyTime = refs_detail::Le64(val + 0x30);
            out.metaTime   = refs_detail::Le64(val + 0x38);
            out.accessTime = refs_detail::Le64(val + 0x40);
            out.attributes = refs_detail::Le32(val + 0x48);
            out.size       = refs_detail::Le64(val + 0x58);
            out.allocated  = refs_detail::Le64(val + 0x60);
            out.homeObjectId = parentOid;
            out.childOrdinal = refs_detail::Le64(val + 0x80);
            return ParseSubRecord(val + 0xA8, valLen - 0xA8, out);
        }
        if (keyFlags == 0x02) {
            if (valLen < 72 || valLen > 84) return RefsStatus::UnsupportedVariant;
            out.childOrdinal = refs_detail::Le64(val + 0x00);
            out.homeObjectId = refs_detail::Le64(val + 0x08);
            out.createTime   = refs_detail::Le64(val + 0x10);
            out.modifyTime   = refs_detail::Le64(val + 0x18);
            out.metaTime     = refs_detail::Le64(val + 0x20);
            out.accessTime   = refs_detail::Le64(val + 0x28);
            out.allocated    = refs_detail::Le64(val + 0x30);
            out.size         = refs_detail::Le64(val + 0x38);
            out.attributes   = refs_detail::Le32(val + 0x40);
            out.dataAvailable = false;   // split backing record -> A1-5b
            out.isDirectory = (out.attributes & kDirectoryAttributeBit) != 0;
            return RefsStatus::Ok;
        }
        return RefsStatus::UnsupportedVariant;
    }

    RefsStatus ParseSubRecord(const uint8_t* p, size_t len, RefsDirEntry& out) const
    {
        if (len < kSubRecordHeader) return RefsStatus::DataUnavailable;
        const uint32_t marker = refs_detail::Le32(p);
        const uint32_t descriptor = refs_detail::Le32(p + 4);
        if (descriptor != kDataDescriptor) return RefsStatus::UnsupportedVariant;
        const uint64_t streamSize = refs_detail::Le64(p + 0x08);
        const uint64_t diskAllocated = refs_detail::Le64(p + 0x10);
        const uint32_t contentOff = refs_detail::Le32(p + 0x18);
        const uint32_t extentCount = refs_detail::Le32(p + 0x1C);
        if (contentOff < kSubRecordHeader || contentOff > len)
            return RefsStatus::UnsupportedVariant;

        if (marker == kDataMarkerInline) {
            if (diskAllocated != 0 || extentCount != 0) return RefsStatus::UnsupportedVariant;
            if (streamSize > len - contentOff) return RefsStatus::Truncated;
            out.dataInline = true;
            out.dataAvailable = true;
            out.inlineData.assign(p + contentOff,
                                  p + contentOff + static_cast<size_t>(streamSize));
            return RefsStatus::Ok;
        }
        if (marker == kDataMarkerExtent) {
            const uint64_t needed = static_cast<uint64_t>(extentCount) * kExtentEntrySize;
            if (extentCount == 0 || needed > len - contentOff)
                return RefsStatus::UnsupportedVariant;
            for (uint32_t i = 0; i < extentCount; ++i) {
                const uint8_t* e = p + contentOff + static_cast<size_t>(i) * kExtentEntrySize;
                const uint16_t recSize = refs_detail::Le16(e + 0x0A);
                if (recSize < kExtentEntrySize) return RefsStatus::UnsupportedVariant;
                const uint64_t step = refs_detail::Align8(recSize);
                if (static_cast<uint64_t>(i) * step + 24 > needed)
                    return RefsStatus::UnsupportedVariant;
                RefsExtent ex;
                ex.vlcn   = refs_detail::Le64(e);
                ex.vcn    = refs_detail::Le32(e + 0x0C);
                ex.length = refs_detail::Le32(e + 0x14);
                if (ex.length == 0) return RefsStatus::BadTree;
                out.extents.push_back(ex);
            }
            (void)streamSize;
            out.dataInline = false;
            out.dataAvailable = true;
            return RefsStatus::Ok;
        }
        return RefsStatus::UnsupportedVariant;
    }

    // -- state -------------------------------------------------------------
    const IRefsImage* m_image = nullptr;
    bool m_open = false;
    mutable RefsStatus m_status = RefsStatus::NotOpen;
    mutable std::string m_error;
    RefsVolumeInfo m_info;
    SuperblockCandidate m_supb;
    CheckpointCandidate m_chkp;
    std::vector<PageRef> m_roots;
    std::unordered_map<uint64_t, uint64_t> m_containerMap;  // container id -> PLCN
    uint64_t m_containerCpc = 0;
    std::unordered_map<uint64_t, PageRef> m_objectRoots;    // OID -> tree root
    std::vector<std::pair<uint64_t, uint64_t>> m_parentChild;
};

} // namespace refs
} // namespace phoenix
