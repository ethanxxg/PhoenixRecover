#include "../common/Platform.h"

#if PHOENIX_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#endif

#include "RecoverEngine.h"
#include "RecoverOutputFile.h"
#include "FragmentStitchService.h"
#include "../common/Utils.h"
#include "../log/Logger.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <sstream>

namespace {

// MinGW g++ 7.3 (the build-ac toolchain) ships no <filesystem>.  The recovery
// engine only needed file_size() and remove(); use platform primitives so the
// whole recovery chain stays buildable on both GCC 7 and modern toolchains.
static uint64_t RecoveredFileSize(const std::wstring& path)
{
#if PHOENIX_OS_WINDOWS
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return 0;
    return (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) |
           static_cast<uint64_t>(fad.nFileSizeLow);
#else
    std::ifstream f(Utils::WideToUtf8(path), std::ios::binary | std::ios::ate);
    if (!f) return 0;
    const std::streamoff n = f.tellg();
    return n > 0 ? static_cast<uint64_t>(n) : 0;
#endif
}

static void RemoveRecoveredFile(const std::wstring& path)
{
#if PHOENIX_OS_WINDOWS
    DeleteFileW(path.c_str());
#else
    std::remove(Utils::WideToUtf8(path).c_str());
#endif
}

#if PHOENIX_OS_WINDOWS
typedef LONG(WINAPI* FnRtlDecompressBuffer)(USHORT CompressionFormat, PUCHAR UncompressedBuffer,
                                            ULONG UncompressedBufferSize, PUCHAR CompressedBuffer,
                                            ULONG CompressedBufferSize, PULONG FinalUncompressedSize);

static FnRtlDecompressBuffer GetLznt1Fn()
{
    static FnRtlDecompressBuffer s_fn = nullptr;
    if (!s_fn) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            s_fn = reinterpret_cast<FnRtlDecompressBuffer>(
                GetProcAddress(ntdll, "RtlDecompressBuffer"));
        }
    }
    return s_fn;
}
#endif

bool DecompressLznt1Unit(const uint8_t* compressedBuf, uint32_t compressedSize, uint32_t unitBytes,
                         uint8_t* outBuf, uint32_t& outLen)
{
#if PHOENIX_OS_WINDOWS
    FnRtlDecompressBuffer fn = GetLznt1Fn();
    if (!fn) {
        const uint32_t toCopy = std::min(compressedSize, unitBytes);
        memcpy(outBuf, compressedBuf, toCopy);
        if (toCopy < unitBytes)
            memset(outBuf + toCopy, 0, unitBytes - toCopy);
        outLen = unitBytes;
        return false;
    }
    ULONG finalLen = 0;
    const LONG st =
        fn(2, outBuf, unitBytes, const_cast<PUCHAR>(compressedBuf), compressedSize, &finalLen);
    if (st != 0) {
        memset(outBuf, 0, unitBytes);
        outLen = unitBytes;
        return false;
    }
    outLen = finalLen;
    return true;
#else
    (void)compressedBuf;
    (void)compressedSize;
    memset(outBuf, 0, unitBytes);
    outLen = unitBytes;
    return false;
#endif
}

struct FsVolumeHints {
    bool     isNtfs = false;
    bool     isExt4 = false;
    bool     isExfat = false;
    uint32_t bytesPerCluster = 0;
    uint32_t ext4BlockSize   = 0;
};

static FsVolumeHints ParseFsVolumeHints(const uint8_t* bootBuf, uint32_t ss)
{
    FsVolumeHints h;
    if (memcmp(bootBuf + 3, "NTFS    ", 8) == 0) {
        h.isNtfs = true;
        const uint16_t bps = Utils::ReadLE16(bootBuf + 11);
        const uint8_t  spc = bootBuf[13];
        h.bytesPerCluster = static_cast<uint32_t>(bps) * spc;
        return h;
    }
    if (memcmp(bootBuf + 3, "EXFAT   ", 8) == 0) {
        h.isExfat = true;
        const uint8_t bpsShift = bootBuf[108];
        const uint8_t spcShift = bootBuf[109];
        if (bpsShift <= 16 && spcShift <= 16)
            h.bytesPerCluster = (1u << bpsShift) * (1u << spcShift);
        if (h.bytesPerCluster == 0)
            h.bytesPerCluster = ss;
        return h;
    }
    const uint16_t ext4Magic = Utils::ReadLE16(bootBuf + 1024 + 0x38);
    if (ext4Magic == 0xEF53) {
        h.isExt4 = true;
        const uint32_t logBlockSize = Utils::ReadLE32(bootBuf + 1024 + 0x18);
        h.ext4BlockSize = 1024u << logBlockSize;
        return h;
    }
    uint16_t bps = Utils::ReadLE16(bootBuf + 11);
    uint8_t  spc = bootBuf[13];
    if (bps == 0)
        bps = static_cast<uint16_t>(ss);
    if (spc == 0)
        spc = 1;
    h.bytesPerCluster = static_cast<uint32_t>(bps) * spc;
    return h;
}

