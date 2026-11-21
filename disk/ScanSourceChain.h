// A1-4b-a -- configuration-driven, NON-GUI composition of read-only sources.
//
// This is the wiring layer that makes the isolated A1 components usable as a
// single scanning source without touching the GUI:
//
//   injected IVirtualDiskReader slots
//     -> VirtualDiskSource (RAW / VHD / VHDX / VMDK / QCOW2 / VDI container)
//     -> RaidProbe (mdadm metadata or conservative blind probe) + VirtualRaid
//     -> BitLockerVolume (read-only decrypt)
//     -> RefsImageFromReader (IRefsImage bridge for fs/refs/RefsVolume)
//
// Design constraints (deliberate):
//   * The caller injects byte sources by name. This header opens NO device,
//     NO file and NO handle, and has no Qt / Win32 / write method.
//   * Configuration is a strict line-oriented key=value text. Unknown keys,
//     duplicate keys and malformed values are hard errors: a silently ignored
//     typo in a recovery configuration is worse than a refusal.
//   * Secrets are never copied into the diagnostic report.
//   * A failed probe never degrades to "just scan the first disk"; the chain
//     refuses to open. Wrong geometry must not silently produce wrong bytes.
//
// Pure C++17, header-only.

#ifndef PHOENIX_DISK_SCAN_SOURCE_CHAIN_H
#define PHOENIX_DISK_SCAN_SOURCE_CHAIN_H

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "VirtualDiskSource.h"
#include "VirtualRaid.h"
#include "RaidProbe.h"
#include "BitLockerVolume.h"
#include "fs/refs/RefsVolume.h"

