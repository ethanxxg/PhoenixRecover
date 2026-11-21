// A0 write-path safety base: unit tests for disk/WriteGuard.h
//
// Built twice by CTest:
//   WriteGuardDefaultDeny  -> PHOENIX_ENABLE_DEVICE_WRITES=0 (production default)
//   WriteGuard             -> PHOENIX_ENABLE_DEVICE_WRITES=1 (isolated logic build)
//
// No device is ever opened. No byte is ever written.

#include "disk/WriteGuard.h"

#include <iostream>

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::cerr << "FAIL line " << __LINE__ << ": " #cond << "\n";   \
            return 1;                                                      \
        }                                                                  \
    } while (0)

using namespace phoenix;

static WriteTarget GoodTarget()
{
    WriteTarget t;
    t.id                 = "PD3";
    t.kind               = WriteTargetKind::TargetDisk;
    t.physicalDiskIndex  = 3;
    t.volume             = L"F:";
    t.hasBaseline        = true;
    t.hasRollbackImage   = true;
    return t;
}

static WriteGuard FullyConfigured()
{
    WriteGuard g;
    g.RegisterSource("PD2", 2, L"E:");
    g.RegisterTarget(GoodTarget(),
                     {WriteOperation::WriteTargetDisk,
                      WriteOperation::WritePartitionTable,
                      WriteOperation::WriteBootRecord,
                      WriteOperation::IrreversibleWipe,
                      WriteOperation::SectorEdit});
    return g;
}

#if PHOENIX_ENABLE_DEVICE_WRITES

static int RunLogicTests()
{
    // S2 -- default deny on an empty guard.
    {
        WriteGuard g;
        const auto d = g.Authorize("PD3", WriteOperation::WriteTargetDisk);
        CHECK(!d.allowed);
        CHECK(d.reason == WriteDenyReason::TargetNotRegistered);
        CHECK(std::string(d.Detail()).find("default deny") != std::string::npos);
    }

    // Happy path: A2 clone/restore to a whitelisted destination disk.
    {
        WriteGuard g = FullyConfigured();
        const auto d = g.Authorize("PD3", WriteOperation::WriteTargetDisk);
        CHECK(d.allowed);
        CHECK(d.reason == WriteDenyReason::None);
    }

    // S2 -- operation outside the per-target whitelist.
    {
        WriteGuard g;
        WriteTarget t = GoodTarget();
        g.RegisterTarget(t, {WriteOperation::WriteTargetDisk});
        const auto d = g.Authorize("PD3", WriteOperation::SectorEdit);
        CHECK(!d.allowed);
        CHECK(d.reason == WriteDenyReason::OperationNotAllowed);
    }

    // S4 -- no verifiable baseline.
    {
        WriteGuard g;
        WriteTarget t = GoodTarget();
        t.hasBaseline = false;
        g.RegisterTarget(t, {WriteOperation::WriteTargetDisk});
        const auto d = g.Authorize("PD3", WriteOperation::WriteTargetDisk);
        CHECK(!d.allowed);
        CHECK(d.reason == WriteDenyReason::MissingBaseline);
    }

    // S1 -- source disk by physical index, by volume (case-insensitive),
    //       by source id, and by explicit SourceDisk kind.
    {
        WriteGuard g = FullyConfigured();

        WriteTarget byDisk = GoodTarget();
        byDisk.id = "ALIAS-DISK2";
        byDisk.physicalDiskIndex = 2;
        byDisk.volume = L"X:";
        g.RegisterTarget(byDisk, {WriteOperation::WriteTargetDisk});
        CHECK(g.Authorize("ALIAS-DISK2", WriteOperation::WriteTargetDisk).reason ==
              WriteDenyReason::SourceDiskWriteAttempt);

        WriteTarget byVolume = GoodTarget();
        byVolume.id = "ALIAS-VOLE";
        byVolume.physicalDiskIndex = 9;
        byVolume.volume = L"e:";
        g.RegisterTarget(byVolume, {WriteOperation::WriteTargetDisk});
        CHECK(g.Authorize("ALIAS-VOLE", WriteOperation::WriteTargetDisk).reason ==
              WriteDenyReason::SourceDiskWriteAttempt);

        WriteTarget byId = GoodTarget();
        byId.id = "PD2";
        byId.physicalDiskIndex = 8;
        byId.volume = L"Z:";
        g.RegisterTarget(byId, {WriteOperation::WriteTargetDisk});
        CHECK(g.Authorize("PD2", WriteOperation::WriteTargetDisk).reason ==
              WriteDenyReason::SourceDiskWriteAttempt);

        WriteTarget byKind = GoodTarget();
        byKind.id = "EXPLICIT-SOURCE";
        byKind.kind = WriteTargetKind::SourceDisk;
        g.RegisterTarget(byKind, {WriteOperation::WriteTargetDisk});
        CHECK(g.Authorize("EXPLICIT-SOURCE", WriteOperation::WriteTargetDisk).reason ==
              WriteDenyReason::SourceDiskWriteAttempt);
    }

    // S3 -- rollback image is mandatory for irreversible operations.
    {
        WriteGuard g;
        WriteTarget t = GoodTarget();
        t.hasRollbackImage = false;
        g.RegisterTarget(t, {WriteOperation::WritePartitionTable});
        const auto d = g.Authorize("PD3", WriteOperation::WritePartitionTable, "any-token");
        CHECK(!d.allowed);
        CHECK(d.reason == WriteDenyReason::MissingRollbackImage);
    }

    // S3 -- missing / invalid / one-shot / op-bound tokens.
    {
        WriteGuard g = FullyConfigured();
        const WriteOperation op = WriteOperation::WritePartitionTable;

        CHECK(g.Authorize("PD3", op).reason ==
              WriteDenyReason::MissingConfirmToken);
        CHECK(g.Authorize("PD3", op, "forged").reason ==
              WriteDenyReason::InvalidConfirmToken);

        const std::string tok = g.IssueConfirmToken("PD3", op);
        CHECK(g.Authorize("PD3", op, tok).allowed);
        CHECK(g.Authorize("PD3", op, tok).reason ==
              WriteDenyReason::ConfirmTokenReused);

        // A token issued for another operation must not be accepted.
        const std::string tokBoot = g.IssueConfirmToken("PD3", WriteOperation::WriteBootRecord);
        CHECK(g.Authorize("PD3", op, tokBoot).reason ==
              WriteDenyReason::InvalidConfirmToken);
    }

    // Reversible A2 operation must NOT require a confirm token.
    {
        WriteGuard g = FullyConfigured();
        CHECK(g.Authorize("PD3", WriteOperation::WriteTargetDisk).allowed);
    }

    // Existing safe path: writing a brand-new regular file.
    {
        WriteGuard g;
        CHECK(g.AuthorizeNewFile(L"D:\\out\\file.bin").allowed);
        CHECK(!g.AuthorizeNewFile(L"").allowed);
        CHECK(g.AuthorizeNewFile(L"").reason == WriteDenyReason::EmptyPath);
    }

    // Revocation and Clear restore default-deny.
    {
        WriteGuard g = FullyConfigured();
        g.RevokeTarget("PD3");
        CHECK(g.Authorize("PD3", WriteOperation::WriteTargetDisk).reason ==
              WriteDenyReason::TargetNotRegistered);

        WriteGuard h = FullyConfigured();
        h.Clear();
        CHECK(h.Authorize("PD3", WriteOperation::WriteTargetDisk).reason ==
              WriteDenyReason::TargetNotRegistered);
        CHECK(!h.IsSourceId("PD2"));
    }

    std::cout << "WriteGuard logic tests: PASS\n";
    return 0;
}