// FatParser / ExfatParser store absolute disk sector numbers in clusters[].
static uint64_t FatExfatClusterByteOffset(uint64_t absSector, uint32_t ss)
{
    return absSector * static_cast<uint64_t>(ss);
}

static bool IsFatFsType(const std::wstring& fsType)
{
    return fsType.find(L"FAT") != std::wstring::npos;
}

static uint64_t FatClusterChainBytes(const FileNode& node, uint32_t bytesPerCluster)
{
    if (bytesPerCluster == 0 || node.clusters.empty())
        return 0;
    return node.clusters.size() * static_cast<uint64_t>(bytesPerCluster);
}

static bool AcceptPartialFatDeleted(const FileNode& node, const Partition& part,
                                    uint64_t got, uint32_t bytesPerCluster)
{
    if (node.status != FileStatus::Deleted)
        return false;
    if (!IsFatFsType(part.fsType) && !IsFatFsType(node.fsType))
        return false;
    if (got == 0 || got >= node.size)
        return false;
    const uint64_t chainMax = FatClusterChainBytes(node, bytesPerCluster);
    if (chainMax == 0)
        return false;
    const uint64_t expected = (std::min)(node.size, chainMax);
    return got == expected;
}

static FsVolumeHints ReadPartitionHints(SectorReader& reader, const Partition& part)
{
    FsVolumeHints h;
    const uint32_t ss = reader.GetSectorSize() ? reader.GetSectorSize() : 512;
    std::vector<uint8_t> boot(static_cast<size_t>(ss) * 2);
    if (!reader.ReadBytes(part.startSector * ss, boot.data(),
                          static_cast<uint32_t>(boot.size()))) {
        return h;
    }
    return ParseFsVolumeHints(boot.data(), ss);
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// TryPreviewSingleReadSlice – one ReadBytes covers the start of the file payload
// ─────────────────────────────────────────────────────────────────────────────
bool RecoverEngine::TryPreviewSingleReadSlice(SectorReader& reader,
                                               const Partition& partition,
                                               const FileNode& node,
                                               uint32_t maxBytes,
                                               uint64_t& outVolumeByteOffset,
                                               uint32_t& outLength)
{
    outVolumeByteOffset = 0;
    outLength = 0;
    if (node.isDirectory || maxBytes == 0) {
        return false;
    }
    if (!node.residentData.empty()) {
        return false;
    }
    if (node.isNtfsCompressed) {
        return false;
    }

    uint64_t logicalNeed = maxBytes;
    if (node.size > 0) {
        logicalNeed = std::min<uint64_t>(logicalNeed, node.size);
    }
    if (logicalNeed == 0) {
        return false;
    }

    if (node.clusters.empty() && node.rawSize > 0) {
        uint64_t n = std::min<uint64_t>(logicalNeed, node.rawSize);
        if (n == 0) {
            return false;
        }
        if (n > UINT32_MAX) {
            n = UINT32_MAX;
        }
        outVolumeByteOffset = node.rawOffset;
        outLength = static_cast<uint32_t>(n);
        return true;
    }

    if (node.clusters.empty()) {
        return false;
    }
    if (node.clusters[0] == 0) {
        return false;
    }

    const uint32_t ss = reader.GetSectorSize();
    if (ss == 0) {
        return false;
    }

    uint8_t bootBuf[2048]{};
    if (!reader.ReadBytes(partition.startSector * ss, bootBuf, 2048)) {
        return false;
    }

    const FsVolumeHints hints = ParseFsVolumeHints(bootBuf, ss);

    if (hints.isNtfs && hints.bytesPerCluster > 0) {
        const uint64_t firstLcn = node.clusters[0];
        uint64_t runBytes = 0;
        uint64_t expected = firstLcn;
        for (size_t idx = 0; idx < node.clusters.size() && runBytes < logicalNeed; ++idx) {
            const uint64_t c = node.clusters[idx];
            if (c == 0 || c != expected) {
                break;
            }
            runBytes += hints.bytesPerCluster;
            ++expected;
        }
        if (runBytes == 0) {
            return false;
        }
        uint64_t take = std::min(logicalNeed, runBytes);
        if (take > UINT32_MAX) {
            take = UINT32_MAX;
        }
        outVolumeByteOffset =
            partition.startSector * ss + firstLcn * hints.bytesPerCluster;
        outLength = static_cast<uint32_t>(take);
        return true;
    }

    if (hints.isExt4 && hints.ext4BlockSize > 0) {
        const uint64_t firstBlk = node.clusters[0];
        uint64_t runBytes = 0;
        uint64_t expected = firstBlk;
        for (size_t idx = 0; idx < node.clusters.size() && runBytes < logicalNeed; ++idx) {
            const uint64_t b = node.clusters[idx];
            if (b != expected) {
                break;
            }
            runBytes += hints.ext4BlockSize;
            ++expected;
        }
        if (runBytes == 0) {
            return false;
        }
        uint64_t take = std::min(logicalNeed, runBytes);
        if (take > UINT32_MAX) {
            take = UINT32_MAX;
        }
        outVolumeByteOffset =
            partition.startSector * ss + firstBlk * hints.ext4BlockSize;
        outLength = static_cast<uint32_t>(take);
        return true;
    }

    const uint64_t firstSec = node.clusters[0];
    uint64_t runBytes = 0;
    uint64_t expectedSec = firstSec;
    for (size_t idx = 0; idx < node.clusters.size() && runBytes < logicalNeed; ++idx) {
        const uint64_t sec = node.clusters[idx];
        if (sec != expectedSec) {
            break;
        }
        runBytes += ss;
        ++expectedSec;
    }
    if (runBytes == 0) {
        return false;
    }
    uint64_t take = std::min(logicalNeed, runBytes);
    if (take > UINT32_MAX) {
        take = UINT32_MAX;
    }
    outVolumeByteOffset = firstSec * ss;
    outLength = static_cast<uint32_t>(take);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReadFileDataPrefix – first N bytes for UI preview
// ─────────────────────────────────────────────────────────────────────────────
bool RecoverEngine::ReadFileDataPrefix(SectorReader&          reader,
                                        const Partition&     partition,
                                        const FileNode&      node,
                                        uint32_t             maxBytes,
                                        std::vector<uint8_t>& out)
{
    out.clear();
    if (node.isDirectory || maxBytes == 0) {
        return false;
    }

    // ── Resident ─────────────────────────────────────────────────────────
    if (!node.residentData.empty()) {
        const uint64_t n =
            std::min(static_cast<uint64_t>(maxBytes),
                     static_cast<uint64_t>(node.residentData.size()));
        out.assign(node.residentData.begin(),
                   node.residentData.begin() + static_cast<std::ptrdiff_t>(n));
        return !out.empty();
    }

    // ── RAW contiguous ───────────────────────────────────────────────────
    if (node.clusters.empty() && node.rawSize > 0) {
        const uint64_t toRead = std::min(static_cast<uint64_t>(maxBytes), node.rawSize);
        out.resize(static_cast<size_t>(toRead));
        if (!reader.ReadBytes(node.rawOffset, out.data(),
                              static_cast<uint32_t>(toRead))) {
            out.clear();
            return false;
        }
        return true;
    }

    if (node.clusters.empty()) {
        return false;
    }

    const uint64_t byteLimit =
        node.size > 0
            ? std::min(static_cast<uint64_t>(maxBytes), node.size)
            : maxBytes;
    if (byteLimit == 0) {
        return false;
    }

    const uint32_t ss = reader.GetSectorSize();
    uint8_t bootBuf[2048]{};
    reader.ReadBytes(partition.startSector * ss, bootBuf, 2048);

    const FsVolumeHints hints = ParseFsVolumeHints(bootBuf, ss);

    uint64_t written64 = 0;

    // ── NTFS compressed ───────────────────────────────────────────────────
    if (node.isNtfsCompressed && hints.isNtfs && hints.bytesPerCluster > 0
        && node.compressionUnit > 0)
    {
        const uint32_t cuClusters = node.compressionUnit;
        const uint32_t unitBytes = cuClusters * hints.bytesPerCluster;

        std::vector<uint8_t> rawUnitBuf(unitBytes);
        std::vector<uint8_t> decompBuf(unitBytes);

        const auto& clus = node.clusters;
        size_t ci = 0;

        while (ci < clus.size() && written64 < byteLimit) {
            const size_t unitEnd = std::min(ci + cuClusters, clus.size());
            uint32_t realCount = 0;
            uint32_t sparseCount = 0;

            for (size_t k = ci; k < unitEnd; ++k) {
                if (clus[k] == 0) {
                    ++sparseCount;
                } else {
                    ++realCount;
                }
            }

            if (sparseCount == 0) {
                const uint64_t toWriteUnit =
                    std::min(static_cast<uint64_t>(unitBytes), byteLimit - written64);
                uint64_t unitWritten = 0;

                for (size_t k = ci; k < unitEnd && unitWritten < toWriteUnit; ++k) {
                    const uint64_t readOffset = partition.startSector * ss
                                                + clus[k] * hints.bytesPerCluster;
                    const uint32_t toWrite = static_cast<uint32_t>(std::min(
                        static_cast<uint64_t>(hints.bytesPerCluster),
                        toWriteUnit - unitWritten));
                    rawUnitBuf.resize(hints.bytesPerCluster);
                    if (!reader.ReadBytes(readOffset, rawUnitBuf.data(),
                                          hints.bytesPerCluster)) {
                        memset(rawUnitBuf.data(), 0, hints.bytesPerCluster);
                    }
                    const size_t oldSz = out.size();
                    out.resize(oldSz + static_cast<size_t>(toWrite));
                    memcpy(out.data() + oldSz, rawUnitBuf.data(), toWrite);
                    unitWritten += toWrite;
                }
                written64 += unitWritten;
            } else {
                const uint32_t compressedBytes = realCount * hints.bytesPerCluster;
                rawUnitBuf.resize(compressedBytes);
                uint32_t rawPos = 0;

                for (size_t k = ci; k < unitEnd; ++k) {
                    if (clus[k] == 0) {
                        continue;
                    }
                    const uint64_t readOffset = partition.startSector * ss
                                                + clus[k] * hints.bytesPerCluster;
                    if (!reader.ReadBytes(readOffset,
                                          rawUnitBuf.data() + rawPos,
                                          hints.bytesPerCluster)) {
                        memset(rawUnitBuf.data() + rawPos, 0, hints.bytesPerCluster);
                    }
                    rawPos += hints.bytesPerCluster;
                }

                decompBuf.resize(unitBytes);
                uint32_t outLen = 0;
                DecompressLznt1Unit(rawUnitBuf.data(), compressedBytes,
                                      unitBytes, decompBuf.data(), outLen);

                const uint64_t toCopy =
                    std::min(static_cast<uint64_t>(outLen), byteLimit - written64);
                const size_t oldSz = out.size();
                out.resize(oldSz + static_cast<size_t>(toCopy));
                memcpy(out.data() + oldSz, decompBuf.data(), static_cast<size_t>(toCopy));
                written64 += toCopy;
            }

            ci = unitEnd;
        }

        return !out.empty();
    }

    // ── Regular cluster / sector chain ────────────────────────────────────
    std::vector<uint8_t> buf(
        std::max<uint32_t>(ss, hints.bytesPerCluster > 0 ? hints.bytesPerCluster : ss));
    if (hints.isExt4 && hints.ext4BlockSize > 0 && hints.ext4BlockSize > buf.size()) {
        buf.resize(hints.ext4BlockSize);
    }

    for (uint64_t entry : node.clusters) {
        if (written64 >= byteLimit) {
            break;
        }

        uint64_t readBytes;
        uint64_t readOffset;

        if (hints.isNtfs && hints.bytesPerCluster > 0) {
            if (entry == 0) {
                const uint64_t toWrite = std::min(static_cast<uint64_t>(hints.bytesPerCluster),
                                                    byteLimit - written64);
                const size_t oldSz = out.size();
                out.resize(oldSz + static_cast<size_t>(toWrite));
                memset(out.data() + oldSz, 0, static_cast<size_t>(toWrite));
                written64 += toWrite;
                continue;
            }
            readOffset = partition.startSector * ss + entry * hints.bytesPerCluster;
            readBytes = hints.bytesPerCluster;
        } else if (hints.isExt4 && hints.ext4BlockSize > 0) {
            if (entry == 0) {
                const uint64_t toWrite = std::min(static_cast<uint64_t>(hints.ext4BlockSize),
                                                  byteLimit - written64);
                const size_t oldSz = out.size();
                out.resize(oldSz + static_cast<size_t>(toWrite));
                memset(out.data() + oldSz, 0, static_cast<size_t>(toWrite));
                written64 += toWrite;
                continue;
            }
            readOffset = partition.startSector * ss + entry * hints.ext4BlockSize;
            readBytes = hints.ext4BlockSize;
        } else {
            const uint64_t clusterBytes = hints.bytesPerCluster > 0
                                              ? hints.bytesPerCluster
                                              : static_cast<uint64_t>(ss);
            readOffset = FatExfatClusterByteOffset(entry, ss);
            readBytes  = clusterBytes;
        }

        const uint64_t toAppend = std::min(readBytes, byteLimit - written64);

        if (!reader.ReadBytes(readOffset, buf.data(),
                              static_cast<uint32_t>(readBytes))) {
            memset(buf.data(), 0, static_cast<size_t>(readBytes));
        }

        const size_t oldSz = out.size();
        out.resize(oldSz + static_cast<size_t>(toAppend));
        memcpy(out.data() + oldSz, buf.data(), static_cast<size_t>(toAppend));
        written64 += toAppend;
    }

    return !out.empty();
}

// ─────────────────────────────────────────────────────────────────────────────
// WriteRecoveredFile – the core data-copy routine
// ─────────────────────────────────────────────────────────────────────────────
bool RecoverEngine::WriteRecoveredFile(const std::wstring& targetPath,
                                        SectorReader&       reader,
                                        const FileNode&     node,
                                        const Partition&    partition,
                                        std::wstring&       outError)
{
    // Ensure target directory exists (no std::filesystem dependency)
    // Extract parent directory from targetPath and create recursively.
    std::wstring parentDir;
    {
        const size_t pos = targetPath.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            parentDir = targetPath.substr(0, pos);
        }
    }
    if (!parentDir.empty()) {
        if (!Utils::CreateDirectoryRecursive(parentDir)) {
            outError = L"Cannot create target directory.";
            return false;
        }
    }

    RecoverOutputFile out;
    if (!out.Open(targetPath, outError)) {
        return false;
    }

    auto finishWrite = [&]() {
        out.SetFileTimes(node.createTime, node.modifyTime, node.accessTime);
        out.Close();
    };

    // ── Case 0: Resident NTFS file – data embedded in MFT record ─────────
    // No disk I/O needed: we already have the bytes in memory.
    if (!node.residentData.empty()) {
        const bool ok =
            out.Write(node.residentData.data(), static_cast<uint32_t>(node.residentData.size()));
        if (!ok) {
            outError = L"Write error for resident data";
        }
        finishWrite();
        return ok;
    }

    // ── Case 1: RAW scan – single range or graph-stitched multi-span ───────
    if (node.clusters.empty() &&
        (!node.rawFragments.empty() || node.rawSize > 0)) {
        constexpr uint32_t kBuf = 4 * 1024 * 1024; // 4 MB
        std::vector<uint8_t> buf(kBuf);
        bool ok = true;

        auto writeSpan = [&](uint64_t offset, uint64_t spanSize) -> bool {
            uint64_t remaining = spanSize;
            while (remaining > 0) {
                const uint32_t toRead = static_cast<uint32_t>(std::min(
                    remaining, static_cast<uint64_t>(kBuf)));
                if (!reader.ReadBytes(offset, buf.data(), toRead)) {
                    outError = L"Read error at offset " + std::to_wstring(offset);
                    return false;
                }
                out.Write(buf.data(), toRead);
                offset += toRead;
                remaining -= toRead;
            }
            return true;
        };

        if (!node.rawFragments.empty()) {
            for (const auto& span : node.rawFragments) {
                if (!writeSpan(span.diskOffset, span.size)) {
                    ok = false;
                    break;
                }
            }
        } else {
            ok = writeSpan(node.rawOffset, node.rawSize);
        }

        finishWrite();
        return ok;
    }

    // ── Case 2: Cluster-list file (NTFS DataRuns / FAT chain / ext4 blocks) ──
    if (!node.clusters.empty()) {
        const uint32_t ss = reader.GetSectorSize();

        // Read boot/superblock area to detect filesystem type
        uint8_t bootBuf[2048]{};
        reader.ReadBytes(partition.startSector * ss, bootBuf, 2048);

        const FsVolumeHints hints = ParseFsVolumeHints(bootBuf, ss);

        uint64_t written64 = 0;
        bool     ok        = true;

        // ── NTFS Compressed file path ─────────────────────────────────────
        // The cluster list encodes compression units:
        //   • Real LCNs (> 0) store compressed or uncompressed data
        //   • LCN = 0 marks the "sparse hole" at the end of a compressed unit
        // We process in compressionUnit-sized slices and call LZNT1 per slice.
        if (node.isNtfsCompressed && hints.isNtfs && hints.bytesPerCluster > 0
            && node.compressionUnit > 0)
        {
            const uint32_t cuClusters = node.compressionUnit; // e.g. 16
            const uint32_t unitBytes  = cuClusters * hints.bytesPerCluster; // e.g. 64 KB

            std::vector<uint8_t> rawUnitBuf(unitBytes);    // compressed data
            std::vector<uint8_t> decompBuf(unitBytes);     // decompressed output

            const auto& clus = node.clusters;
            size_t ci = 0;

            while (ci < clus.size() && written64 < node.size) {
                // Collect one compression unit (cuClusters entries)
                size_t unitEnd  = std::min(ci + cuClusters, clus.size());
                uint32_t realCount   = 0; // real (non-sparse) clusters
                uint32_t sparseCount = 0; // sparse (LCN=0) clusters

                for (size_t k = ci; k < unitEnd; ++k) {
                    if (clus[k] == 0) ++sparseCount;
                    else              ++realCount;
                }

                if (sparseCount == 0) {
                    // ── Uncompressed unit: read all clusters directly ──────
                    uint64_t toWriteUnit = std::min(
                        static_cast<uint64_t>(unitBytes), node.size - written64);
                    uint64_t unitWritten = 0;

                    for (size_t k = ci; k < unitEnd && unitWritten < toWriteUnit; ++k) {
                        uint64_t readOffset = partition.startSector * ss
                                            + clus[k] * hints.bytesPerCluster;
                        uint32_t toWrite    = static_cast<uint32_t>(
                            std::min(static_cast<uint64_t>(hints.bytesPerCluster),
                                     toWriteUnit - unitWritten));
                        rawUnitBuf.resize(hints.bytesPerCluster);
                        if (!reader.ReadBytes(readOffset, rawUnitBuf.data(),
                                              hints.bytesPerCluster))
                            memset(rawUnitBuf.data(), 0, hints.bytesPerCluster);
                        out.Write(rawUnitBuf.data(), toWrite);
                        unitWritten += toWrite;
                    }
                    written64 += unitWritten;
                } else {
                    // ── Compressed unit: read real clusters, then LZNT1 ───
                    uint32_t compressedBytes = realCount * hints.bytesPerCluster;
                    rawUnitBuf.resize(compressedBytes);
                    uint32_t rawPos = 0;

                    for (size_t k = ci; k < unitEnd; ++k) {
                        if (clus[k] == 0) continue; // sparse = skip
                        uint64_t readOffset = partition.startSector * ss
                                            + clus[k] * hints.bytesPerCluster;
                        if (!reader.ReadBytes(readOffset,
                                              rawUnitBuf.data() + rawPos,
                                              hints.bytesPerCluster))
                            memset(rawUnitBuf.data() + rawPos, 0, hints.bytesPerCluster);
                        rawPos += hints.bytesPerCluster;
                    }

                    decompBuf.resize(unitBytes);
                    uint32_t outLen = 0;
                    DecompressLznt1Unit(rawUnitBuf.data(), compressedBytes,
                                        unitBytes, decompBuf.data(), outLen);

                    uint64_t toWriteUnit = std::min(
                        static_cast<uint64_t>(outLen), node.size - written64);
                    out.Write(decompBuf.data(), static_cast<uint32_t>(toWriteUnit));
                    written64 += toWriteUnit;
                }

                ci = unitEnd;
            }

            finishWrite();
            return ok;
        }

        // ── Regular (non-compressed) cluster-list path ────────────────────
        constexpr uint32_t kBuf = 4 * 1024 * 1024;
        std::vector<uint8_t> buf(kBuf);

        for (uint64_t entry : node.clusters) {
            if (written64 >= node.size) break;

            uint64_t readBytes;
            uint64_t readOffset;

            if (hints.isNtfs && hints.bytesPerCluster > 0) {
                // NTFS: entry is an LCN; LCN=0 means sparse cluster → zero-fill
                if (entry == 0) {
                    uint64_t toWrite = std::min(
                        static_cast<uint64_t>(hints.bytesPerCluster), node.size - written64);
                    memset(buf.data(), 0, static_cast<size_t>(toWrite));
                    out.Write(buf.data(), static_cast<uint32_t>(toWrite));
                    written64 += toWrite;
                    continue;
                }
                readOffset = partition.startSector * ss + entry * hints.bytesPerCluster;
                readBytes  = hints.bytesPerCluster;
            } else if (hints.isExt4 && hints.ext4BlockSize > 0) {
                // ext4: entry is a block number relative to partition start
                if (entry == 0) {
                    uint64_t toWrite = std::min(
                        static_cast<uint64_t>(hints.ext4BlockSize), node.size - written64);
                    memset(buf.data(), 0, static_cast<size_t>(toWrite));
                    out.Write(buf.data(), static_cast<uint32_t>(toWrite));
                    written64 += toWrite;
                    continue;
                }
                readOffset = partition.startSector * ss + entry * hints.ext4BlockSize;
                readBytes  = hints.ext4BlockSize;
            } else {
                // FAT/exFAT: clusters[] are absolute disk sector numbers per cluster.
                const uint64_t clusterBytes = hints.bytesPerCluster > 0
                                                  ? hints.bytesPerCluster
                                                  : static_cast<uint64_t>(ss);
                readOffset = FatExfatClusterByteOffset(entry, ss);
                readBytes  = clusterBytes;
            }

            uint64_t toWrite = std::min(readBytes, node.size - written64);

            if (!reader.ReadBytes(readOffset, buf.data(),
                                  static_cast<uint32_t>(readBytes))) {
                memset(buf.data(), 0, static_cast<size_t>(toWrite));
            }

            out.Write(buf.data(), static_cast<uint32_t>(toWrite));
            written64 += toWrite;
        }

        // FAT/exFAT: directory size may exceed recoverable chain by <= one cluster.
        if (written64 < node.size && !hints.isNtfs && !hints.isExt4) {
            const uint64_t clusterBytes = hints.bytesPerCluster > 0
                                              ? hints.bytesPerCluster
                                              : static_cast<uint64_t>(ss);
            const uint64_t slack = node.size - written64;
            if (clusterBytes > 0 && slack <= clusterBytes) {
                std::vector<uint8_t> pad(static_cast<size_t>(slack), 0);
                if (!out.Write(pad.data(), static_cast<uint32_t>(slack))) {
                    ok = false;
                    outError = L"Write error while padding allocation slack";
                } else {
                    written64 += slack;
                }
            }
        }

        // ext4: sparse tail — i_size may extend beyond last allocated block (faillog, .ko, etc.)
        if (written64 < node.size && hints.isExt4) {
            const uint64_t slack = node.size - written64;
            constexpr uint32_t kPadChunk = 4 * 1024 * 1024;
            uint64_t left = slack;
            std::vector<uint8_t> zeros(static_cast<size_t>((std::min)(left, static_cast<uint64_t>(kPadChunk))), 0);
            while (left > 0 && ok) {
                const uint32_t chunk = static_cast<uint32_t>((std::min)(left, static_cast<uint64_t>(kPadChunk)));
                if (!out.Write(zeros.data(), chunk)) {
                    ok = false;
                    outError = L"Write error while padding ext4 sparse tail";
                    break;
                }
                written64 += chunk;
                left -= chunk;
            }
        }

        finishWrite();
        return ok;
    }

    // ── Case 3: Fallback – no data available ──────────────────────────────
    // This should not be reached for resident files (handled in Case 0).
    // Possible for empty files (size == 0) or unusual FS layouts.
    out.Close();
    if (node.size == 0) {
        // Legitimately empty file – create the 0-byte file and succeed.
        return true;
    }
    outError = L"No recoverable data (no clusters and no resident data)";
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// RecoverFile
// ─────────────────────────────────────────────────────────────────────────────
FileRecoverResult RecoverEngine::RecoverFile(
    const FileNode&     node,
    const std::wstring& targetDir,
    bool                keepDirStructure,
    SectorReader&       reader,
    const Partition&    partition)
{
    FileRecoverResult res;
    res.node = &node;

    if (node.isDirectory) {
        res.status = RecoverStatus::Skipped;
        res.reason = L"Directory";
        return res;
    }

    res.targetPath = BuildTargetPathForNode(node, targetDir, keepDirStructure);

    // If already exists, auto-rename to avoid silent data loss.
    if (Utils::FileExists(res.targetPath)) {
        const std::wstring unique = Utils::MakeUniqueFilePath(res.targetPath);
        if (unique != res.targetPath && !Utils::FileExists(unique)) {
            res.targetPath = unique;
        } else {
            res.status = RecoverStatus::Skipped;
            res.reason = L"Target already exists";
            return res;
        }
    }

    FileNode writeNode = node;
    FragmentStitchService::TryImproveForRecovery(
        reader, partition, writeNode, m_options.fragmentStitch);

    std::wstring err;
    bool ok = WriteRecoveredFile(res.targetPath, reader, writeNode, partition, err);

    if (ok) {
        const uint64_t got = RecoveredFileSize(res.targetPath);
        if (got == node.size) {
            res.status = RecoverStatus::Success;
            LOG_INFO(L"Recovered: " + node.name + L" -> " + res.targetPath);
        } else {
            const FsVolumeHints hints = ReadPartitionHints(reader, partition);
            if (AcceptPartialFatDeleted(node, partition, got, hints.bytesPerCluster)) {
                res.status = RecoverStatus::Partial;
                res.reason = L"partial recover (deleted, cluster-limited)";
                LOG_INFO(L"Partial recover: " + node.name + L" -> " + res.targetPath);
            } else {
                res.status = RecoverStatus::Failed;
                std::wostringstream oss;
                oss << L"size mismatch expected=" << node.size << L" got=" << got;
                res.reason = oss.str();
                LOG_WARN(L"Failed to recover " + node.name + L": " + res.reason);
                RemoveRecoveredFile(res.targetPath);
            }
        }
    } else {
        res.status = RecoverStatus::Failed;
        res.reason = err;
        LOG_WARN(L"Failed to recover " + node.name + L": " + err);
        RemoveRecoveredFile(res.targetPath);
    }

    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// RecoverBatch
// ─────────────────────────────────────────────────────────────────────────────
RecoverBatchResult RecoverEngine::RecoverBatch(
    const std::vector<FileNode>& nodes,
    const std::wstring&          targetDir,
    bool                         keepDirStructure,
    SectorReader&                reader,
    const Partition&             partition,
    std::atomic<bool>*           cancelFlag)
{
    RecoverBatchResult batch;
    uint32_t total = static_cast<uint32_t>(nodes.size());
    uint32_t done  = 0;

    for (const auto& node : nodes) {
        if (m_onPause && !m_onPause()) {
            batch.cancelled = true;
            break;
        }
        if (cancelFlag && cancelFlag->load()) {
            batch.cancelled = true;
            break;
        }

        FileRecoverResult r =
            RecoverFile(node, targetDir, keepDirStructure, reader, partition);

        switch (r.status) {
        case RecoverStatus::Success:
        case RecoverStatus::Partial:
            ++batch.succeeded;
            break;
        case RecoverStatus::Failed:  ++batch.failed;    break;
        case RecoverStatus::Skipped: ++batch.skipped;   break;
        }

        batch.details.push_back(r);
        ++done;

        if (m_onProgress) m_onProgress(done, total);
        if (m_onFile)     m_onFile(r);
    }

    return batch;
}
