#pragma once

// ---------------------------------------------------------------------------
// WriteTransaction -- A0-3 transaction journal + rollback planner.
//
// STILL ZERO I/O. This header never opens a handle, never writes a byte and
// never persists the journal to disk. It models the safety contract that the
// future real writer must implement:
//
//   * a transaction can only be created from a valid, single-use WriteGrant
//     issued by WriteGuard (no default constructor, no forging);
//   * every write intent records BOTH the before-image digest and the expected
//     after-image digest, so a write can be verified and undone;
//   * overlapping ranges are rejected, otherwise LIFO rollback would restore
//     stale sectors and silently corrupt the baseline;
//   * the journal carries a checksum, so an interrupted / truncated journal is
//     detected on resume instead of being trusted;
//   * a resume of anything not cleanly committed lands in Aborted state, i.e.
//     crash recovery is "roll back, never roll forward".
//
// The baseline digest is supplied by the caller. In a real A2/A3 build it will
// come from a read-only full-disk image or a full-disk hash (invariant S4).
// ---------------------------------------------------------------------------

#include "disk/WriteGuard.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace phoenix {

enum class TxState : uint8_t
{
    Open      = 0,  // recording intents
    Prepared  = 1,  // journal sealed, ready to commit
    Committed = 2,  // applied (rollback still available)
    RolledBack= 3,  // before-images restored and verified
    Aborted   = 4,  // interrupted / explicitly aborted, rollback pending
    Rejected  = 5   // grant or journal was invalid -- no write is possible
};

enum class TxResult : uint8_t
{
    Ok = 0,
    NotOpen,
    BadState,
    TargetMismatch,
    OperationMismatch,
    InvalidRange,
    MissingDigest,
    UnsafeField,
    OverlappingRange,
    MissingBaseline,
    EmptyJournal,
    AlreadyRolledBack,
    BaselineMismatch,
    MalformedJournal,
    ChecksumMismatch,
    TooManyIntents
};

inline const char* TxResultDetail(TxResult r)
{
    switch (r) {
    case TxResult::Ok:                 return "ok";
    case TxResult::NotOpen:            return "transaction is not open for recording";
    case TxResult::BadState:           return "operation not allowed in current state";
    case TxResult::TargetMismatch:     return "intent target does not match the grant";
    case TxResult::OperationMismatch:  return "intent operation does not match the grant";
    case TxResult::InvalidRange:       return "empty or overflowing write range";
    case TxResult::MissingDigest:      return "before/after image digest missing";
    case TxResult::UnsafeField:        return "field contains a journal delimiter";
    case TxResult::OverlappingRange:   return "write range overlaps a recorded intent";
    case TxResult::MissingBaseline:    return "no verifiable baseline digest";
    case TxResult::EmptyJournal:       return "journal is empty";
    case TxResult::AlreadyRolledBack:  return "rollback already completed";
    case TxResult::BaselineMismatch:   return "post-rollback digest differs from baseline";
    case TxResult::MalformedJournal:   return "journal is malformed";
    case TxResult::ChecksumMismatch:   return "journal checksum mismatch (interrupted write)";
    case TxResult::TooManyIntents:     return "journal exceeds intent limit";
    }
    return "unknown";
}

struct WriteIntent
{
    std::string    targetId;
    WriteOperation op           = WriteOperation::WriteNewFile;
    uint64_t       offsetBytes  = 0;
    uint64_t       lengthBytes  = 0;
    std::string    beforeDigest;  // snapshot of the range BEFORE writing
    std::string    afterDigest;   // expected digest of the range AFTER writing
    std::string    note;
};

// --------------------------- journal codec (pure) --------------------------

inline uint64_t TxFnv1a64(const std::string& s)
{
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

inline std::string HexU64(uint64_t v)
{
    static const char* kDigits = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<size_t>(i)] = kDigits[v & 0xFULL];
        v >>= 4;
    }
    return out;
}

inline bool ParseHexU64(const std::string& s, uint64_t& out)
{
    if (s.empty() || s.size() > 16) return false;
    uint64_t v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9')      d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    out = v;
    return true;
}

