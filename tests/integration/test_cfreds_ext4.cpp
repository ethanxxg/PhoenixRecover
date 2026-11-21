// CFReDS DFR-01-EXT — ext4 deleted-file discovery via FileSystemModuleService.

#include "common/Platform.h"

#if PHOENIX_ENABLE_VOLUME_IO

#include "common/Utils.h"
#include "disk/Partition.h"
#include "disk/PartitionRecoveryService.h"
#include "disk/SectorReader.h"
#include "fs/FileSystem.h"
#include "recover/FileNode.h"
#include "recover/RecoverEngine.h"
#include "scan/FileSystemModuleService.h"
#include "scan/ScanEngine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Read the ext block size (bytes) from the superblock at partition start+1024.
// ext2/3/4 superblock magic is 0xEF53 at +0x38, s_log_block_size at +0x18.
static uint32_t ExtBlockSize(SectorReader& reader, const Partition& part)
{
    const uint32_t ss = reader.GetSectorSize() ? reader.GetSectorSize() : 512;
    // superblock lives at partition start + 1024; read 3 sectors to cover it.
    std::vector<uint8_t> sb(ss * 3);
    if (!reader.ReadBytes(part.startSector * (uint64_t)ss, sb.data(),
                          (uint32_t)sb.size())) {
        return 0;
    }
    const uint8_t* p = sb.data() + 1024;
    if (p[0x38] != 0x53 || p[0x39] != 0xEF) return 0;   // no ext magic
    uint32_t log = 0;
    std::memcpy(&log, p + 0x18, sizeof(log));
    if (log > 6) return 0;                               // 64 KiB max
    return 1024u << log;
}

// Rebuild a file's bytes from the engine's own runlist directly off the image
// and compare against what RecoverEngine wrote. This is the same method used
// by `phoenix_headless verify`: two independent code paths (RecoverEngine's
// write path vs. this direct cluster read), no external oracle decides.
// Sparse clusters (0) are zero-filled, matching RecoverEngine semantics.
static bool ByteVerifyFile(SectorReader& reader, const Partition& part,
                           uint32_t blockSize, const FileNode& node,
                           const std::wstring& outPath, uint64_t& firstDiff)
{
    // MinGW libstdc++ has no wide-path ifstream ctor; use the CRT wide API.
    FILE* f = _wfopen(outPath.c_str(), L"rb");
    if (!f) { firstDiff = 0; return false; }

    const uint64_t total = node.size;
    uint64_t done = 0;
    firstDiff = 0;

    auto cmp = [&](const uint8_t* expected, size_t n) -> bool {
        std::vector<uint8_t> got(n);
        if (std::fread(got.data(), 1, n, f) != n) return false;
        for (size_t i = 0; i < n; ++i) {
            if (expected[i] != got[i]) { firstDiff = done + i; return false; }
        }
        done += n;
        return true;
    };

    if (!node.residentData.empty()) {
        const size_t take = (size_t)std::min<uint64_t>(total, node.residentData.size());
        if (take && !cmp(node.residentData.data(), take)) return false;
        done = take;
    } else {
        const uint32_t ss = reader.GetSectorSize() ? reader.GetSectorSize() : 512;
        std::vector<uint8_t> buf(blockSize);
        for (uint64_t blk : node.clusters) {
            if (done >= total) break;
            const size_t take = (size_t)std::min<uint64_t>(blockSize, total - done);
            if (blk == 0) {
                std::vector<uint8_t> zeros(take, 0);
                if (!cmp(zeros.data(), take)) return false;
            } else {
                const uint64_t off = part.startSector * (uint64_t)ss
                                   + blk * (uint64_t)blockSize;
                if (!reader.ReadBytes(off, buf.data(), (uint32_t)blockSize)) return false;
                if (!cmp(buf.data(), take)) return false;
            }
        }
    }
    std::fclose(f);
    if (done != total) { firstDiff = done; return false; }
    return true;
}

static int g_failures = 0;

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "FAIL: %s  (%s:%d)\n", #expr, __FILE__, __LINE__); \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

#define CHECK_MSG(expr, msg)                                                \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "FAIL: %s — %s  (%s:%d)\n",                    \
                    #expr, (msg), __FILE__, __LINE__);                      \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

static std::wstring ResolveImage()
{
#ifdef PHOENIX_SOURCE_DIR
    namespace fs = std::filesystem;
    const fs::path base(PHOENIX_SOURCE_DIR);
    const fs::path candidates[] = {
        base / "testdata" / "dfr-test-images" / "dfr-01" / "dfr-01-ext.dd",
        base / "testdata" / "cfreds" / "dfr-01-ext.dd",
        base / "testdata" / "DFR" / "dfr-01-ext.dd",
    };
    for (const auto& p : candidates) {
        if (fs::exists(p)) {
            return p.wstring();
        }
    }
#endif
    return L"";
}

