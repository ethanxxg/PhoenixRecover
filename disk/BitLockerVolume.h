#pragma once

// ---------------------------------------------------------------------------
// BitLockerVolume -- A1-3 read-only BitLocker unlock layer.
//
// Implements phoenix::IVirtualDiskReader, so a decrypted BitLocker volume can
// be plugged anywhere a raw/virtual image is accepted (VirtualRaid members,
// VirtualDiskSource-style consumers, future scan-source wiring). It holds a
// `const IVirtualDiskReader*` only: no device handle, no Qt, no Win32, no
// write method, no file creation. This is a strictly read-only decryption
// layer; it can never modify a source volume.
//
// Supported (as of A1-3a):
//   * Windows 7 / 8 / 10 / 11 fixed-disk BitLocker volumes (FVE block header v2)
//   * ciphers 0x8004 AES-128-XTS, 0x8005 AES-256-XTS,
//             0x8002 AES-128-CBC (no diffuser), 0x8003 AES-256-CBC (no diffuser)
//   * VMK protectors: clear key, recovery password (48 digits), user password
//     (UTF-16LE), external/startup key (raw 32-byte BEK key material), plus a
//     raw-VMK escape hatch for forensic key material
//   * 3 redundant FVE metadata copies, best-copy selection by nonce counter,
//     volume-header relocation, metadata-area zero fill, partial-decryption
//     boundary (encrypted_volume_size)
//
// Explicitly rejected with a reason (never silently treated as a raw disk):
//   * Windows Vista (FVE block header v1 / boot EB 52 90) -- different layout
//   * BitLocker To Go hybrid volumes -- encrypted data lives in a separate file
//   * Elephant Diffuser ciphers 0x8000 / 0x8001 -- no implementation yet
//   * TPM / TPM+PIN protectors -- require platform TPM, reported but not unlocked
//   * non-BitLocker volumes, truncated/corrupt FVE metadata
//
// COVERAGE BOUNDARY: everything is validated against SYNTHETIC volumes built by
// an independent Python oracle (testdata/bitlocker-golden/). No real
// BitLocker-encrypted volume, TPM-protected volume, or 4Kn volume has been
// tested. Do not claim "real BitLocker recovery is supported" until A1-3b.
// See ROADMAP_RECOVERY.md A1-3.
// ---------------------------------------------------------------------------

#include "BitLockerCrypto.h"
#include "VirtualDiskSource.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace phoenix {

enum class BitLockerOpenResult
{
    Ok,
    TooSmall,
    ReadFailed,
    NotBitLocker,
    UnsupportedVariant,
    BadMetadata,
    NoUsableMetadata
};

enum class BitLockerUnlockResult
{
    Ok,
    NotOpen,
    UnsupportedCipher,
    NoMatchingProtector,
    WrongCredential,
    BadKeyMaterial,
    InternalError
};

enum class BitLockerVariant
{
    Unknown,
    WindowsVista,
    Windows7OrLater,
    BitLockerToGo
};

enum class BitLockerProtectorKind
{
    Unknown,
    ClearKey,
    Tpm,
    StartupKey,
    TpmAndPin,
    RecoveryPassword,
    Password
};

struct BitLockerProtector
{
    BitLockerProtectorKind kind = BitLockerProtectorKind::Unknown;
    uint16_t rawType = 0;
    uint8_t guid[16]{};
    std::string description;
    bool supportedByThisBuild = false;
};

struct BitLockerInfo
{
    BitLockerVariant variant = BitLockerVariant::Unknown;
    uint8_t volumeGuid[16]{};
    uint32_t cipherMethod = 0;
    std::string cipherName;
    uint64_t volumeSize = 0;
    uint64_t encryptedVolumeSize = 0;
    uint32_t bytesPerSector = 512;
    uint64_t volumeHeaderOffset = 0;
    uint64_t volumeHeaderSize = 0;
    uint64_t metadataOffsets[3] = { 0, 0, 0 };
    uint64_t metadataAreaSize = 0;
    uint64_t fvekKeyDataBytes = 0;
    uint32_t metadataRevision = 0;
    std::vector<BitLockerProtector> protectors;
    std::vector<std::string> warnings;
    std::string detail;
};

namespace bitlocker_detail {

using Key32  = std::array<uint8_t, 32>;
using Salt16 = std::array<uint8_t, 16>;

inline uint16_t Le16(const uint8_t* p)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
}
inline uint32_t Le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t Le64(const uint8_t* p)
{
    return static_cast<uint64_t>(Le32(p)) | (static_cast<uint64_t>(Le32(p + 4)) << 32);
}
inline uint64_t RoundUp(uint64_t v, uint64_t align)
{
    if (align == 0 || v > (std::numeric_limits<uint64_t>::max)() - align + 1) return v;
    return ((v + align - 1) / align) * align;
}