#else

static int RunDefaultDenyTests()
{
    // S5 -- production default: every device-level write is rejected even
    // when the whitelist, baseline, rollback image and token are all present.
    WriteGuard g = FullyConfigured();
    const std::string tok = g.IssueConfirmToken("PD3", WriteOperation::WritePartitionTable);

    const WriteOperation ops[] = {
        WriteOperation::WriteTargetDisk,
        WriteOperation::WritePartitionTable,
        WriteOperation::WriteBootRecord,
        WriteOperation::IrreversibleWipe,
        WriteOperation::SectorEdit
    };
    for (WriteOperation op : ops) {
        const auto d = g.Authorize("PD3", op, tok);
        CHECK(!d.allowed);
        CHECK(d.reason == WriteDenyReason::PlatformNotSupported);
    }

    // Unregistered targets are also blocked before the whitelist lookup.
    CHECK(!g.Authorize("UNKNOWN", WriteOperation::WriteTargetDisk).allowed);
    CHECK(g.Authorize("UNKNOWN", WriteOperation::WriteTargetDisk).reason ==
          WriteDenyReason::PlatformNotSupported);

    // The pre-existing recover-to-new-file path stays available.
    CHECK(g.AuthorizeNewFile(L"D:\\out\\file.bin").allowed);

    std::cout << "WriteGuard default-deny tests: PASS\n";
    return 0;
}

#endif

int main()
{
#if PHOENIX_ENABLE_DEVICE_WRITES
    return RunLogicTests();
#else
    return RunDefaultDenyTests();
#endif
}
