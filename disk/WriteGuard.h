#pragma once

// ---------------------------------------------------------------------------
// WriteGuard -- A0 write-path safety base, decision layer only.
//
// This header is intentionally header-only, pure C++17, free of Qt / Win32
// dependencies and free of ANY I/O. It never opens a handle and never writes
// a single byte. It is the mandatory gate that the future DeviceWriter must
// call before opening a device for writing (see docs/ROADMAP_RECOVERY.md A0).
//
// Five safety invariants:
//   S1 A device registered as a scan source can never be written in-session.
//   S2 Default deny: unknown target or unauthorised operation is rejected.
//   S3 Irreversible operations need a one-shot confirm token + rollback image.
//   S4 Every device write needs a verifiable baseline (image / full hash).
//   S5 Device writes are compile-time disabled until the A0 gate passes:
//      define PHOENIX_ENABLE_DEVICE_WRITES=1 ONLY in an isolated test build.
//
// Until A0 is accepted, production builds leave the macro at 0, so every
// device-level operation returns PlatformNotSupported. WriteNewFile (the
// existing recover-to-a-new-file path) is unaffected.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifndef PHOENIX_ENABLE_DEVICE_WRITES
#define PHOENIX_ENABLE_DEVICE_WRITES 0
#endif

namespace phoenix {

enum class WriteTargetKind : uint8_t
{
    Unknown = 0,
    RegularFile,   // recovered output file (existing, safe path)
    ImageFile,     // disk image output (existing Imager path)
    TargetDisk,    // destination disk / partition (A2/A3)
    SourceDisk     // scan source -- always forbidden
};

enum class WriteOperation : uint8_t
{
    WriteNewFile = 0,      // A0-safe: recover / image to a brand-new file
    WriteTargetDisk,       // A2: clone, image restore
    WritePartitionTable,   // A3: partition CRUD, MBR<->GPT
    WriteBootRecord,       // A4: rebuild MBR, boot repair
    IrreversibleWipe,      // A4: erase / permanent delete
    SectorEdit             // A4: sector editor write
};

enum class WriteDenyReason : uint8_t
{
    None = 0,
    EmptyPath,
    TargetNotRegistered,
    OperationNotAllowed,
    SourceDiskWriteAttempt,
    MissingBaseline,
    MissingRollbackImage,
    MissingConfirmToken,
    InvalidConfirmToken,
    ConfirmTokenReused,
    PlatformNotSupported
};

inline bool IsDeviceOperation(WriteOperation op)
{
    return op != WriteOperation::WriteNewFile;
}

inline bool RequiresConfirmation(WriteOperation op)
{
    return op == WriteOperation::WritePartitionTable ||
           op == WriteOperation::WriteBootRecord ||
           op == WriteOperation::IrreversibleWipe ||
           op == WriteOperation::SectorEdit;
}

class WriteGuard;

// ---------------------------------------------------------------------------
// WriteGrant -- a non-forgeable, single-use proof of authorisation.
//
// The constructor is private and WriteGuard is the only friend, so the ONLY way
// to obtain a valid grant is WriteGuard::Grant() / WriteGuard::GrantNewFile().
// WriteTransaction (A0-3) takes it by value, which makes the capability chain
//
//     WriteGuard -> WriteGrant -> WriteTransaction -> DeviceWriter
//
// a type-level constraint: no caller can construct a transaction (and hence a
// DeviceWriter) without going through Authorize(). Default/copy construction is
// deleted so a grant cannot be duplicated or replayed.
// ---------------------------------------------------------------------------
class WriteGrant
{
public:
    WriteGrant() = delete;
    WriteGrant(const WriteGrant&) = delete;
    WriteGrant& operator=(const WriteGrant&) = delete;
    WriteGrant(WriteGrant&&) = default;
    WriteGrant& operator=(WriteGrant&&) = default;

    bool Valid() const { return m_allowed; }
    WriteDenyReason Reason() const { return m_reason; }
    const std::string& TargetId() const { return m_targetId; }
    WriteOperation Operation() const { return m_op; }
    bool IsDeviceOperation() const { return phoenix::IsDeviceOperation(m_op); }

    // Consumed exactly once. A rejected grant can never be consumed.
    bool Consume()
    {
        if (m_consumed || !m_allowed) return false;
        m_consumed = true;
        return true;
    }
    bool Consumed() const { return m_consumed; }

private:
    friend class WriteGuard;