struct Entry
{
    uint16_t size = 0;
    uint16_t type = 0;
    uint16_t value = 0;
    uint16_t version = 0;
    const uint8_t* raw = nullptr;
    const uint8_t* data = nullptr;
    size_t dataSize = 0;
};

inline bool ReadEntry(const uint8_t* p, size_t remain, Entry& e)
{
    if (p == nullptr || remain < 8) return false;
    e.size    = Le16(p);
    e.type    = Le16(p + 2);
    e.value   = Le16(p + 4);
    e.version = Le16(p + 6);
    if (e.size < 8 || static_cast<size_t>(e.size) > remain) return false;
    e.raw = p;
    e.data = p + 8;
    e.dataSize = static_cast<size_t>(e.size) - 8;
    return true;
}

inline std::string Utf16LeToAscii(const uint8_t* p, size_t bytes)
{
    std::string out;
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        const uint16_t c = Le16(p + i);
        if (c == 0) break;
        out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return out;
}

inline std::string GuidToString(const uint8_t g[16])
{
    static const char* hex = "0123456789abcdef";
    std::string s;
    const int orders[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) s.push_back('-');
        const uint8_t b = g[orders[i]];
        s.push_back(hex[b >> 4]);
        s.push_back(hex[b & 0x0f]);
    }
    return s;
}

struct VmkRecord
{
    BitLockerProtector protector;
    std::vector<std::vector<uint8_t>> ccmEntries;
    std::vector<Salt16> salts;
    std::vector<Key32> directKeys;
};

struct MetadataCandidate
{
    bool valid = false;
    uint32_t revision = 0;
    uint32_t cipher = 0;
    uint64_t encryptedSize = 0;
    uint64_t volumeHeaderOffset = 0;
    uint64_t volumeHeaderSize = 0;
    uint64_t offsets[3] = { 0, 0, 0 };
    uint64_t areaSize = 0;
    uint8_t guid[16]{};
    std::vector<BitLockerProtector> protectors;
    std::vector<VmkRecord> vmks;
    std::vector<uint8_t> fvekCcm;
};

inline const char* ProtectorName(uint16_t raw)
{
    switch (raw) {
    case 0x0000: return "clear key";
    case 0x0100: return "TPM";
    case 0x0200: return "startup / external key";
    case 0x0500: return "TPM + PIN";
    case 0x0800: return "recovery password";
    case 0x2000: return "password";
    default:     return "unknown";
    }
}

inline BitLockerProtectorKind ProtectorKind(uint16_t raw)
{
    switch (raw) {
    case 0x0000: return BitLockerProtectorKind::ClearKey;
    case 0x0100: return BitLockerProtectorKind::Tpm;
    case 0x0200: return BitLockerProtectorKind::StartupKey;
    case 0x0500: return BitLockerProtectorKind::TpmAndPin;
    case 0x0800: return BitLockerProtectorKind::RecoveryPassword;
    case 0x2000: return BitLockerProtectorKind::Password;
    default:     return BitLockerProtectorKind::Unknown;
    }
}

inline bool IsSupportedCipher(uint32_t method)
{
    return method == 0x8002 || method == 0x8003 ||
           method == 0x8004 || method == 0x8005;
}

inline const char* CipherName(uint32_t method)
{
    switch (method) {
    case 0x8000: return "AES-128-CBC + Elephant Diffuser (unsupported)";
    case 0x8001: return "AES-256-CBC + Elephant Diffuser (unsupported)";
    case 0x8002: return "AES-128-CBC (no diffuser)";
    case 0x8003: return "AES-256-CBC (no diffuser)";
    case 0x8004: return "AES-128-XTS";
    case 0x8005: return "AES-256-XTS";
    default:     return "unknown cipher";
    }
}

inline const char* OpenResultName(BitLockerOpenResult r)
{
    switch (r) {
    case BitLockerOpenResult::Ok:                 return "Ok";
    case BitLockerOpenResult::TooSmall:           return "TooSmall";
    case BitLockerOpenResult::ReadFailed:         return "ReadFailed";
    case BitLockerOpenResult::NotBitLocker:       return "NotBitLocker";
    case BitLockerOpenResult::UnsupportedVariant: return "UnsupportedVariant";
    case BitLockerOpenResult::BadMetadata:        return "BadMetadata";
    case BitLockerOpenResult::NoUsableMetadata:   return "NoUsableMetadata";
    }
    return "?";
}

