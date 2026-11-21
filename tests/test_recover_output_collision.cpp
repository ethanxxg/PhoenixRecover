// P0-4 regression gate: output-path collision discovered on the real damaged
// disk (2026-11-14, batch_003, 1287 failed files).
//
// On that disk the MFT contains BOTH a FILE and a DIRECTORY with the same path
// segment, e.g.
//     ...\commonlib\QmSafeMNg.zip                    (a 2.7 MB file)
//     ...\commonlib\QmSafeMNg.zip\hooks\commit-msg.sample  (its "children")
// With keepDirStructure=true, RecoverEngine::BuildTargetPathForNode puts the
// children under a directory named QmSafeMNg.zip. Since a regular file with
// that name was already written, Utils::CreateDirectoryRecursive fails and
// WriteRecoveredFile returns "Cannot create target directory." for every child.
// 123 distinct parent directories / 1287 files failed on the real disk.
//
// This test uses NTFS *resident* files (data embedded in the MFT record), so
// WriteRecoveredFile's Case 0 needs NO disk I/O and the defect is reproducible
// without any image or physical disk.
//
// Gate 1 (defect lock): with the original paths, exactly one node succeeds and
//         the child fails. This pins the known engine behaviour; if someone
//         fixes BuildTargetPathForNode, this test fails and forces the docs to
//         be updated instead of silently drifting.
// Gate 2 (workaround): isolating each node into its own "<seq>_<id>\" segment
//         (exactly what tools/phoenix_headless.cpp now does by default) makes
//         BOTH nodes succeed with byte-identical payloads.
// Negative guard: PHOENIX_NEGATIVE_COLLISION_GUARD=1 re-introduces the
//         collision into Gate 2, so the test MUST fail. Run once to prove the
//         gate can actually detect a broken workaround, then leave it unset.
//
// Zero production changes: RecoverEngine / RecoverTargetPath / Utils are linked
// exactly as in the GUI; only output path metadata is varied.

#include "disk/SectorReader.h"
#include "disk/Partition.h"
#include "recover/FileNode.h"
#include "recover/RecoverEngine.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static int g_failures = 0;

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__,    \
                         __LINE__);                                         \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

static std::vector<uint8_t> Payload(char fill, size_t n)
{
    return std::vector<uint8_t>(n, static_cast<uint8_t>(fill));
}

static FileNode MakeNode(uint64_t id, const std::wstring& name,
                         const std::wstring& originalPath,
                         const std::vector<uint8_t>& data)
{
    FileNode n;
    n.id             = id;
    n.name           = name;
    n.originalPath   = originalPath;
    n.size           = data.size();
    n.residentData   = data;   // Case 0: written straight from memory
    n.isDirectory    = false;
    n.fsType         = L"NTFS";
    return n;
}

static std::vector<uint8_t> ReadAll(const std::wstring& p)
{
    std::ifstream f(fs::path(p), std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

struct RunResult {
    int                          succeeded = 0;
    int                          failed    = 0;
    std::vector<std::wstring>    paths;
    std::vector<int>             statuses;
};

static RunResult RunBatch(SectorReader& reader, const Partition& part,
                          const std::wstring& dir,
                          const std::vector<FileNode>& nodes)
{
    std::error_code ec;
    fs::create_directories(fs::path(dir), ec);
    RecoverEngine engine;
    RecoverBatchResult res = engine.RecoverBatch(nodes, dir, true, reader, part);
    RunResult out;
    out.succeeded = res.succeeded;
    out.failed    = res.failed;
    for (const auto& d : res.details) {
        out.paths.push_back(d.targetPath);
        out.statuses.push_back(static_cast<int>(d.status));
    }
    return out;
}

int main()
{
    // Deliberately NOT opened: resident data never touches the reader.
    SectorReader reader;
    Partition part{};
    part.fsType = L"NTFS";

    const auto dataA = Payload('A', 64);
    const auto dataB = Payload('B', 96);

    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch().count();
    const fs::path base = fs::temp_directory_path() /
        ("phoenix_output_collision_" + std::to_string(stamp));

    const std::wstring collisionDir = (base / L"collision").wstring();
    const std::wstring isolatedDir  = (base / L"isolated").wstring();
    const std::wstring negativeDir  = (base / L"negative").wstring();

    // ---- Gate 1: reproduce the real-disk defect ----------------------------
    FileNode fileA = MakeNode(101, L"QmSafeMNg.zip", L"QmSafeMNg.zip", dataA);
    FileNode childB = MakeNode(102, L"commit-msg.sample",
                               L"QmSafeMNg.zip\\hooks\\commit-msg.sample", dataB);
    RunResult r1 = RunBatch(reader, part, collisionDir, {fileA, childB});
    std::printf("collision run: succeeded=%d failed=%d details=%zu\n",
                r1.succeeded, r1.failed, r1.paths.size());
    CHECK(r1.succeeded == 1);
    CHECK(r1.failed == 1);
    CHECK(r1.paths.size() == 2);
    if (r1.paths.size() == 2) {
        CHECK(ReadAll(r1.paths[0]) == dataA);   // the FILE still recovers
        CHECK(ReadAll(r1.paths[1]).empty());    // child was never written
    }

    // ---- Gate 2: P0-4 isolation workaround --------------------------------
    FileNode isolatedB = childB;
    // Exactly the rewrite in tools/phoenix_headless.cpp SafeLeaf()/CmdRecover():
    // a unique first segment, with the data location left untouched.
    isolatedB.originalPath = L"f1_102\\commit-msg.sample";
    RunResult r2 = RunBatch(reader, part, isolatedDir, {fileA, isolatedB});
    std::printf("isolated run: succeeded=%d failed=%d details=%zu\n",
                r2.succeeded, r2.failed, r2.paths.size());
    CHECK(r2.succeeded == 2);
    CHECK(r2.failed == 0);
    CHECK(r2.paths.size() == 2);
    if (r2.paths.size() == 2) {
        CHECK(ReadAll(r2.paths[0]) == dataA);
        CHECK(ReadAll(r2.paths[1]) == dataB);   // byte-identical payload
    }

    // ---- Negative guard (run once with the env var, expect FAIL) ----------
    if (std::getenv("PHOENIX_NEGATIVE_COLLISION_GUARD") != nullptr) {
        FileNode brokenB = childB;   // isolation removed -> collision returns
        RunResult rn = RunBatch(reader, part, negativeDir, {fileA, brokenB});
        std::printf("NEGATIVE run: succeeded=%d failed=%d\n",
                    rn.succeeded, rn.failed);
        CHECK(rn.succeeded == 2);    // MUST fail when the guard is disabled
    }

    std::error_code ec;
    fs::remove_all(base, ec);

    if (g_failures) {
        std::fprintf(stderr, "FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("OK: RecoverOutputCollision (defect locked + isolation "
                "workaround byte-verified)\n");
    return 0;
}
