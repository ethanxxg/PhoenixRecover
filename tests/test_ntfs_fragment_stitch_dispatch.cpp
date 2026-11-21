// NTFS dispatch smoke test: with a default-constructed (unopened) SectorReader
// the stitcher cannot load volume geometry and must safely return false.
// The real NTFS dispatch is wired to NtfsFragmentStitcher::TryImproveNode
// (see FragmentStitchService.cpp and FRAGMENT_STITCH_MODULE.md §NTFS) —
// the older "stub returns false" comment was inaccurate.
#include "recover/FragmentStitchService.h"
#include "recover/FragmentStitchPolicy.h"
#include "recover/FileNode.h"
#include "disk/Partition.h"
#include "disk/SectorReader.h"

#include <cstdio>

static int g_failures = 0;

#define CHECK(expr) \
    do { \
        if (!(expr)) { \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
            ++g_failures; \
        } \
    } while (0)

int main()
{
    FileNode node;
    node.fsType     = L"NTFS";
    node.extension  = L"jpg";
    node.size       = 1024;
    node.clusters   = { 100, 101, 102 };

    SectorReader reader;
    Partition part;
    part.fsType = L"NTFS";

    FragmentStitchPolicy policy;
    policy.enabled = true;

    const bool improved =
        FragmentStitchService::TryImproveForRecovery(reader, part, node, policy);
    CHECK(!improved);

    policy.enabled = false;
    CHECK(!FragmentStitchService::TryImproveForRecovery(reader, part, node, policy));

    if (g_failures != 0) {
        fprintf(stderr, "NtfsFragmentStitch_Dispatch: %d failure(s)\n", g_failures);
        return 1;
    }
    fprintf(stdout, "OK: NtfsFragmentStitch_Dispatch\n");
    return 0;
}