inline const char* UnlockResultName(BitLockerUnlockResult r)
{
    switch (r) {
    case BitLockerUnlockResult::Ok:                   return "Ok";
    case BitLockerUnlockResult::NotOpen:              return "NotOpen";
    case BitLockerUnlockResult::UnsupportedCipher:    return "UnsupportedCipher";
    case BitLockerUnlockResult::NoMatchingProtector:  return "NoMatchingProtector";
    case BitLockerUnlockResult::WrongCredential:      return "WrongCredential";
    case BitLockerUnlockResult::BadKeyMaterial:       return "BadKeyMaterial";
    case BitLockerUnlockResult::InternalError:        return "InternalError";
    }
    return "?";
}

// Scan a nested property array for the pieces this build can use.
inline void CollectNested(const uint8_t* p, size_t bytes, VmkRecord& rec, int depth)
{
    if (p == nullptr || depth > 3 || bytes < 8) return;
    size_t pos = 0;
    while (pos + 8 <= bytes) {
        Entry e;
        if (!ReadEntry(p + pos, bytes - pos, e)) break;
        if (e.value == 0x0001 && e.dataSize >= 36) {
            Key32 k{};
            std::memcpy(k.data(), e.data + 4, 32);
            rec.directKeys.push_back(k);
        } else if (e.value == 0x0005 && e.dataSize >= 28) {
            rec.ccmEntries.emplace_back(e.raw, e.raw + e.size);
        } else if (e.value == 0x0003 && e.dataSize >= 20) {
            Salt16 salt{};
            std::memcpy(salt.data(), e.data + 4, 16);
            rec.salts.push_back(salt);
            CollectNested(e.data + 20, e.dataSize - 20, rec, depth + 1);
        } else if (e.value == 0x0009 && e.dataSize >= 24) {
            CollectNested(e.data + 24, e.dataSize - 24, rec, depth + 1);
        } else if (e.value == 0x0004 && e.dataSize >= 4) {
            CollectNested(e.data + 4, e.dataSize - 4, rec, depth + 1);
        } else if (e.value == 0x0002) {
            if (rec.protector.description.empty())
                rec.protector.description = Utf16LeToAscii(e.data, e.dataSize);
        }
        pos += e.size;
    }
}

} // namespace bitlocker_detail

class BitLockerVolume : public IVirtualDiskReader
{
public:
    explicit BitLockerVolume(const IVirtualDiskReader& reader) : m_reader(&reader) {}
    ~BitLockerVolume() override { ClearKeys(); }

    BitLockerVolume(const BitLockerVolume&) = delete;
    BitLockerVolume& operator=(const BitLockerVolume&) = delete;