    WriteGrant(bool allowed, WriteDenyReason reason,
               std::string targetId, WriteOperation op)
        : m_allowed(allowed)
        , m_reason(reason)
        , m_targetId(std::move(targetId))
        , m_op(op)
    {}

    bool            m_allowed  = false;
    WriteDenyReason m_reason   = WriteDenyReason::TargetNotRegistered;
    std::string     m_targetId;
    WriteOperation  m_op       = WriteOperation::WriteNewFile;
    bool            m_consumed = false;
};

struct WriteTarget
{
    std::string  id;                    // stable identity, e.g. "PD2" or "VOL-E:"
    WriteTargetKind kind = WriteTargetKind::Unknown;
    int          physicalDiskIndex = -1;
    std::wstring volume;                // L"D:" or device path
    std::wstring path;                  // file path for RegularFile / ImageFile
    bool         hasBaseline       = false; // S4: read-only image or full-disk hash
    bool         hasRollbackImage  = false; // S3: verified rollback image
};

struct WriteDecision
{
    bool            allowed = false;
    WriteDenyReason reason  = WriteDenyReason::TargetNotRegistered;

    const char* Detail() const
    {
        switch (reason) {
        case WriteDenyReason::None:                   return "allowed";
        case WriteDenyReason::EmptyPath:              return "empty target path";
        case WriteDenyReason::TargetNotRegistered:    return "target not registered (default deny)";
        case WriteDenyReason::OperationNotAllowed:    return "operation not authorised for this target";
        case WriteDenyReason::SourceDiskWriteAttempt: return "scan source is write-protected";
        case WriteDenyReason::MissingBaseline:        return "missing verifiable baseline";
        case WriteDenyReason::MissingRollbackImage:   return "missing rollback image";
        case WriteDenyReason::MissingConfirmToken:    return "missing confirmation token";
        case WriteDenyReason::InvalidConfirmToken:    return "invalid confirmation token";
        case WriteDenyReason::ConfirmTokenReused:     return "confirmation token already used";
        case WriteDenyReason::PlatformNotSupported:   return "device writes disabled until A0 gate passes";
        }
        return "unknown";
    }
};

class WriteGuard
{
public:
    // Register a scan source. Source registration is permanent for the session.
    void RegisterSource(std::string id, int physicalDiskIndex, std::wstring volume)
    {
        m_sources[id] = Source{physicalDiskIndex, std::move(volume)};
    }

    bool IsSourceId(const std::string& id) const
    {
        return m_sources.find(id) != m_sources.end();
    }

    // Explicit whitelist. A target with no entry, or an operation outside this
    // list, is rejected (S2).
    void RegisterTarget(const WriteTarget& target,
                        std::initializer_list<WriteOperation> ops)
    {
        Entry e;
        e.target = target;
        for (WriteOperation op : ops) {
            e.ops.insert(static_cast<uint8_t>(op));
        }
        m_targets[target.id] = std::move(e);
    }

    void RevokeTarget(const std::string& id)
    {
        m_targets.erase(id);
    }

    // One-shot token bound to (target, operation). Tokens are never reusable.
    std::string IssueConfirmToken(const std::string& targetId, WriteOperation op)
    {
        const std::string prefix = TokenPrefix(targetId, op);
        const std::string token  = prefix + std::to_string(++m_tokenCounter);
        m_issuedTokens.insert(token);
        return token;
    }

