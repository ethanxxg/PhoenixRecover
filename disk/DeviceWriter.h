#pragma once

// ---------------------------------------------------------------------------
// DeviceWriter -- A0-3 write-handle capability skeleton.
//
// IMPORTANT: this class performs NO I/O. It does not include windows.h, does
// not call CreateFile/WriteFile and does not hold any handle. It exists to fix
// the *shape* of the future write path before any real writer is written:
//
//   1. DeviceWriter has no default constructor and is not copyable. Its only
//      constructor requires a live WriteTransaction.
//   2. A WriteTransaction can only be built from a WriteGrant, whose ctor is
//      private to WriteGuard (see WriteGuard.h). So there is no way to reach
//      WriteSectors() without passing through WriteGuard::Authorize().
//   3. IsArmed() additionally requires PHOENIX_ENABLE_DEVICE_WRITES=1, a
//      consumed device-operation grant, and a transaction that is Open with a
//      fixed baseline digest.
//   4. WriteSectors() only APPENDS AN INTENT to the transaction journal and
//      returns a receipt describing what *would* be written.
//
// When A0 is accepted, only the body of WriteSectors() is replaced with the
// real Windows write call (still guarded by IsArmed()); the capability chain,
// the journal and the rollback contract stay unchanged. Until then this file
// is safe to compile into production builds: with the macro at 0 every call
// returns WritesDisabled.
// ---------------------------------------------------------------------------

#include "disk/WriteGuard.h"
#include "disk/WriteTransaction.h"

#include <cstdint>
#include <string>
#include <type_traits>

namespace phoenix {

enum class WriteReceiptStatus : uint8_t
{
    Planned = 0,        // intent recorded in the journal (no byte written)
    WritesDisabled,     // S5: compile-time kill switch active
    NotArmed,           // grant / transaction / baseline not satisfied
    RecordFailed,       // transaction rejected the intent
    MissingDigest       // before- or after-image digest absent
};

struct WriteReceipt
{
    WriteReceiptStatus status      = WriteReceiptStatus::NotArmed;
    WriteDenyReason    guardReason = WriteDenyReason::None;
    TxResult           txResult    = TxResult::Ok;
    uint64_t           offsetBytes = 0;
    uint64_t           lengthBytes = 0;

    bool IsPlanned() const { return status == WriteReceiptStatus::Planned; }

    const char* Detail() const
    {
        switch (status) {
        case WriteReceiptStatus::Planned:        return "intent recorded (no I/O performed)";
        case WriteReceiptStatus::WritesDisabled: return "device writes disabled until A0 gate passes";
        case WriteReceiptStatus::NotArmed:       return "writer not armed: grant/transaction/baseline invalid";
        case WriteReceiptStatus::RecordFailed:   return TxResultDetail(txResult);
        case WriteReceiptStatus::MissingDigest:  return "before/after image digest required";
        }
        return "unknown";
    }
};

class DeviceWriter
{
public:
    DeviceWriter() = delete;
    DeviceWriter(const DeviceWriter&) = delete;
    DeviceWriter& operator=(const DeviceWriter&) = delete;

    explicit DeviceWriter(WriteTransaction& tx) : m_tx(&tx) {}

    bool IsArmed() const
    {
        if (!PHOENIX_ENABLE_DEVICE_WRITES) return false;
        return m_tx != nullptr &&
               m_tx->HasValidGrant() &&
               m_tx->UsesDeviceOperation() &&
               m_tx->IsReadyToWrite();
    }

    WriteReceipt WriteSectors(uint64_t offsetBytes,
                              uint64_t lengthBytes,
                              const std::string& beforeDigest,
                              const std::string& afterDigest,
                              const std::string& note = std::string())
    {
        WriteReceipt rc;
        rc.offsetBytes = offsetBytes;
        rc.lengthBytes = lengthBytes;

        // S5 -- production default. Checked before anything else.
        if (!PHOENIX_ENABLE_DEVICE_WRITES) {
            rc.status      = WriteReceiptStatus::WritesDisabled;
            rc.guardReason = WriteDenyReason::PlatformNotSupported;
            return rc;
        }
        if (!IsArmed()) {
            rc.status      = WriteReceiptStatus::NotArmed;
            rc.guardReason = m_tx ? m_tx->GuardReason()
                                  : WriteDenyReason::TargetNotRegistered;
            return rc;
        }
        if (beforeDigest.empty() || afterDigest.empty()) {
            rc.status = WriteReceiptStatus::MissingDigest;
            return rc;
        }

        WriteIntent in;
        in.targetId     = m_tx->TargetId();
        in.op           = m_tx->Operation();
        in.offsetBytes  = offsetBytes;
        in.lengthBytes  = lengthBytes;
        in.beforeDigest = beforeDigest;
        in.afterDigest  = afterDigest;
        in.note         = note;

        const TxResult r = m_tx->Record(in);
        if (r != TxResult::Ok) {
            rc.status   = WriteReceiptStatus::RecordFailed;
            rc.txResult = r;
            return rc;
        }
        rc.status = WriteReceiptStatus::Planned;
        return rc;
    }

    const WriteTransaction& Transaction() const { return *m_tx; }

private:
    WriteTransaction* m_tx;   // never owns; always created from a WriteGrant
};

// Type-level proof: no caller can bypass the capability chain.
static_assert(!std::is_default_constructible<DeviceWriter>::value,
              "DeviceWriter must not be default-constructible");
static_assert(!std::is_copy_constructible<DeviceWriter>::value,
              "DeviceWriter must not be copyable");
static_assert(!std::is_default_constructible<WriteTransaction>::value,
              "WriteTransaction requires a WriteGrant");
static_assert(!std::is_constructible<WriteGrant, bool, WriteDenyReason,
                                     std::string, WriteOperation>::value,
              "WriteGrant must only be constructible by WriteGuard");

} // namespace phoenix