    // ---- open / introspection --------------------------------------------
    BitLockerOpenResult Open()
    {
        ClearKeys();
        m_info = BitLockerInfo{};
        if (m_reader == nullptr) return Fail(BitLockerOpenResult::ReadFailed, "no source reader");

        const uint64_t sourceSize = m_reader->Size();
        if (sourceSize < 512)
            return Fail(BitLockerOpenResult::TooSmall, "source is smaller than one sector");

        uint8_t head[512] = {};
        if (!m_reader->Read(0, head, sizeof(head)))
            return Fail(BitLockerOpenResult::ReadFailed, "cannot read the first sector");

        uint64_t headerOffsets[3] = { 0, 0, 0 };
        if (std::memcmp(head + 3, "-FVE-FS-", 8) == 0) {
            if (head[0] == 0xEB && head[1] == 0x52 && head[2] == 0x90) {
                m_info.variant = BitLockerVariant::WindowsVista;
                return Fail(BitLockerOpenResult::UnsupportedVariant,
                            "Windows Vista BitLocker (FVE block header v1) is not implemented; "
                            "it uses a different volume header reconstruction");
            }
            m_info.variant = BitLockerVariant::Windows7OrLater;
            std::memcpy(m_info.volumeGuid, head + 160, 16);
            for (int i = 0; i < 3; ++i)
                headerOffsets[i] = bitlocker_detail::Le64(head + 176 + 8 * i);
            const uint16_t bps = bitlocker_detail::Le16(head + 11);
            if (bps != 512 && bps != 4096) {
                return Fail(BitLockerOpenResult::UnsupportedVariant,
                            "unsupported bytes-per-sector in FVE volume header");
            }
            m_info.bytesPerSector = bps;
        } else if (std::memcmp(head + 3, "MSWIN4.1", 8) == 0) {
            m_info.variant = BitLockerVariant::BitLockerToGo;
            return Fail(BitLockerOpenResult::UnsupportedVariant,
                        "BitLocker To Go hybrid volume: encrypted data lives in a separate "
                        "COV *.ER file, not in this FAT volume");
        } else {
            return Fail(BitLockerOpenResult::NotBitLocker,
                        "no -FVE-FS- / MSWIN4.1 signature at offset 3");
        }

        m_info.volumeSize = (sourceSize / m_info.bytesPerSector) * m_info.bytesPerSector;

        bitlocker_detail::MetadataCandidate best;
        int usable = 0;
        for (int i = 0; i < 3; ++i) {
            const uint64_t off = headerOffsets[i];
            if (off == 0 || off + 64 > sourceSize) {
                m_info.warnings.push_back("FVE metadata copy " + std::to_string(i + 1) +
                                          " has an out-of-range offset");
                continue;
            }
            bitlocker_detail::MetadataCandidate c;
            if (!ParseMetadataBlock(off, sourceSize, c)) {
                m_info.warnings.push_back("FVE metadata copy " + std::to_string(i + 1) +
                                          " is unreadable or corrupt");
                continue;
            }
            ++usable;
            if (!best.valid || c.revision > best.revision) best = std::move(c);
        }
        if (usable == 0)
            return Fail(BitLockerOpenResult::NoUsableMetadata,
                        "none of the three FVE metadata copies could be parsed");
        if (usable < 3)
            m_info.warnings.push_back("only " + std::to_string(usable) +
                                      " of 3 FVE metadata copies are readable");

        m_candidate = best;
        m_info.cipherMethod       = best.cipher;
        m_info.cipherName         = bitlocker_detail::CipherName(best.cipher);
        m_info.encryptedVolumeSize = best.encryptedSize;
        m_info.volumeHeaderOffset = best.volumeHeaderOffset;
        m_info.volumeHeaderSize   = best.volumeHeaderSize;
        m_info.metadataAreaSize   = m_metadataAreaOverride != 0
                                  ? m_metadataAreaOverride : best.areaSize;
        m_info.metadataRevision   = best.revision;
        m_info.protectors         = best.protectors;
        std::memcpy(m_info.volumeGuid, best.guid, 16);
        for (int i = 0; i < 3; ++i) m_info.metadataOffsets[i] = best.offsets[i];

        if (m_info.volumeHeaderSize == 0 || m_info.volumeHeaderOffset == 0)
            m_info.warnings.push_back("volume header relocation is absent; "
                                     "first sectors will be read in place");
        if (!bitlocker_detail::IsSupportedCipher(m_info.cipherMethod))
            m_info.warnings.push_back(std::string("cipher ") + m_info.cipherName +
                                      " is recognised but not decryptable by this build");

        m_open = true;
        m_info.detail = "BitLocker " + m_info.cipherName + ", " +
                        std::to_string(m_info.protectors.size()) + " protector(s), metadata rev " +
                        std::to_string(m_info.metadataRevision);
        return BitLockerOpenResult::Ok;
    }

    bool IsOpen() const { return m_open; }
    bool IsUnlocked() const { return m_unlocked; }
    const BitLockerInfo& Info() const { return m_info; }
    const std::string& LastError() const { return m_lastError; }

    // Override the derived FVE metadata area size (in bytes). Real volumes may
    // reserve a larger area than metadata_size suggests. Defaults to
    // RoundUp(metadata_size, bytes_per_sector).
    void SetMetadataAreaSize(uint64_t bytes) { m_metadataAreaOverride = bytes; }

    // ---- credential handling ---------------------------------------------
    BitLockerUnlockResult UnlockWithClearKey()
    {
        std::vector<bitlocker_detail::Key32> candidates;
        for (const auto& vmk : m_candidate.vmks)
            if (vmk.protector.kind == BitLockerProtectorKind::ClearKey)
                candidates.insert(candidates.end(), vmk.directKeys.begin(), vmk.directKeys.end());
        if (candidates.empty())
            return UnlockFail(BitLockerUnlockResult::NoMatchingProtector,
                              "no clear-key VMK protector found");
        return TryKeyEncryptionKeys(candidates);
    }

    BitLockerUnlockResult UnlockWithRecoveryPassword(const std::string& text)
    {
        bitlocker_crypto::Hash32 initialLE{};
        bitlocker_crypto::Hash32 initialBE{};
        if (!RecoveryPasswordInitial(text, false, initialLE))
            return UnlockFail(BitLockerUnlockResult::BadKeyMaterial,
                              "recovery password must be 8 groups of digits, each divisible by 11");
        (void)RecoveryPasswordInitial(text, true, initialBE);
        return TryStretchedCredentials({ initialLE, initialBE });
    }

    BitLockerUnlockResult UnlockWithPassword(const std::wstring& text)
    {
        std::vector<bitlocker_crypto::Hash32> initials;
        initials.push_back(PasswordInitial(text, false));
        initials.push_back(PasswordInitial(text, true));
        return TryStretchedCredentials(initials);
    }