    WriteDecision Authorize(const std::string& targetId,
                            WriteOperation op,
                            const std::string& confirmToken = std::string()) const
    {
        // S5: compile-time kill switch for every device-level write.
        if (IsDeviceOperation(op) && !PHOENIX_ENABLE_DEVICE_WRITES) {
            return {false, WriteDenyReason::PlatformNotSupported};
        }

        // S2: default deny for unknown targets.
        const auto it = m_targets.find(targetId);
        if (it == m_targets.end()) {
            return {false, WriteDenyReason::TargetNotRegistered};
        }
        const WriteTarget& target = it->second.target;

        // S1: scan sources are never writable, even if explicitly whitelisted.
        if (target.kind == WriteTargetKind::SourceDisk ||
            IsSourceId(targetId) || MatchesSource(target)) {
            return {false, WriteDenyReason::SourceDiskWriteAttempt};
        }

        // S2: operation must be in the per-target whitelist.
        if (it->second.ops.find(static_cast<uint8_t>(op)) == it->second.ops.end()) {
            return {false, WriteDenyReason::OperationNotAllowed};
        }

        // S4: verifiable baseline before touching any device.
        if (!target.hasBaseline) {
            return {false, WriteDenyReason::MissingBaseline};
        }

        // S3: irreversible operations need rollback image + one-shot token.
        if (RequiresConfirmation(op)) {
            if (!target.hasRollbackImage) {
                return {false, WriteDenyReason::MissingRollbackImage};
            }
            if (confirmToken.empty()) {
                return {false, WriteDenyReason::MissingConfirmToken};
            }
            if (m_usedTokens.find(confirmToken) != m_usedTokens.end()) {
                return {false, WriteDenyReason::ConfirmTokenReused};
            }
            if (m_issuedTokens.find(confirmToken) == m_issuedTokens.end() ||
                confirmToken.compare(0, TokenPrefix(targetId, op).size(),
                                     TokenPrefix(targetId, op)) != 0) {
                return {false, WriteDenyReason::InvalidConfirmToken};
            }
            m_usedTokens.insert(confirmToken);
        }

        return {true, WriteDenyReason::None};
    }

    // Existing safe path: writing a brand-new regular file. Same-volume
    // protection remains the responsibility of RecoverEngine::ValidateTargetPath.
    WriteDecision AuthorizeNewFile(const std::wstring& path) const
    {
        if (path.empty()) {
            return {false, WriteDenyReason::EmptyPath};
        }
        return {true, WriteDenyReason::None};
    }

    // Capability factory (A0-3). Every WriteGrant in the process comes from
    // here; there is no public constructor and no way to fake `allowed`.
    WriteGrant Grant(const std::string& targetId,
                     WriteOperation op,
                     const std::string& confirmToken = std::string()) const
    {
        const WriteDecision d = Authorize(targetId, op, confirmToken);
        return WriteGrant(d.allowed, d.reason, targetId, op);
    }

    // Grant for the pre-existing recover-to-new-file path. This bypasses the
    // device whitelist on purpose (it is the one operation that never touches
    // a device handle), but it is still a first-class grant so the transaction
    // and rollback machinery can be tested in production builds.
    WriteGrant GrantNewFile(const std::wstring& path) const
    {
        const WriteDecision d = AuthorizeNewFile(path);
        return WriteGrant(d.allowed, d.reason, NarrowAscii(path),
                          WriteOperation::WriteNewFile);
    }

    void Clear()
    {
        m_sources.clear();
        m_targets.clear();
        m_issuedTokens.clear();
        m_usedTokens.clear();
        m_tokenCounter = 0;
    }

private:
    static std::string TokenPrefix(const std::string& targetId, WriteOperation op)
    {
        return "WG|" + targetId + "|" +
               std::to_string(static_cast<int>(op)) + "|";
    }

    // Stable, loss-tolerant id for a wide path. Non-ASCII chars collapse to
    // '?' -- this is only an identity label, never a path used for I/O.
    static std::string NarrowAscii(const std::wstring& s)
    {
        std::string out;
        out.reserve(s.size());
        for (wchar_t c : s) {
            const unsigned int u = static_cast<unsigned int>(c);
            out.push_back(u <= 127 ? static_cast<char>(u) : '?');
        }
        return out;
    }

    static bool VolumeEquals(const std::wstring& a, const std::wstring& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            wchar_t ca = a[i], cb = b[i];
            if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
            if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'A' + L'a');
            if (ca != cb) return false;
        }
        return true;
    }

    bool MatchesSource(const WriteTarget& t) const
    {
        for (const auto& kv : m_sources) {
            const Source& s = kv.second;
            if (t.physicalDiskIndex >= 0 && s.disk >= 0 &&
                t.physicalDiskIndex == s.disk) {
                return true;
            }
            if (!t.volume.empty() && !s.volume.empty() &&
                VolumeEquals(t.volume, s.volume)) {
                return true;
            }
        }
        return false;
    }

    struct Source
    {
        int         disk = -1;
        std::wstring volume;
    };

    struct Entry
    {
        WriteTarget target;
        std::unordered_set<uint8_t> ops;
    };

    std::unordered_map<std::string, Source> m_sources;
    std::unordered_map<std::string, Entry>  m_targets;
    std::unordered_set<std::string>        m_issuedTokens;
    mutable std::unordered_set<std::string> m_usedTokens;
    uint64_t m_tokenCounter = 0;
};

} // namespace phoenix