namespace phoenix {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

enum class ChainParseResult : uint8_t
{
    Ok,
    EmptyConfig,
    SyntaxError,
    UnknownKey,
    DuplicateKey,
    BadValue,
    MissingMembers
};

inline const char* ChainParseDetail(ChainParseResult r)
{
    switch (r) {
    case ChainParseResult::Ok:             return "ok";
    case ChainParseResult::EmptyConfig:    return "empty configuration";
    case ChainParseResult::SyntaxError:    return "syntax error";
    case ChainParseResult::UnknownKey:     return "unknown key";
    case ChainParseResult::DuplicateKey:   return "duplicate key";
    case ChainParseResult::BadValue:       return "invalid value";
    case ChainParseResult::MissingMembers: return "members= is required";
    }
    return "unknown";
}

enum class ChainContainer : uint8_t { None, Auto, Raw, Vhd, Vhdx, Vmdk, Qcow2, Vdi };
enum class ChainRaidMode : uint8_t { None, Auto, Configured };
enum class ChainUnlock : uint8_t { None, ClearKey, RecoveryPassword, Password, ExternalKey, Vmk };

struct ChainSpec
{
    std::string  name;
    std::vector<std::string> members;
    ChainContainer container = ChainContainer::Raw;
    ChainRaidMode  raidMode  = ChainRaidMode::None;
    RaidConfig     raidConfig;
    RaidProbeOptions probeOptions;
    ChainUnlock    unlock    = ChainUnlock::None;
    std::string    secret;            // NEVER included in Report()
    bool           refs      = false;
};

namespace chain_detail {

inline std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

inline std::string Lower(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::vector<std::string> Split(const std::string& s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t p = s.find(sep, start);
        out.push_back(Trim(s.substr(start, p == std::string::npos ? p : p - start)));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

inline bool ParseU64(const std::string& s, uint64_t& out, int base = 10)
{
    if (s.empty()) return false;
    try {
        size_t used = 0;
        const unsigned long long v = std::stoull(s, &used, base);
        if (used != s.size()) return false;
        out = static_cast<uint64_t>(v);
        return true;
    } catch (...) { return false; }
}

inline bool ParseBool(const std::string& s, bool& out)
{
    const std::string v = Lower(s);
    if (v == "1" || v == "true" || v == "yes" || v == "on")  { out = true;  return true; }
    if (v == "0" || v == "false" || v == "no" || v == "off") { out = false; return true; }
    return false;
}

inline int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool ParseHexBytes(const std::string& s, std::vector<uint8_t>& out)
{
    if (s.empty() || (s.size() % 2u) != 0u) return false;
    out.clear();
    out.reserve(s.size() / 2u);
    for (size_t i = 0; i < s.size(); i += 2) {
        const int a = HexDigit(s[i]), b = HexDigit(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(static_cast<uint8_t>((a << 4) | b));
    }
    return true;
}

inline bool ParseAnchor(const std::string& value, ProbeAnchor& out)
{
    const size_t colon = value.find(':');
    if (colon == std::string::npos) return false;
    return ParseU64(Trim(value.substr(0, colon)), out.offset, 16) &&
           ParseHexBytes(Trim(value.substr(colon + 1)), out.bytes) &&
           !out.bytes.empty();
}

inline bool ParseLevel(const std::string& s, RaidLevel& out)
{
    const std::string v = Lower(s);
    if (v == "jbod" || v == "span")  { out = RaidLevel::Jbod;  return true; }
    if (v == "0")  { out = RaidLevel::Raid0;  return true; }
    if (v == "1")  { out = RaidLevel::Raid1;  return true; }
    if (v == "4")  { out = RaidLevel::Raid4;  return true; }
    if (v == "5")  { out = RaidLevel::Raid5;  return true; }
    if (v == "6")  { out = RaidLevel::Raid6;  return true; }
    if (v == "10") { out = RaidLevel::Raid10; return true; }
    return false;
}

inline bool ParseLayout(const std::string& s, RaidLayout& out)
{
    const std::string v = Lower(s);
    if (v == "left-symmetric")    { out = RaidLayout::LeftSymmetric;   return true; }
    if (v == "left-asymmetric")   { out = RaidLayout::LeftAsymmetric;  return true; }
    if (v == "right-symmetric")   { out = RaidLayout::RightSymmetric;  return true; }
    if (v == "right-asymmetric")  { out = RaidLayout::RightAsymmetric; return true; }
    if (v == "parity-first")      { out = RaidLayout::ParityFirst;     return true; }
    if (v == "parity-last")       { out = RaidLayout::ParityLast;      return true; }
    return false;
}

inline bool ParseContainer(const std::string& s, ChainContainer& out)
{
    const std::string v = Lower(s);
    if (v == "none")  { out = ChainContainer::None;  return true; }
    if (v == "auto")  { out = ChainContainer::Auto;  return true; }
    if (v == "raw")   { out = ChainContainer::Raw;   return true; }
    if (v == "vhd")   { out = ChainContainer::Vhd;   return true; }
    if (v == "vhdx")  { out = ChainContainer::Vhdx;  return true; }
    if (v == "vmdk")  { out = ChainContainer::Vmdk;  return true; }
    if (v == "qcow2") { out = ChainContainer::Qcow2; return true; }
    if (v == "vdi")   { out = ChainContainer::Vdi;   return true; }
    return false;
}

inline bool ParseRaidMode(const std::string& s, RaidConfig& cfg,
                          ChainRaidMode& out)
{
    const std::string v = Lower(s);
    if (v == "none") { out = ChainRaidMode::None; return true; }
    if (v == "auto") { out = ChainRaidMode::Auto; return true; }
    RaidLevel l = RaidLevel::Raid5;
    if (!ParseLevel(v, l)) return false;
    cfg.level = l;
    out = ChainRaidMode::Configured;
    return true;
}

inline bool ParseUnlock(const std::string& s, ChainUnlock& out)
{
    const std::string v = Lower(s);
    if (v == "none")      { out = ChainUnlock::None;             return true; }
    if (v == "clear")     { out = ChainUnlock::ClearKey;         return true; }
    if (v == "recovery")  { out = ChainUnlock::RecoveryPassword; return true; }
    if (v == "password")  { out = ChainUnlock::Password;         return true; }
    if (v == "external")  { out = ChainUnlock::ExternalKey;      return true; }
    if (v == "vmk")       { out = ChainUnlock::Vmk;              return true; }
    return false;
}

} // namespace chain_detail

inline ChainParseResult ParseChainConfig(const std::string& text,
                                        ChainSpec& out, std::string& error)
{
    using namespace chain_detail;
    out = ChainSpec{};
    error.clear();
    std::istringstream in(text);
    std::string line;
    uint32_t lineNo = 0;
    bool any = false;
    std::vector<std::string> seen;

    while (std::getline(in, line)) {
        ++lineNo;
        const std::string s = Trim(line);
        if (s.empty() || s[0] == '#') continue;
        any = true;
        const size_t eq = s.find('=');
        if (eq == std::string::npos) {
            error = "line " + std::to_string(lineNo) + ": expected key=value";
            return ChainParseResult::SyntaxError;
        }
        const std::string key = Lower(Trim(s.substr(0, eq)));
        const std::string val = Trim(s.substr(eq + 1));
        if (key.empty()) {
            error = "line " + std::to_string(lineNo) + ": empty key";
            return ChainParseResult::SyntaxError;
        }
        if (key != "raid.probe_anchor") {
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                error = "line " + std::to_string(lineNo) + ": duplicate key '" + key + "'";
                return ChainParseResult::DuplicateKey;
            }
            seen.push_back(key);
        }
        const bool known =
            key == "name" || key == "members" || key == "container" ||
            key == "raid" || key == "raid.level" || key == "raid.layout" ||
            key == "raid.chunk" || key == "raid.data_offset" ||
            key == "raid.missing" || key == "raid.probe_stripes" ||
            key == "raid.probe_sectors" || key == "raid.probe_max_candidates" ||
            key == "raid.probe_max_permutations" || key == "raid.probe_use_metadata" ||
            key == "raid.probe_allow_blind" || key == "raid.probe_anchor" ||
            key == "raid.probe_chunks" || key == "unlock" || key == "secret" ||
            key == "refs";
        if (!known) {
            error = "line " + std::to_string(lineNo) + ": unknown key '" + key + "'";
            return ChainParseResult::UnknownKey;
        }

        bool ok = true;
        uint64_t num = 0;
        bool flag = false;
        if (key == "name") out.name = val;
        else if (key == "members") {
            out.members = Split(val, ';');
            for (const std::string& m : out.members) if (m.empty()) ok = false;
        }
        else if (key == "container") ok = ParseContainer(val, out.container);
        else if (key == "raid") ok = ParseRaidMode(val, out.raidConfig, out.raidMode);
        else if (key == "raid.level") ok = ParseLevel(val, out.raidConfig.level);
        else if (key == "raid.layout") ok = ParseLayout(val, out.raidConfig.layout);
        else if (key == "raid.chunk") {
            ok = ParseU64(val, num) && num >= 512u && num <= kRaidMaxChunk &&
                 (num % 512u) == 0u;
            if (ok) out.raidConfig.chunkBytes = static_cast<uint32_t>(num);
        }
        else if (key == "raid.data_offset") {
            ok = ParseU64(val, num) && (num % 512u) == 0u;
            if (ok) out.raidConfig.dataOffset = num;
        }
        else if (key == "raid.missing") {
            if (!val.empty()) {
                for (const std::string& p : Split(val, ',')) {
                    uint64_t idx = 0;
                    if (!ParseU64(p, idx) || idx >= kRaidMaxMembers) { ok = false; break; }
                    out.raidConfig.missing.push_back(static_cast<uint32_t>(idx));
                }
            }
        }
        else if (key == "raid.probe_stripes") {
            ok = ParseU64(val, num) && num > 0 && num <= 1024;
            if (ok) out.probeOptions.stripesToCheck = static_cast<uint32_t>(num);
        }
        else if (key == "raid.probe_sectors") {
            ok = ParseU64(val, num) && num > 0 && num <= 4096;
            if (ok) out.probeOptions.sectorsPerStripe = static_cast<uint32_t>(num);
        }
        else if (key == "raid.probe_max_candidates") {
            ok = ParseU64(val, num) && num > 0;
            if (ok) out.probeOptions.maxCandidates = num;
        }
        else if (key == "raid.probe_max_permutations") {
            ok = ParseU64(val, num) && num > 0;
            if (ok) out.probeOptions.maxPermutations = static_cast<uint32_t>(num);
        }
        else if (key == "raid.probe_use_metadata") {
            ok = ParseBool(val, flag);
            if (ok) out.probeOptions.useMetadata = flag;
        }
        else if (key == "raid.probe_allow_blind") {
            ok = ParseBool(val, flag);
            if (ok) out.probeOptions.allowBlind = flag;
        }
        else if (key == "raid.probe_anchor") {
            ProbeAnchor a;
            ok = ParseAnchor(val, a);
            if (ok) out.probeOptions.anchors.push_back(std::move(a));
        }
        else if (key == "raid.probe_chunks") {
            // P/Q are computed per sector and therefore CANNOT determine the
            // chunk size. The caller must supply the plausible chunk set;
            // volume-internal known-plaintext anchors then discriminate it.
            std::vector<uint32_t> chunks;
            for (const std::string& part : Split(val, ';')) {
                uint64_t v = 0;
                if (!ParseU64(part, v) || v < 512u || v > kRaidMaxChunk ||
                    (v % 512u) != 0u) { ok = false; break; }
                chunks.push_back(static_cast<uint32_t>(v));
            }
            if (ok && !chunks.empty()) out.probeOptions.chunkCandidates = std::move(chunks);
            else if (ok) ok = false;
        }
        else if (key == "unlock") ok = ParseUnlock(val, out.unlock);
        else if (key == "secret") out.secret = val;
        else if (key == "refs") { ok = ParseBool(val, flag); if (ok) out.refs = flag; }

        if (!ok) {
            error = "line " + std::to_string(lineNo) + ": invalid value for '" +
                    key + "'";
            return ChainParseResult::BadValue;
        }
    }
    if (!any) { error = "no configuration lines"; return ChainParseResult::EmptyConfig; }
    if (out.members.empty()) {
        error = "members= is required and must name at least one injected source";
        return ChainParseResult::MissingMembers;
    }
    if (out.raidMode == ChainRaidMode::Configured &&
        out.raidConfig.chunkBytes == 0u) {
        out.raidConfig.chunkBytes = 64u * 1024u;
    }
    return ChainParseResult::Ok;
}

// ---------------------------------------------------------------------------
// Adapters
// ---------------------------------------------------------------------------

// VirtualDiskSource exposes ReadSectors(), not IVirtualDiskReader::Read().
// This adapter keeps byte-level reads sector-aligned and zero-copy where
// possible, and never writes to the backing source.
class VirtualDiskSourceAdapter : public IVirtualDiskReader
{
public:
    explicit VirtualDiskSourceAdapter(const IVirtualDiskReader& src) : m_src(src) {}

    VirtualDiskOpenResult Open(ChainContainer expected)
    {
        m_ready = false;
        const VirtualDiskOpenResult r = m_src.Open();
        if (r != VirtualDiskOpenResult::Ok) return r;
        if (expected != ChainContainer::Auto && expected != ChainContainer::None) {
            const VirtualDiskFormat actual = m_src.Info().format;
            VirtualDiskFormat want = VirtualDiskFormat::Unknown;
            switch (expected) {
            case ChainContainer::Raw:   want = VirtualDiskFormat::Raw;        break;
            case ChainContainer::Vhd:   want = VirtualDiskFormat::VhdFixed;   break;
            case ChainContainer::Vhdx:  want = VirtualDiskFormat::Vhdx;       break;
            case ChainContainer::Vmdk:  want = VirtualDiskFormat::Vmdk;       break;
            case ChainContainer::Qcow2: want = VirtualDiskFormat::Qcow2;      break;
            case ChainContainer::Vdi:   want = VirtualDiskFormat::Vdi;        break;
            default: break;
            }
            if (expected == ChainContainer::Vhd &&
                actual == VirtualDiskFormat::VhdDynamic) want = VirtualDiskFormat::VhdDynamic;
            if (actual != want) return VirtualDiskOpenResult::UnsupportedFormat;
        }
        m_ready = true;
        return VirtualDiskOpenResult::Ok;
    }

    const VirtualDiskInfo& Info() const { return m_src.Info(); }

    uint64_t Size() const override
    {
        return m_ready ? m_src.SectorCount() * 512u : 0u;
    }

    bool Read(uint64_t offset, void* dst, size_t length) const override
    {
        if (!m_ready || dst == nullptr) return false;
        if (length == 0) return true;
        const uint64_t total = Size();
        if (offset > total || static_cast<uint64_t>(length) > total - offset) return false;
        const uint64_t first = offset / 512u;
        const uint64_t last  = (offset + length - 1u) / 512u;
        const uint64_t count = last - first + 1u;
        if (count > (std::numeric_limits<uint32_t>::max)()) return false;
        std::vector<uint8_t> buf(static_cast<size_t>(count * 512u));
        if (!m_src.ReadSectors(first, static_cast<uint32_t>(count), buf.data()))
            return false;
        std::memcpy(dst, buf.data() + static_cast<size_t>(offset - first * 512u), length);
        return true;
    }

private:
    VirtualDiskSource m_src;
    bool m_ready = false;
};

// fs/refs/RefsVolume consumes IRefsImage, not IVirtualDiskReader. This thin
// bridge lets a fully composed chain feed the ReFS parser directly.
class RefsImageFromReader : public refs::IRefsImage
{
public:
    explicit RefsImageFromReader(const IVirtualDiskReader* reader) : m_reader(reader) {}
    uint64_t Size() const override { return m_reader ? m_reader->Size() : 0u; }
    bool Read(uint64_t offset, void* dst, size_t length) const override
    {
        return m_reader != nullptr && m_reader->Read(offset, dst, length);
    }
private:
    const IVirtualDiskReader* m_reader = nullptr;
};

// ---------------------------------------------------------------------------
// Chain
// ---------------------------------------------------------------------------

enum class ChainOpenResult : uint8_t
{
    Ok,
    BadConfig,
    MissingSlot,
    ContainerOpenFailed,
    ProbeFailed,
    RaidOpenFailed,
    UnlockOpenFailed,
    UnlockFailed,
    UnsupportedUnlock,
    NoHead
};

inline const char* ChainOpenDetail(ChainOpenResult r)
{
    switch (r) {
    case ChainOpenResult::Ok:                   return "ok";
    case ChainOpenResult::BadConfig:            return "invalid chain configuration";
    case ChainOpenResult::MissingSlot:          return "a named member was not injected";
    case ChainOpenResult::ContainerOpenFailed:  return "container image could not be opened";
    case ChainOpenResult::ProbeFailed:          return "RAID layout probe refused to guess";
    case ChainOpenResult::RaidOpenFailed:       return "RAID assembly failed";
    case ChainOpenResult::UnlockOpenFailed:     return "BitLocker volume header rejected";
    case ChainOpenResult::UnlockFailed:         return "BitLocker unlock failed";
    case ChainOpenResult::UnsupportedUnlock:    return "unsupported unlock material";
    case ChainOpenResult::NoHead:               return "no final reader produced";
    }
    return "unknown";
}

class SourceChain
{
public:
    SourceChain() = default;
    SourceChain(const SourceChain&) = delete;
    SourceChain& operator=(const SourceChain&) = delete;

    ChainOpenResult Open(const ChainSpec& spec,
                         const std::map<std::string, const IVirtualDiskReader*>& slots)
    {
        Close();
        m_spec = spec;
        m_report.clear();
        if (spec.members.empty()) return Fail(ChainOpenResult::BadConfig, "no members");

        // ---- stage 1: injected slots, optionally unwrapped as containers ---
        std::vector<const IVirtualDiskReader*> base;
        base.reserve(spec.members.size());
        for (const std::string& name : spec.members) {
            const auto it = slots.find(name);
            if (it == slots.end() || it->second == nullptr)
                return Fail(ChainOpenResult::MissingSlot,
                            "injected source '" + name + "' is missing");
            if (spec.container == ChainContainer::None) {
                base.push_back(it->second);
                Note("slot " + name + ": raw injected reader, " +
                     std::to_string(it->second->Size()) + " bytes");
            } else {
                std::unique_ptr<VirtualDiskSourceAdapter> adapter(
                    new VirtualDiskSourceAdapter(*it->second));
                const VirtualDiskOpenResult r = adapter->Open(spec.container);
                if (r != VirtualDiskOpenResult::Ok)
                    return Fail(ChainOpenResult::ContainerOpenFailed,
                                "slot " + name + ": " + VirtualDiskOpenDetail(r));
                Note("slot " + name + ": " +
                     VirtualDiskFormatName(adapter->Info().format) + ", " +
                     std::to_string(adapter->Size()) + " bytes");
                base.push_back(adapter.get());
                m_containers.push_back(std::move(adapter));
            }
        }

        // ---- stage 2: RAID assembly / probe --------------------------------
        const IVirtualDiskReader* head = nullptr;
        if (spec.raidMode == ChainRaidMode::None) {
            if (base.size() != 1u)
                return Fail(ChainOpenResult::BadConfig,
                            "raid=none requires exactly one member");
            head = base[0];
            Note("raid: disabled (single source)");
        } else if (spec.raidMode == ChainRaidMode::Auto) {
            RaidProbe probe;
            m_probe = probe.Probe(base, spec.probeOptions);
            Note(std::string("probe: ") + RaidProbeStatusName(m_probe.status) +
                 " (" + m_probe.detail + ")");
            if (m_probe.status != RaidProbeStatus::Ok)
                return Fail(ChainOpenResult::ProbeFailed, m_probe.detail);
            if (!Assemble(base, m_probe.config, m_probe.order))
                return m_lastResult;
            head = m_raid.get();
            Note(std::string("raid: ") + RaidLevelName(m_probe.config.level) +
                 " / " + RaidLayoutName(m_probe.config.layout) + ", " +
                 std::to_string(m_probe.config.chunkBytes) + "-byte chunk, " +
                 std::to_string(m_probe.stripesVerified) + " stripes verified");
        } else {
            std::vector<uint32_t> order(base.size());
            for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
            if (!Assemble(base, spec.raidConfig, order)) return m_lastResult;
            head = m_raid.get();
            Note(std::string("raid: configured ") +
                 RaidLevelName(spec.raidConfig.level));
        }

        // ---- stage 3: BitLocker read-only unlock ---------------------------
        if (spec.unlock != ChainUnlock::None) {
            m_bitlocker.reset(new BitLockerVolume(*head));
            const BitLockerOpenResult opened = m_bitlocker->Open();
            if (opened != BitLockerOpenResult::Ok)
                return Fail(ChainOpenResult::UnlockOpenFailed,
                            std::string("BitLocker: ") +
                            bitlocker_detail::OpenResultName(opened) +
                            " (" + m_bitlocker->LastError() + ")");
            const BitLockerUnlockResult u = Unlock(spec);
            if (u == BitLockerUnlockResult::Ok) {
                head = m_bitlocker.get();
                Note("bitlocker: unlocked (credential redacted)");
            } else {
                // A partial chain would hand encrypted bytes to a filesystem
                // parser. Refuse instead.
                const std::string why = m_bitlocker->LastError();
                return Fail(ChainOpenResult::UnlockFailed,
                            std::string("BitLocker: ") +
                            bitlocker_detail::UnlockResultName(u) +
                            " (" + why + ")");
            }
        }

        // ---- stage 4: ReFS image bridge -----------------------------------
        if (spec.refs) {
            m_refsImage.reset(new RefsImageFromReader(head));
            Note("refs: IRefsImage bridge attached (" +
                 std::to_string(m_refsImage->Size()) + " bytes)");
        }
        m_head = head;
        m_open = (m_head != nullptr);
        if (!m_open) return Fail(ChainOpenResult::NoHead, "empty head");
        m_lastResult = ChainOpenResult::Ok;
        Note("head: " + std::to_string(m_head->Size()) + " bytes, read-only");
        return ChainOpenResult::Ok;
    }

    void Close()
    {
        m_head = nullptr;
        m_refsImage.reset();
        m_bitlocker.reset();
        m_raid.reset();
        m_ordered.clear();
        m_containers.clear();
        m_probe = RaidProbeResult{};
        m_open = false;
    }

    bool IsOpen() const { return m_open; }
    const IVirtualDiskReader* Head() const { return m_open ? m_head : nullptr; }
    const refs::IRefsImage* RefsImage() const { return m_refsImage.get(); }
    const RaidProbeResult& ProbeResult() const { return m_probe; }
    ChainOpenResult LastResult() const { return m_lastResult; }
    const std::string& LastError() const { return m_lastError; }
    const std::vector<std::string>& Report() const { return m_report; }
    const ChainSpec& Spec() const { return m_spec; }

    // Human-readable configuration echo with every secret redacted.
    std::string Describe() const
    {
        std::string s = "chain '" + m_spec.name + "': " +
                        std::to_string(m_spec.members.size()) + " member(s)";
        if (m_spec.unlock != ChainUnlock::None) s += ", unlock=***redacted***";
        if (m_spec.refs) s += ", refs bridge";
        return s;
    }

private:
    void Note(const std::string& line) { m_report.push_back(line); }

    ChainOpenResult Fail(ChainOpenResult r, const std::string& why)
    {
        m_lastResult = r;
        m_lastError  = why;
        m_open       = false;
        m_head       = nullptr;
        Note(std::string("ERROR: ") + ChainOpenDetail(r) + " -- " + why);
        return r;
    }

    bool Assemble(const std::vector<const IVirtualDiskReader*>& base,
                  const RaidConfig& cfg, const std::vector<uint32_t>& order)
    {
        m_ordered.assign(cfg.members > 0u ? cfg.members : order.size(), nullptr);
        if (order.size() > m_ordered.size()) m_ordered.resize(order.size());
        for (size_t p = 0; p < order.size(); ++p) {
            if (order[p] == kMdRoleSpare) continue;
            if (order[p] >= base.size()) {
                Fail(ChainOpenResult::RaidOpenFailed, "probe returned an invalid slot index");
                return false;
            }
            m_ordered[p] = base[order[p]];
        }
        m_raid.reset(new VirtualRaid());
        const RaidOpenResult r = m_raid->Open(cfg, m_ordered);
        if (r != RaidOpenResult::Ok) {
            Fail(ChainOpenResult::RaidOpenFailed,
                 std::string("VirtualRaid: ") + RaidOpenDetail(r));
            return false;
        }
        return true;
    }

    BitLockerUnlockResult Unlock(const ChainSpec& spec)
    {
        switch (spec.unlock) {
        case ChainUnlock::ClearKey:
            return m_bitlocker->UnlockWithClearKey();
        case ChainUnlock::RecoveryPassword:
            return m_bitlocker->UnlockWithRecoveryPassword(spec.secret);
        case ChainUnlock::Password: {
            // Configuration text is UTF-8/ASCII; widen byte-wise. Non-ASCII
            // passwords need a UTF-8 -> UTF-16 conversion supplied by caller.
            std::wstring w(spec.secret.begin(), spec.secret.end());
            return m_bitlocker->UnlockWithPassword(w);
        }
        case ChainUnlock::ExternalKey:
        case ChainUnlock::Vmk: {
            std::vector<uint8_t> key;
            if (!chain_detail::ParseHexBytes(spec.secret, key) || key.size() != 32u) {
                m_lastError = "external key / VMK must be 32 bytes as 64 hex characters";
                return BitLockerUnlockResult::BadKeyMaterial;
            }
            return spec.unlock == ChainUnlock::ExternalKey
                 ? m_bitlocker->UnlockWithExternalKey(key.data())
                 : m_bitlocker->UnlockWithVmk(key.data());
        }
        default:
            return BitLockerUnlockResult::BadKeyMaterial;
        }
    }

    ChainSpec        m_spec;
    std::vector<std::unique_ptr<VirtualDiskSourceAdapter>> m_containers;
    std::unique_ptr<VirtualRaid>       m_raid;
    std::unique_ptr<BitLockerVolume>   m_bitlocker;
    std::unique_ptr<RefsImageFromReader> m_refsImage;
    std::vector<const IVirtualDiskReader*> m_ordered;
    const IVirtualDiskReader* m_head   = nullptr;
    RaidProbeResult  m_probe;
    ChainOpenResult  m_lastResult = ChainOpenResult::Ok;
    std::string      m_lastError;
    std::vector<std::string> m_report;
    bool             m_open = false;
};

} // namespace phoenix

#endif // PHOENIX_DISK_SCAN_SOURCE_CHAIN_H