    // Raw 32-byte key material from a .BEK external key / startup key file.
    BitLockerUnlockResult UnlockWithExternalKey(const uint8_t key[32])
    {
        if (key == nullptr) return UnlockFail(BitLockerUnlockResult::BadKeyMaterial, "null key");
        std::vector<bitlocker_crypto::Hash32> initials;
        bitlocker_crypto::Hash32 asHash{};
        std::memcpy(asHash.data(), key, 32);
        initials.push_back(asHash);
        const bitlocker_crypto::Hash32 hashed = bitlocker_crypto::Sha256(key, 32);
        initials.push_back(hashed);

        auto r = TryStretchedCredentials(initials);
        if (r == BitLockerUnlockResult::Ok) return r;
        bitlocker_detail::Key32 raw{};
        std::memcpy(raw.data(), key, 32);
        return TryKeyEncryptionKeys({ raw });
    }

    // Forensic escape hatch: a VMK already recovered by another tool.
    BitLockerUnlockResult UnlockWithVmk(const uint8_t vmk[32])
    {
        if (!PrepareForUnlock()) return m_lastUnlockResult;
        if (vmk == nullptr) return UnlockFail(BitLockerUnlockResult::BadKeyMaterial, "null VMK");
        if (TryFvek(vmk))
            return UnlockOk();
        return UnlockFail(BitLockerUnlockResult::WrongCredential,
                          "the supplied VMK does not unwrap the FVEK (CCM tag mismatch)");
    }

    void ClearKeys()
    {
        bitlocker_crypto::SecureZero(m_dataKeyStorage.data(), m_dataKeyStorage.size());
        bitlocker_crypto::SecureZero(m_tweakKeyStorage.data(), m_tweakKeyStorage.size());
        bitlocker_crypto::SecureZero(&m_dataKey, sizeof(m_dataKey));
        bitlocker_crypto::SecureZero(&m_tweakKey, sizeof(m_tweakKey));
        m_unlocked = false;
    }

    // ---- IVirtualDiskReader: decrypted, read-only sector access ------------
    uint64_t Size() const override { return m_info.volumeSize; }

    bool Read(uint64_t offset, void* dst, size_t length) const override
    {
        if (!m_unlocked || dst == nullptr) return false;
        if (length == 0) return true;
        const uint64_t volumeSize = m_info.volumeSize;
        if (offset > volumeSize || static_cast<uint64_t>(length) > volumeSize - offset)
            return false;

        const uint64_t ss = m_info.bytesPerSector;
        uint8_t* out = static_cast<uint8_t*>(dst);
        std::vector<uint8_t> sector(static_cast<size_t>(ss));
        size_t done = 0;
        while (done < length) {
            const uint64_t logical = offset + done;
            const uint64_t sectorBase = (logical / ss) * ss;
            const size_t skip = static_cast<size_t>(logical - sectorBase);
            const size_t take = (std::min)(static_cast<size_t>(ss) - skip, length - done);
            if (!ReadSector(sectorBase, sector.data())) return false;
            std::memcpy(out + done, sector.data() + skip, take);
            done += take;
        }
        return true;
    }

    // Re-read one decrypted sector without the zero/metadata special casing,
    // useful for forensic anchors. Returns false when not unlocked.
    bool ReadDecryptedSector(uint64_t logicalSectorOffset, uint8_t* dst) const
    {
        if (!m_unlocked || dst == nullptr) return false;
        if ((logicalSectorOffset % m_info.bytesPerSector) != 0) return false;
        return ReadSector(logicalSectorOffset, dst);
    }

private:
    BitLockerOpenResult Fail(BitLockerOpenResult r, const std::string& why)
    {
        m_open = false;
        m_lastError = why;
        m_info.detail = why;
        return r;
    }

    BitLockerUnlockResult UnlockFail(BitLockerUnlockResult r, const std::string& why)
    {
        m_lastError = why;
        m_lastUnlockResult = r;
        return r;
    }

    BitLockerUnlockResult UnlockOk()
    {
        m_lastUnlockResult = BitLockerUnlockResult::Ok;
        m_lastError.clear();
        return m_lastUnlockResult;
    }

    bool PrepareForUnlock()
    {
        if (!m_open) {
            UnlockFail(BitLockerUnlockResult::NotOpen, "volume is not open");
            return false;
        }
        if (!bitlocker_detail::IsSupportedCipher(m_info.cipherMethod)) {
            UnlockFail(BitLockerUnlockResult::UnsupportedCipher,
                       std::string("cipher ") + m_info.cipherName +
                       " is not implemented; refusing to return wrong plaintext");
            return false;
        }
        if (m_candidate.fvekCcm.empty()) {
            UnlockFail(BitLockerUnlockResult::BadKeyMaterial,
                       "FVE metadata contains no FVEK (AES-CCM) entry");
            return false;
        }
        return true;
    }