int main()
{
    const std::wstring imagePath = ResolveImage();
    if (imagePath.empty()) {
        fprintf(stdout, "SKIP: dfr-01-ext.dd not found (download CFReDS DFR-01-EXT)\n");
        return 0;
    }

    fprintf(stdout, "Image: %ls\n", imagePath.c_str());

    SectorReader reader;
    CHECK_MSG(reader.OpenImage(imagePath), "OpenImage");
    if (!reader.IsOpen()) {
        return 1;
    }

    PartitionRecoveryService::Options opt;
    auto candidates = PartitionRecoveryService::ProbeVolume(reader, opt, nullptr, nullptr);
    CHECK_MSG(!candidates.empty(), "no partition candidates");

    std::error_code ec;
    const fs::path tmpRoot = fs::temp_directory_path(ec) / "phoenix_cfreds_ext4_verify";
    if (!ec) {
        fs::remove_all(tmpRoot, ec);
        fs::create_directories(tmpRoot, ec);
    }
    const std::wstring outDir = tmpRoot.wstring();

    RecoverEngine engine;
    int totalNodes = 0, totalDeleted = 0;
    int attempted = 0, recovered = 0, byteMatched = 0, byteFailed = 0;
    uint64_t verifiedBytes = 0;
    int extPartitions = 0;

    // Iterate every ext partition in the real image: p1/p2 use indirect block
    // maps (ext2), p3 uses extents + flex_bg (ext4). Both deep paths get a
    // byte-level gate instead of the old size-only check.
    for (const auto& cand : candidates) {
        if (cand.totalSectors == 0) continue;
        Partition part = PartitionRecoveryService::ToPartition(cand, -1);
        if (part.fsType.empty()) part.fsType = L"ext4";

        const uint32_t bs = ExtBlockSize(reader, part);
        if (bs == 0) continue;   // not an ext volume -> skip
        ++extPartitions;

        ScanResult scanMeta;
        auto fs = FileSystemModuleService::CreateParserForPartition(reader, part, scanMeta);
        if (!fs || !fs->Open(reader, part) || !fs->ScanMetadata()) {
            CHECK_MSG(fs != nullptr, "CreateParserForPartition");
            continue;
        }
        const auto& files = fs->GetAllFiles();
        int deleted = 0;
        for (const auto& f : files)
            if (f.status == FileStatus::Deleted && !f.isDirectory) ++deleted;
        totalNodes += (int)files.size();
        totalDeleted += deleted;
        fprintf(stdout, "ext part@%llu: nodes=%zu deleted=%d blockSize=%u\n",
                (unsigned long long)part.startSector, files.size(), deleted, bs);

        for (const FileNode& node : files) {
            if (node.status != FileStatus::Deleted || node.isDirectory) continue;
            if (node.size == 0) continue;
            if (node.clusters.empty() && node.residentData.empty()) continue;
            ++attempted;
            const FileRecoverResult rr =
                engine.RecoverFile(node, outDir, false, reader, part);
            if (rr.status != RecoverStatus::Success || rr.targetPath.empty()) continue;
            FILE* probe = _wfopen(rr.targetPath.c_str(), L"rb");
            if (!probe) continue;
            std::fseek(probe, 0, SEEK_END);
            const long sz = std::ftell(probe);
            std::fclose(probe);
            if (sz < 0 || (uint64_t)sz != node.size) continue;
            ++recovered;
            uint64_t diff = 0;
            if (ByteVerifyFile(reader, part, bs, node, rr.targetPath, diff)) {
                ++byteMatched;
                verifiedBytes += node.size;
            } else {
                ++byteFailed;
                fprintf(stderr,
                        "FAIL: byte mismatch part=%llu node=%u size=%llu firstDiff=%llu\n",
                        (unsigned long long)part.startSector, node.id,
                        (unsigned long long)node.size,
                        (unsigned long long)diff);
            }
        }
    }

    fprintf(stdout,
            "byte-gate: partitions=%d nodes=%d deleted=%d attempted=%d "
            "recovered=%d byteMatched=%d byteFailed=%d bytes=%llu\n",
            extPartitions, totalNodes, totalDeleted, attempted,
            recovered, byteMatched, byteFailed, (unsigned long long)verifiedBytes);
    CHECK_MSG(extPartitions >= 2, "expected at least 2 real ext partitions in DFR-01-EXT");
    CHECK_MSG(totalNodes >= 1, "expected at least one file node from ext scan");
    CHECK_MSG(totalDeleted >= 1, "expected at least one Deleted file node (htree + dentry scan)");
    CHECK_MSG(recovered >= 1, "no deleted file was byte-recoverable from the real ext volume");
    CHECK_MSG(byteFailed == 0, "at least one recovered file failed byte verification");
    CHECK_MSG(byteMatched == recovered, "byte-verified count != recovered count");

    if (!ec) fs::remove_all(tmpRoot, ec);
    reader.Close();

    if (g_failures == 0) {
        fprintf(stdout,
                "OK: CFReDS EXT4 DFR-01 smoke (%d nodes, %d deleted) + "
                "byte-gate (%d partitions, %d/%d files, %llu bytes)\n",
                totalNodes, totalDeleted, extPartitions, byteMatched, recovered,
                (unsigned long long)verifiedBytes);
    }
    return g_failures ? 1 : 0;
}

#else

#include <cstdio>
int main()
{
    fprintf(stdout, "SKIP: CFReDS ext4 test requires volume I/O platform\n");
    return 0;
}

#endif
