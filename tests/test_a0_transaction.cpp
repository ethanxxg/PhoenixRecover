// A0-3: unit tests for disk/WriteTransaction.h and disk/DeviceWriter.h
//
// Built twice by CTest:
//   A0Transaction            -> PHOENIX_ENABLE_DEVICE_WRITES=1 (isolated logic)
//   A0TransactionDefaultDeny -> PHOENIX_ENABLE_DEVICE_WRITES=0 (production)
//
// NO handle is opened, NO byte is written, in either build.

#include "disk/DeviceWriter.h"
#include "disk/WriteGuard.h"
#include "disk/WriteTransaction.h"

#include <iostream>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::cerr << "FAIL line " << __LINE__ << ": " #cond << "\n";   \
            return 1;                                                      \
        }                                                                  \
    } while (0)

using namespace phoenix;

static_assert(!std::is_default_constructible<WriteGrant>::value,
              "WriteGrant must not be default-constructible");
static_assert(!std::is_copy_constructible<WriteGrant>::value,
              "WriteGrant must not be copyable");

static WriteTarget GoodTarget()
{
    WriteTarget t;
    t.id                = "PD3";
    t.kind              = WriteTargetKind::TargetDisk;
    t.physicalDiskIndex = 3;
    t.volume            = L"F:";
    t.hasBaseline       = true;
    t.hasRollbackImage  = true;
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

static WriteIntent MakeIntent(const std::string& target, WriteOperation op,
                              uint64_t off, uint64_t len)
{
    WriteIntent in;
    in.targetId     = target;
    in.op           = op;
    in.offsetBytes  = off;
    in.lengthBytes  = len;
    in.beforeDigest = "before" + std::to_string(off);
    in.afterDigest  = "after"  + std::to_string(off);
    in.note         = "sector";
    return in;
}

// ---------------------------------------------------------------------------
// Core transaction/journal tests. Run under BOTH macro values: with a device
// grant in the isolated build, and with the safe new-file grant in production.
// This keeps rollback/recovery verification available in every build.
// ---------------------------------------------------------------------------
template <class GrantFactory, class MismatchFactory>
static int CoreTests(GrantFactory makeGrant, MismatchFactory makeMismatchGrant)
{
    WriteGrant probe = makeGrant();
    CHECK(probe.Valid());
    CHECK(probe.Consume());
    CHECK(!probe.Consume());                 // single-use capability
    const std::string   target = probe.TargetId();
    const WriteOperation op    = probe.Operation();

    // Construction / S4 baseline is immutable once fixed.
    {
        WriteTransaction tx(makeGrant());
        CHECK(tx.State() == TxState::Open);
        CHECK(tx.HasValidGrant());
        CHECK(!tx.IsReadyToWrite());
        CHECK(tx.SetBaseline("BASELINE-A"));
        CHECK(!tx.SetBaseline("BASELINE-B"));
        CHECK(tx.IsReadyToWrite());
        CHECK(tx.Prepare() == TxResult::EmptyJournal);
    }
    {
        WriteTransaction tx(makeGrant());
        CHECK(tx.Prepare() == TxResult::MissingBaseline);
    }

    // Intent validation: range, digests, delimiters, target, operation, overlap.
    {
        WriteTransaction tx(makeGrant());
        CHECK(tx.SetBaseline("BASELINE-A"));
        CHECK(tx.Record(MakeIntent(target, op, 0, 4096)) == TxResult::Ok);

        WriteIntent bad = MakeIntent(target, op, 4096, 0);
        CHECK(tx.Record(bad) == TxResult::InvalidRange);
        bad = MakeIntent(target, op, std::numeric_limits<uint64_t>::max(), 4096);
        CHECK(tx.Record(bad) == TxResult::InvalidRange);

        bad = MakeIntent(target, op, 4096, 4096);
        bad.beforeDigest.clear();
        CHECK(tx.Record(bad) == TxResult::MissingDigest);

        bad = MakeIntent(target, op, 4096, 4096);
        bad.afterDigest = "after|4096";
        CHECK(tx.Record(bad) == TxResult::UnsafeField);

        bad = MakeIntent(target, op, 4096, 4096);
        bad.targetId = "SOMEONE-ELSE";
        CHECK(tx.Record(bad) == TxResult::TargetMismatch);

        bad = MakeIntent(target, WriteOperation::IrreversibleWipe, 4096, 4096);
        CHECK(tx.Record(bad) == TxResult::OperationMismatch);

        bad = MakeIntent(target, op, 2048, 4096);
        CHECK(tx.Record(bad) == TxResult::OverlappingRange);

        CHECK(tx.Record(MakeIntent(target, op, 4096, 4096)) == TxResult::Ok);
        CHECK(tx.Intents().size() == 2);
    }

    // Prepare -> Commit seals the journal; post-condition is verifiable.
    {
        WriteTransaction tx(makeGrant());
        CHECK(tx.SetBaseline("BASELINE-A"));
        CHECK(tx.Record(MakeIntent(target, op, 0, 512)) == TxResult::Ok);
        CHECK(tx.Record(MakeIntent(target, op, 512, 512)) == TxResult::Ok);
        CHECK(tx.VerifyPostCondition("after512"));
        CHECK(!tx.VerifyPostCondition("garbage"));
        CHECK(tx.Prepare() == TxResult::Ok);
        CHECK(tx.Record(MakeIntent(target, op, 1024, 512)) == TxResult::NotOpen);
        CHECK(tx.Commit() == TxResult::Ok);
        CHECK(tx.Commit() == TxResult::BadState);
    }

    // Rollback plan is strict LIFO and covers every intent.
    {
        WriteTransaction tx(makeGrant());
        CHECK(tx.SetBaseline("BASELINE-A"));
        for (uint64_t i = 0; i < 3; ++i) {
            CHECK(tx.Record(MakeIntent(target, op, i * 512, 512)) == TxResult::Ok);
        }
        const auto plan = tx.RollbackPlan();
        CHECK(plan.size() == 3);
        CHECK(plan[0].offsetBytes == 1024);
        CHECK(plan[1].offsetBytes == 512);
        CHECK(plan[2].offsetBytes == 0);
        CHECK(plan[0].beforeDigest == "before1024");

        // Rollback only closes on a verified baseline restoration.
        CHECK(tx.Rollback("WRONG-HASH") == TxResult::BaselineMismatch);
        CHECK(tx.State() != TxState::RolledBack);
        CHECK(tx.Rollback("BASELINE-A") == TxResult::Ok);
        CHECK(tx.State() == TxState::RolledBack);
        CHECK(tx.Rollback("BASELINE-A") == TxResult::AlreadyRolledBack);
        CHECK(!tx.RollbackPlan().empty());       // audit trail survives
    }

    // Abort path (interrupted operation).
    {
        WriteTransaction tx(makeGrant());
        CHECK(tx.SetBaseline("BASELINE-A"));
        CHECK(tx.Record(MakeIntent(target, op, 0, 512)) == TxResult::Ok);
        CHECK(tx.Abort() == TxResult::Ok);
        CHECK(tx.State() == TxState::Aborted);
        CHECK(tx.Record(MakeIntent(target, op, 512, 512)) == TxResult::NotOpen);
        CHECK(tx.Rollback("BASELINE-A") == TxResult::Ok);
    }

    // Journal codec: checksum detects truncation / tampering; malformed input
    // is rejected instead of being trusted.
    std::string committedJournal;
    std::string openJournal;
    {
        WriteTransaction tx(makeGrant());
        CHECK(tx.SetBaseline("BASELINE-A"));
        CHECK(tx.Record(MakeIntent(target, op, 0, 512)) == TxResult::Ok);
        openJournal = tx.JournalText();
        CHECK(tx.Prepare() == TxResult::Ok);
        CHECK(tx.Commit() == TxResult::Ok);
        committedJournal = tx.JournalText();

        const RecoveredJournal j = ParseJournal(committedJournal);
        CHECK(j.ok);
        CHECK(j.checksum != 0);
        CHECK(j.targetId == target);
        CHECK(j.op == op);
        CHECK(j.baselineDigest == "BASELINE-A");
        CHECK(j.stateAtCrash == TxState::Committed);
        CHECK(j.intents.size() == 1);

        const size_t p = committedJournal.find("INTENT|");
        CHECK(p != std::string::npos);
        const size_t e = committedJournal.find('\n', p);
        CHECK(e != std::string::npos);
        const std::string truncated =
            committedJournal.substr(0, p) + committedJournal.substr(e + 1);
        CHECK(ParseJournal(truncated).error == TxResult::ChecksumMismatch);

        std::string tampered = committedJournal;
        const size_t b = tampered.find("BASE|");
        CHECK(b != std::string::npos);
        tampered[b + 5] = (tampered[b + 5] == 'a') ? 'b' : 'a';
        CHECK(ParseJournal(tampered).error == TxResult::ChecksumMismatch);

        CHECK(ParseJournal("junk").error == TxResult::MalformedJournal);
        CHECK(ParseJournal("").error == TxResult::MalformedJournal);
    }

    // Crash recovery: committed -> Committed; anything else -> Aborted
    // (roll back, never roll forward).
    {
        TxResult out = TxResult::Ok;
        WriteTransaction a = WriteTransaction::Resume(makeGrant(), committedJournal, out);
        CHECK(out == TxResult::Ok);
        CHECK(a.State() == TxState::Committed);
        CHECK(a.Intents().size() == 1);
        CHECK(a.Rollback("BASELINE-A") == TxResult::Ok);
    }
    {
        TxResult out = TxResult::Ok;
        WriteTransaction a = WriteTransaction::Resume(makeGrant(), openJournal, out);
        CHECK(out == TxResult::Ok);
        CHECK(a.State() == TxState::Aborted);
        CHECK(a.Rollback("BASELINE-A") == TxResult::Ok);
    }
    {
        TxResult out = TxResult::Ok;
        WriteTransaction a = WriteTransaction::Resume(makeGrant(), "junk", out);
        CHECK(out == TxResult::MalformedJournal);
        CHECK(a.State() == TxState::Rejected);
    }
    {
        TxResult out = TxResult::Ok;
        WriteTransaction a = WriteTransaction::Resume(makeMismatchGrant(),
                                                      committedJournal, out);
        CHECK(out == TxResult::TargetMismatch ||
              out == TxResult::OperationMismatch);
        CHECK(a.State() == TxState::Rejected);
    }

    return 0;
}

#if PHOENIX_ENABLE_DEVICE_WRITES

static int RunLogicTests()
{
    WriteGuard g = FullyConfigured();

    // Capability chain: guard -> grant -> transaction -> writer.
    {
        WriteGrant grant = g.Grant("PD3", WriteOperation::WriteTargetDisk);
        CHECK(grant.Valid());
        CHECK(grant.IsDeviceOperation());
        WriteTransaction tx(std::move(grant));
        CHECK(tx.SetBaseline("BASELINE-A"));

        DeviceWriter w(tx);
        CHECK(w.IsArmed());
        const WriteReceipt r = w.WriteSectors(0, 4096, "before0", "after0", "mbr");
        CHECK(r.IsPlanned());
        CHECK(std::string(r.Detail()).find("no I/O") != std::string::npos);
        CHECK(tx.Intents().size() == 1);

        CHECK(w.WriteSectors(4096, 4096, "before4096", "after4096").IsPlanned());
        CHECK(w.WriteSectors(2048, 4096, "b", "a").status ==
              WriteReceiptStatus::RecordFailed);
        CHECK(w.WriteSectors(2048, 4096, "b", "a").txResult ==
              TxResult::OverlappingRange);
        CHECK(w.WriteSectors(8192, 4096, "", "a").status ==
              WriteReceiptStatus::MissingDigest);
        CHECK(tx.Intents().size() == 2);

        CHECK(tx.Prepare() == TxResult::Ok);
        CHECK(!w.IsArmed());            // journal sealed, no more writes
        CHECK(w.WriteSectors(16384, 4096, "b", "a").status ==
              WriteReceiptStatus::NotArmed);
    }

    // S1: a grant for a registered scan source can never arm a writer.
    {
        WriteTarget src = GoodTarget();
        src.id = "PD2";
        src.physicalDiskIndex = 2;
        src.volume = L"e:";
        g.RegisterTarget(src, {WriteOperation::WriteTargetDisk});

        WriteGrant grant = g.Grant("PD2", WriteOperation::WriteTargetDisk);
        CHECK(!grant.Valid());
        CHECK(grant.Reason() == WriteDenyReason::SourceDiskWriteAttempt);
        WriteTransaction tx(std::move(grant));
        CHECK(tx.State() == TxState::Rejected);
        CHECK(tx.GuardReason() == WriteDenyReason::SourceDiskWriteAttempt);
        DeviceWriter w(tx);
        CHECK(!w.IsArmed());
        CHECK(w.WriteSectors(0, 512, "b", "a").status ==
              WriteReceiptStatus::NotArmed);
    }

    // S3: one-shot confirmation token for irreversible operations.
    {
        const WriteOperation op = WriteOperation::WritePartitionTable;
        CHECK(g.Grant("PD3", op).Reason() ==
              WriteDenyReason::MissingConfirmToken);
        const std::string tok = g.IssueConfirmToken("PD3", op);
        CHECK(g.Grant("PD3", op, tok).Valid());
        CHECK(g.Grant("PD3", op, tok).Reason() ==
              WriteDenyReason::ConfirmTokenReused);
    }

    // A new-file grant is valid but can never arm the device writer.
    {
        WriteGrant grant = g.GrantNewFile(L"D:\\out\\recovered.bin");
        CHECK(grant.Valid());
        CHECK(!grant.IsDeviceOperation());
        WriteTransaction tx(std::move(grant));
        CHECK(tx.SetBaseline("BASELINE-A"));
        DeviceWriter w(tx);
        CHECK(!w.IsArmed());
    }

    // Core journal / rollback tests on a real device grant.
    auto makeGrant = [&g]() { return g.Grant("PD3", WriteOperation::WriteTargetDisk); };
    auto makeMismatch = [&g]() {
        const WriteOperation op = WriteOperation::WritePartitionTable;
        return g.Grant("PD3", op, g.IssueConfirmToken("PD3", op));
    };
    if (CoreTests(makeGrant, makeMismatch) != 0) return 1;

    std::cout << "A0 transaction / device-writer logic tests: PASS\n";
    return 0;
}

#else

static int RunDefaultDenyTests()
{
    WriteGuard g = FullyConfigured();

    // S5: no device grant can be obtained in a production build.
    {
        const auto d = g.Authorize("PD3", WriteOperation::WriteTargetDisk);
        CHECK(!d.allowed);
        CHECK(d.reason == WriteDenyReason::PlatformNotSupported);

        WriteGrant grant = g.Grant("PD3", WriteOperation::WriteTargetDisk);
        CHECK(!grant.Valid());
        WriteTransaction tx(std::move(grant));
        CHECK(tx.State() == TxState::Rejected);
        CHECK(!tx.IsReadyToWrite());

        DeviceWriter w(tx);
        CHECK(!w.IsArmed());
        const WriteReceipt r = w.WriteSectors(0, 4096, "b", "a");
        CHECK(r.status == WriteReceiptStatus::WritesDisabled);
        CHECK(r.guardReason == WriteDenyReason::PlatformNotSupported);
        CHECK(tx.Intents().empty());
    }

    // Even a fully valid new-file transaction cannot arm a device writer.
    {
        WriteGrant grant = g.GrantNewFile(L"D:\\out\\recovered.bin");
        CHECK(grant.Valid());
        WriteTransaction tx(std::move(grant));
        CHECK(tx.SetBaseline("BASELINE-A"));
        DeviceWriter w(tx);
        CHECK(!w.IsArmed());
        CHECK(w.WriteSectors(0, 512, "b", "a").status ==
              WriteReceiptStatus::WritesDisabled);
        CHECK(tx.Intents().empty());
    }

    // Journal / rollback logic remains fully testable in the production build.
    auto makeGrant = [&g]() {
        return g.GrantNewFile(L"D:\\out\\recovered.bin");
    };
    auto makeMismatch = [&g]() {
        return g.GrantNewFile(L"D:\\out\\another.bin");
    };
    if (CoreTests(makeGrant, makeMismatch) != 0) return 1;

    std::cout << "A0 transaction default-deny tests: PASS\n";
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