    static bool RecoveryPasswordInitial(const std::string& text,
                                        bool bigEndian,
                                        bitlocker_crypto::Hash32& out)
    {
        std::vector<std::string> groups;
        std::string cur;
        for (char c : text) {
            if (c == '-' || c == ' ') {
                if (!cur.empty()) { groups.push_back(cur); cur.clear(); }
            } else if (c >= '0' && c <= '9') {
                cur.push_back(c);
            } else {
                return false;
            }
        }
        if (!cur.empty()) groups.push_back(cur);
        if (groups.size() != 8) return false;

        uint8_t binary[16] = {};
        for (size_t i = 0; i < 8; ++i) {
            if (groups[i].empty() || groups[i].size() > 7) return false;
            uint64_t v = 0;
            for (char c : groups[i]) {
                v = v * 10 + static_cast<uint64_t>(c - '0');
                if (v > 0xFFFFFFFFull) return false;
            }
            if ((v % 11) != 0) return false;
            v /= 11;
            if (v > 0xFFFFull) return false;
            if (bigEndian) {
                binary[i * 2]     = static_cast<uint8_t>(v >> 8);
                binary[i * 2 + 1] = static_cast<uint8_t>(v);
            } else {
                binary[i * 2]     = static_cast<uint8_t>(v);
                binary[i * 2 + 1] = static_cast<uint8_t>(v >> 8);
            }
        }
        out = bitlocker_crypto::Sha256(binary, sizeof(binary));
        return true;
    }

    static bitlocker_crypto::Hash32 PasswordInitial(const std::wstring& text, bool withNul)
    {
        std::vector<uint8_t> utf16;
        utf16.reserve(text.size() * 2 + 2);
        for (wchar_t ch : text) {
            const uint32_t cp = static_cast<uint32_t>(ch);
            if (cp <= 0xFFFFu) {
                utf16.push_back(static_cast<uint8_t>(cp));
                utf16.push_back(static_cast<uint8_t>(cp >> 8));
            } else {
                const uint32_t v = cp - 0x10000u;
                const uint16_t hi = static_cast<uint16_t>(0xD800u + (v >> 10));
                const uint16_t lo = static_cast<uint16_t>(0xDC00u + (v & 0x3FFu));
                for (uint16_t w : { hi, lo }) {
                    utf16.push_back(static_cast<uint8_t>(w));
                    utf16.push_back(static_cast<uint8_t>(w >> 8));
                }
            }
        }
        if (withNul) { utf16.push_back(0); utf16.push_back(0); }
        const auto first = bitlocker_crypto::Sha256(utf16);
        return bitlocker_crypto::Sha256(first.data(), first.size());
    }

    BitLockerUnlockResult TryStretchedCredentials(
        const std::vector<bitlocker_crypto::Hash32>& initials)
    {
        if (!PrepareForUnlock()) return m_lastUnlockResult;
        std::vector<bitlocker_detail::Salt16> salts;
        for (const auto& vmk : m_candidate.vmks)
            salts.insert(salts.end(), vmk.salts.begin(), vmk.salts.end());
        if (salts.empty())
            return UnlockFail(BitLockerUnlockResult::NoMatchingProtector,
                              "no stretch-key VMK protector found for this credential");

        // Derive lazily: the canonical variant succeeds on the first attempt,
        // so a correct credential costs one 0x100000-iteration stretch rather
        // than one per compatibility variant.
        size_t attempts = 0;
        for (const auto& initial : initials)
            for (const auto& salt : salts) {
                const auto stretched = bitlocker_crypto::StretchKey(initial, salt.data());
                if (TrySingleKek(stretched.data(), attempts)) return UnlockOk();
            }
        return UnlockFail(BitLockerUnlockResult::WrongCredential,
                          "no VMK could be unwrapped (" + std::to_string(attempts) +
                          " AES-CCM attempt(s)); wrong credential, damaged metadata, "
                          "or an unsupported protector");
    }

    bool TrySingleKek(const uint8_t kek[32], size_t& attempts)
    {
        for (const auto& vmk : m_candidate.vmks) {
            for (const auto& ccm : vmk.ccmEntries) {
                ++attempts;
                std::vector<uint8_t> plain;
                if (!UnwrapCcm(ccm, kek, plain)) continue;
                if (plain.size() < 44) continue;   // 12-byte header + 32-byte VMK
                const bool ok = TryFvek(plain.data() + 12);
                bitlocker_crypto::SecureZero(plain.data(), plain.size());
                if (ok) return true;
            }
        }
        return false;
    }