inline bool ParseU64Dec(const std::string& s, uint64_t& out)
{
    if (s.empty() || s.size() > 20) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const uint64_t d = static_cast<uint64_t>(c - '0');
        if (v > (std::numeric_limits<uint64_t>::max() - d) / 10) return false;
        v = v * 10 + d;
    }
    out = v;
    return true;
}

inline std::vector<std::string> SplitBy(const std::string& s, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

inline bool JournalSafe(const std::string& s)
{
    for (char c : s) {
        if (c == '|' || c == '\n' || c == '\r' || c == '\0') return false;
    }
    return true;
}

struct RecoveredJournal
{
    bool            ok            = false;
    TxResult        error         = TxResult::MalformedJournal;
    std::string     targetId;
    WriteOperation  op            = WriteOperation::WriteNewFile;
    TxState         stateAtCrash  = TxState::Open;
    std::string     baselineDigest;
    std::vector<WriteIntent> intents;
    uint64_t        checksum      = 0;
};

inline RecoveredJournal ParseJournal(const std::string& text)
{
    RecoveredJournal r;
    const std::vector<std::string> lines = SplitBy(text, '\n');
    if (lines.size() < 5) { r.error = TxResult::MalformedJournal; return r; }

    // Strip CR (Windows line endings) from a copy of every line.
    std::vector<std::string> clean;
    clean.reserve(lines.size());
    for (const std::string& line : lines) {
        std::string s = line;
        if (!s.empty() && s.back() == '\r') s.pop_back();
        clean.push_back(s);
    }

    const std::string& tail = clean.back();
    const std::vector<std::string> sumFields = SplitBy(tail, '|');
    if (sumFields.size() != 2 || sumFields[0] != "CHECKSUM") {
        r.error = TxResult::MalformedJournal;
        return r;
    }
    uint64_t claimed = 0;
    if (!ParseHexU64(sumFields[1], claimed)) {
        r.error = TxResult::MalformedJournal;
        return r;
    }

    std::string body;
    for (size_t i = 0; i + 1 < clean.size(); ++i) {
        if (i) body.push_back('\n');
        body += clean[i];
    }
    if (TxFnv1a64(body) != claimed) {
        r.error = TxResult::ChecksumMismatch;
        return r;
    }
    r.checksum = claimed;

    if (clean[0] != "PHOENIX_TX|1") {
        r.error = TxResult::MalformedJournal; return r;
    }

    const std::vector<std::string> grant = SplitBy(clean[1], '|');
    if (grant.size() != 3 || grant[0] != "GRANT" ||
        !JournalSafe(grant[1]) || grant[1].empty()) {
        r.error = TxResult::MalformedJournal; return r;
    }
    uint64_t opVal = 0;
    if (!ParseU64Dec(grant[2], opVal) || opVal > 5) {
        r.error = TxResult::MalformedJournal; return r;
    }
    r.targetId = grant[1];
    r.op       = static_cast<WriteOperation>(static_cast<uint8_t>(opVal));

    const std::vector<std::string> state = SplitBy(clean[2], '|');
    uint64_t stateVal = 0;
    if (state.size() != 2 || state[0] != "STATE" ||
        !ParseU64Dec(state[1], stateVal) || stateVal > 5) {
        r.error = TxResult::MalformedJournal; return r;
    }
    r.stateAtCrash = static_cast<TxState>(static_cast<uint8_t>(stateVal));

    const std::vector<std::string> base = SplitBy(clean[3], '|');
    if (base.size() != 2 || base[0] != "BASE" || base[1].empty() ||
        !JournalSafe(base[1])) {
        r.error = TxResult::MalformedJournal; return r;
    }
    r.baselineDigest = base[1];

    const size_t kMaxIntents = 100000;
    if (clean.size() - 5 > kMaxIntents) {
        r.error = TxResult::TooManyIntents; return r;
    }

    for (size_t i = 4; i + 1 < clean.size(); ++i) {
        const std::vector<std::string> f = SplitBy(clean[i], '|');
        if (f.size() != 8 || f[0] != "INTENT") {
            r.error = TxResult::MalformedJournal; return r;
        }
        WriteIntent in;
        in.targetId     = f[1];
        uint64_t op = 0;
        if (!ParseU64Dec(f[2], op) || op > 5 ||
            !ParseHexU64(f[3], in.offsetBytes) ||
            !ParseHexU64(f[4], in.lengthBytes) ||
            f[5].empty() || f[6].empty() ||
            !JournalSafe(f[5]) || !JournalSafe(f[6]) || !JournalSafe(f[7])) {
            r.error = TxResult::MalformedJournal; return r;
        }
        in.op           = static_cast<WriteOperation>(static_cast<uint8_t>(op));
        in.beforeDigest = f[5];
        in.afterDigest  = f[6];
        in.note         = f[7];

        if (in.targetId != r.targetId || in.op != r.op ||
            in.lengthBytes == 0 ||
            in.lengthBytes > std::numeric_limits<uint64_t>::max() - in.offsetBytes) {
            r.error = TxResult::MalformedJournal; return r;
        }
        for (const WriteIntent& prev : r.intents) {
            if (in.offsetBytes < prev.offsetBytes + prev.lengthBytes &&
                prev.offsetBytes < in.offsetBytes + in.lengthBytes) {
                r.error = TxResult::OverlappingRange; return r;
            }
        }
        r.intents.push_back(std::move(in));
    }

    r.ok    = true;
    r.error = TxResult::Ok;
    return r;
}

// ------------------------------- transaction -------------------------------

class WriteTransaction
{
public:
    WriteTransaction() = delete;                       // no grant => no transaction
    WriteTransaction(const WriteTransaction&) = delete;
    WriteTransaction& operator=(const WriteTransaction&) = delete;
    WriteTransaction(WriteTransaction&&) = default;
    WriteTransaction& operator=(WriteTransaction&&) = default;

    explicit WriteTransaction(WriteGrant grant)
        : m_grant(std::move(grant))
    {
        if (!m_grant.Valid() || !m_grant.Consume()) {
            m_guardReason = m_grant.Reason();
            m_state = TxState::Rejected;
        } else {
            m_state = TxState::Open;
        }
    }

    // S4: a verifiable baseline must be fixed once, before any intent.
    bool SetBaseline(const std::string& digest)
    {
        if (m_state != TxState::Open || digest.empty() ||
            !m_baselineDigest.empty() || !JournalSafe(digest)) {
            return false;
        }
        m_baselineDigest = digest;
        return true;
    }

    TxResult Record(const WriteIntent& in)
    {
        if (m_state != TxState::Open)          return TxResult::NotOpen;
        if (in.targetId != TargetId())         return TxResult::TargetMismatch;
        if (in.op != Operation())              return TxResult::OperationMismatch;
        if (in.lengthBytes == 0 ||
            in.lengthBytes > std::numeric_limits<uint64_t>::max() - in.offsetBytes) {
            return TxResult::InvalidRange;
        }
        if (in.beforeDigest.empty() || in.afterDigest.empty()) {
            return TxResult::MissingDigest;
        }
        if (!JournalSafe(in.beforeDigest) || !JournalSafe(in.afterDigest) ||
            !JournalSafe(in.note) || !JournalSafe(in.targetId)) {
            return TxResult::UnsafeField;
        }
        for (const WriteIntent& prev : m_intents) {
            if (in.offsetBytes < prev.offsetBytes + prev.lengthBytes &&
                prev.offsetBytes < in.offsetBytes + in.lengthBytes) {
                return TxResult::OverlappingRange;
            }
        }
        m_intents.push_back(in);
        return TxResult::Ok;
    }

    TxResult Prepare()
    {
        if (m_state != TxState::Open)       return TxResult::BadState;
        if (m_baselineDigest.empty())       return TxResult::MissingBaseline;
        if (m_intents.empty())              return TxResult::EmptyJournal;
        m_state = TxState::Prepared;
        return TxResult::Ok;
    }

    TxResult Commit()
    {
        if (m_state != TxState::Prepared) return TxResult::BadState;
        m_state = TxState::Committed;
        return TxResult::Ok;
    }

    TxResult Abort()
    {
        if (m_state != TxState::Open && m_state != TxState::Prepared) {
            return TxResult::BadState;
        }
        m_state = TxState::Aborted;
        return TxResult::Ok;
    }

    // LIFO: the last write must be the first one undone.
    std::vector<WriteIntent> RollbackPlan() const
    {
        if (m_state == TxState::Rejected) return {};
        std::vector<WriteIntent> plan(m_intents.rbegin(), m_intents.rend());
        return plan;
    }

    // Only a verified restoration of the baseline closes the transaction.
    // Rollback is deliberately allowed from Open/Prepared/Committed/Aborted:
    // a crash during intent recording must still be recoverable. The only
    // states that cannot roll back are Rejected (never had a valid grant) and
    // RolledBack (already done).
    TxResult Rollback(const std::string& observedBaselineDigest)
    {
        if (m_state == TxState::Rejected) return TxResult::BadState;
        if (m_state == TxState::RolledBack) return TxResult::AlreadyRolledBack;
        if (observedBaselineDigest != m_baselineDigest) {
            return TxResult::BaselineMismatch;
        }
        m_state = TxState::RolledBack;
        return TxResult::Ok;
    }

    bool VerifyPostCondition(const std::string& observedDigest) const
    {
        return !m_intents.empty() && m_intents.back().afterDigest == observedDigest;
    }

    std::string JournalText() const
    {
        std::string body;
        body += "PHOENIX_TX|1";
        body += "\nGRANT|" + TargetId() + "|" +
                std::to_string(static_cast<int>(Operation()));
        body += "\nSTATE|" + std::to_string(static_cast<int>(m_state));
        body += "\nBASE|" + m_baselineDigest;
        for (const WriteIntent& in : m_intents) {
            body += "\nINTENT|" + in.targetId + "|" +
                    std::to_string(static_cast<int>(in.op)) + "|" +
                    HexU64(in.offsetBytes) + "|" + HexU64(in.lengthBytes) + "|" +
                    in.beforeDigest + "|" + in.afterDigest + "|" + in.note;
        }
        return body + "\nCHECKSUM|" + HexU64(TxFnv1a64(body));
    }

    // Crash recovery. A fresh grant is required: a process restart never
    // inherits a capability. Anything not cleanly committed becomes Aborted,
    // forcing a rollback rather than a roll-forward.
    static WriteTransaction Resume(WriteGrant grant,
                                   const std::string& journalText,
                                   TxResult& out)
    {
        WriteTransaction tx(std::move(grant));
        const RecoveredJournal j = ParseJournal(journalText);
        if (!j.ok) {
            out = j.error;
            tx.Reject();
            return tx;
        }
        if (tx.TargetId() != j.targetId) {
            out = TxResult::TargetMismatch; tx.Reject(); return tx;
        }
        if (tx.Operation() != j.op) {
            out = TxResult::OperationMismatch; tx.Reject(); return tx;
        }
        tx.m_baselineDigest = j.baselineDigest;
        tx.m_intents        = j.intents;
        tx.m_state          = (j.stateAtCrash == TxState::Committed)
                              ? TxState::Committed : TxState::Aborted;
        out = TxResult::Ok;
        return tx;
    }

    // --- introspection (used by DeviceWriter + tests) ---
    TxState             State() const          { return m_state; }
    const std::string&  TargetId() const      { return m_grant.TargetId(); }
    WriteOperation      Operation() const     { return m_grant.Operation(); }
    const std::string&  Baseline() const      { return m_baselineDigest; }
    const std::vector<WriteIntent>& Intents() const { return m_intents; }
    bool HasValidGrant() const { return m_grant.Valid() && m_grant.Consumed(); }
    bool UsesDeviceOperation() const { return m_grant.IsDeviceOperation(); }
    bool IsReadyToWrite() const
    {
        return m_state == TxState::Open && !m_baselineDigest.empty();
    }
    WriteDenyReason GuardReason() const { return m_guardReason; }

private:
    void Reject() { m_state = TxState::Rejected; }

    WriteGrant      m_grant;
    WriteDenyReason m_guardReason = WriteDenyReason::None;
    TxState         m_state       = TxState::Rejected;
    std::string     m_baselineDigest;
    std::vector<WriteIntent> m_intents;
};

} // namespace phoenix