    BitLockerUnlockResult TryKeyEncryptionKeys(
        const std::vector<bitlocker_detail::Key32>& keks)
    {
        if (!PrepareForUnlock()) return m_lastUnlockResult;
        if (m_candidate.vmks.empty())
            return UnlockFail(BitLockerUnlockResult::NoMatchingProtector,
                              "FVE metadata contains no VMK protector");
        size_t attempts = 0;
        for (const auto& kek : keks)
            if (TrySingleKek(kek.data(), attempts)) return UnlockOk();
        return UnlockFail(BitLockerUnlockResult::WrongCredential,
                          "no VMK could be unwrapped (" + std::to_string(attempts) +
                          " AES-CCM attempt(s)); wrong credential, damaged metadata, "
                          "or an unsupported protector");
    }

    static bool UnwrapCcm(const std::vector<uint8_t>& entry,
                          const uint8_t kek[32], std::vector<uint8_t>& plain)
    {
        // datum_aes_ccm_t: 8-byte entry header + 12-byte nonce + 16-byte tag,
        // then ciphertext. See dislocker datums.h / libbde BDE spec 5.x.
        if (entry.size() < 36 + 12) return false;
        bitlocker_crypto::Aes aes;
        if (!aes.SetKey(kek, 32)) return false;
        const size_t n = entry.size() - 36;
        plain.assign(n, 0);
        return bitlocker_crypto::CcmDecrypt(aes, entry.data() + 8, entry.data() + 20,
                                            entry.data() + 36, n, plain.data());
    }

    bool TryFvek(const uint8_t vmk[32])
    {
        std::vector<uint8_t> plain;
        struct ZeroOnExit
        {
            std::vector<uint8_t>& v;
            ~ZeroOnExit() { bitlocker_crypto::SecureZero(v.data(), v.size()); }
        } guard{ plain };
        if (!UnwrapCcm(m_candidate.fvekCcm, vmk, plain)) return false;
        if (plain.size() < 12) return false;
        const uint8_t* key = plain.data() + 12;
        const size_t keyBytes = plain.size() - 12;

        const uint32_t method = m_info.cipherMethod;
        size_t dataLen = 0, tweakLen = 0;
        if (method == 0x8004) { dataLen = 16; tweakLen = 16; }
        else if (method == 0x8005) { dataLen = 32; tweakLen = 32; }
        else if (method == 0x8002) { dataLen = 16; }
        else if (method == 0x8003) { dataLen = 32; }
        else return false;
        if (keyBytes < dataLen + tweakLen) return false;

        bitlocker_crypto::Aes data, tweak;
        if (!data.SetKey(key, dataLen)) return false;
        if (tweakLen != 0 && !tweak.SetKey(key + dataLen, tweakLen)) return false;

        m_dataKeyStorage.assign(key, key + dataLen);
        if (tweakLen != 0) m_tweakKeyStorage.assign(key + dataLen, key + dataLen + tweakLen);
        m_dataKey = data;
        m_tweakKey = tweak;
        m_info.fvekKeyDataBytes = keyBytes;
        m_unlocked = true;
        return true;
    }

    bool ParseMetadataBlock(uint64_t off, uint64_t sourceSize,
                            bitlocker_detail::MetadataCandidate& out)
    {
        uint8_t bh[64] = {};
        if (!m_reader->Read(off, bh, sizeof(bh))) return false;
        if (std::memcmp(bh, "-FVE-FS-", 8) != 0) return false;
        const uint16_t version = bitlocker_detail::Le16(bh + 10);
        if (version != 2) return false;   // v1 = Windows Vista, handled as unsupported

        out.encryptedSize       = bitlocker_detail::Le64(bh + 16);
        const uint32_t hdrSect  = bitlocker_detail::Le32(bh + 28);
        out.volumeHeaderOffset  = bitlocker_detail::Le64(bh + 56);
        out.volumeHeaderSize    = static_cast<uint64_t>(hdrSect) * m_info.bytesPerSector;
        for (int i = 0; i < 3; ++i)
            out.offsets[i] = bitlocker_detail::Le64(bh + 32 + 8 * i);

        uint8_t mh[48] = {};
        if (!m_reader->Read(off + 64, mh, sizeof(mh))) return false;
        const uint32_t metadataSize = bitlocker_detail::Le32(mh);
        const uint32_t headerSize   = bitlocker_detail::Le32(mh + 8);
        if (metadataSize < 48 || headerSize < 48 || metadataSize > (1u << 20)) return false;
        if (off + 64 + metadataSize > sourceSize) return false;
        std::memcpy(out.guid, mh + 16, 16);
        out.revision = bitlocker_detail::Le32(mh + 32);
        out.cipher   = bitlocker_detail::Le32(mh + 36) & 0xFFFFu;
        out.areaSize = bitlocker_detail::RoundUp(metadataSize, m_info.bytesPerSector);

        std::vector<uint8_t> entries(metadataSize - 48, 0);
        if (!entries.empty() &&
            !m_reader->Read(off + 64 + 48, entries.data(), entries.size())) return false;

        size_t pos = 0;
        while (pos + 8 <= entries.size()) {
            bitlocker_detail::Entry e;
            if (!bitlocker_detail::ReadEntry(entries.data() + pos,
                                             entries.size() - pos, e)) break;
            if (e.type == 0x000f && e.value == 0x000f && e.dataSize >= 16) {
                const uint64_t o = bitlocker_detail::Le64(e.data);
                const uint64_t n = bitlocker_detail::Le64(e.data + 8);
                if (o != 0 && n != 0 && n <= m_info.volumeSize) {
                    out.volumeHeaderOffset = o;
                    out.volumeHeaderSize   = n;
                }
            } else if (e.type == 0x0002 && e.value == 0x0008 && e.dataSize >= 28) {
                bitlocker_detail::VmkRecord rec;
                const uint16_t rawType = bitlocker_detail::Le16(e.data + 26);
                std::memcpy(rec.protector.guid, e.data, 16);
                rec.protector.rawType = rawType;
                rec.protector.kind = bitlocker_detail::ProtectorKind(rawType);
                rec.protector.supportedByThisBuild =
                    (rec.protector.kind == BitLockerProtectorKind::ClearKey ||
                     rec.protector.kind == BitLockerProtectorKind::StartupKey ||
                     rec.protector.kind == BitLockerProtectorKind::RecoveryPassword ||
                     rec.protector.kind == BitLockerProtectorKind::Password);
                bitlocker_detail::CollectNested(e.data + 28, e.dataSize - 28, rec, 0);
                out.protectors.push_back(rec.protector);
                out.vmks.push_back(std::move(rec));
            } else if (e.type == 0x0003 && e.value == 0x0005) {
                out.fvekCcm.assign(e.raw, e.raw + e.size);
            }
            pos += e.size;
        }
        out.valid = !out.fvekCcm.empty() && !out.vmks.empty();
        return out.valid;
    }

    bool IsZeroRegion(uint64_t logical) const
    {
        for (int i = 0; i < 3; ++i) {
            const uint64_t base = m_info.metadataOffsets[i];
            if (base != 0 && logical >= base &&
                logical - base < m_info.metadataAreaSize) return true;
        }
        const uint64_t h = m_info.volumeHeaderOffset;
        if (h != 0 && logical >= h && logical - h < m_info.volumeHeaderSize) return true;
        return false;
    }

    bool ReadSector(uint64_t logical, uint8_t* out) const
    {
        const uint64_t ss = m_info.bytesPerSector;
        if (IsZeroRegion(logical)) {
            std::memset(out, 0, static_cast<size_t>(ss));
            return true;
        }
        uint64_t physical = logical;
        if (m_info.volumeHeaderSize != 0 && logical < m_info.volumeHeaderSize) {
            if (m_info.volumeHeaderOffset >
                (std::numeric_limits<uint64_t>::max)() - logical) return false;
            physical = m_info.volumeHeaderOffset + logical;
        }
        if (!m_reader->Read(physical, out, static_cast<size_t>(ss))) return false;
        if (m_info.encryptedVolumeSize != 0 && physical >= m_info.encryptedVolumeSize)
            return true;

        const uint32_t method = m_info.cipherMethod;
        if (method == 0x8004 || method == 0x8005)
            return bitlocker_crypto::XtsDecryptSector(m_dataKey, m_tweakKey,
                                                      physical / ss, out, out,
                                                      static_cast<size_t>(ss));
        return bitlocker_crypto::CbcDecryptSector(m_dataKey, physical, out, out,
                                                  static_cast<size_t>(ss));
    }

    const IVirtualDiskReader* m_reader = nullptr;
    BitLockerInfo m_info;
    bitlocker_detail::MetadataCandidate m_candidate;
    std::string m_lastError;
    BitLockerUnlockResult m_lastUnlockResult = BitLockerUnlockResult::NotOpen;
    uint64_t m_metadataAreaOverride = 0;
    bool m_open = false;
    bool m_unlocked = false;

    std::vector<uint8_t> m_dataKeyStorage;   // kept only for secure zeroing
    std::vector<uint8_t> m_tweakKeyStorage;
    bitlocker_crypto::Aes m_dataKey;
    bitlocker_crypto::Aes m_tweakKey;
};

} // namespace phoenix
