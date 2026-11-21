#pragma once

// ---------------------------------------------------------------------------
// RefsCrypto -- A1-5a ReFS read-only integrity primitives.
//
// Pure C++17, header-only, zero Qt / zero Win32 / zero I/O.  These are the
// checksums a ReFS parser needs in order to *refuse* damaged metadata rather
// than silently returning recovered-looking zeros:
//
//   * CRC-32C (Castagnoli, reflected 0x82F63B78)  -- SUPB / CHKP self-checksum
//   * CRC-64/NVME (reflected 0x9A6C9329AC4BC9B5) -- page-reference digest
//   * SHA-256                                    -- SHA-256 volume page refs
//   * VBR ROR-1 + ADD self-checksum (RefsIsBootSectorOurs)
//   * FNV-1a 64                                  -- compact golden fingerprints
//
// Published references: ReFS Forensics Reference (forefst) "VBR",
// "Checksum Architecture", "Page References"; libfsrefs / refs_core.
// Standard check values: CRC32C("123456789") = 0xE3069283,
// CRC64/NVME("123456789") = 0xAE8B14860A799888.
//
// SHA-256 is reused from disk/BitLockerCrypto.h (already FIPS-180-4 verified
// by test_bitlocker_crypto.cpp) rather than duplicated.
// ---------------------------------------------------------------------------

#include "../../disk/BitLockerCrypto.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace phoenix {
namespace refs {

using Hash32 = std::array<uint8_t, 32>;

struct CrcTable32C
{
    uint32_t v[256];
    constexpr CrcTable32C() : v{}
    {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? ((c >> 1) ^ 0x82F63B78u) : (c >> 1);
            v[i] = c;
        }
    }
};

struct CrcTable64Nvme
{
    uint64_t v[256];
    constexpr CrcTable64Nvme() : v{}
    {
        for (uint32_t i = 0; i < 256; ++i) {
            uint64_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1ull) ? ((c >> 1) ^ 0x9A6C9329AC4BC9B5ull) : (c >> 1);
            v[i] = c;
        }
    }
};

inline constexpr CrcTable32C    kCrc32CTable{};
inline constexpr CrcTable64Nvme kCrc64NvmeTable{};

// Update/Finish are split deliberately: a chainable CRC must pass the RAW
// (non-xorout) state between chunks. Exposing that as a defaulted third
// argument would silently mis-chain every second call.
inline uint32_t Crc32CUpdate(uint32_t state, const uint8_t* data, size_t length)
{
    for (size_t i = 0; i < length; ++i)
        state = kCrc32CTable.v[(state ^ data[i]) & 0xFFu] ^ (state >> 8);
    return state;
}
inline uint32_t Crc32CFinish(uint32_t state) { return state ^ 0xFFFFFFFFu; }
inline uint32_t Crc32C(const uint8_t* data, size_t length)
{
    return Crc32CFinish(Crc32CUpdate(0xFFFFFFFFu, data, length));
}
inline uint32_t Crc32C(const std::vector<uint8_t>& data)
{
    return Crc32C(data.data(), data.size());
}

inline uint64_t Crc64NvmeUpdate(uint64_t state, const uint8_t* data, size_t length)
{
    for (size_t i = 0; i < length; ++i)
        state = kCrc64NvmeTable.v[(state ^ data[i]) & 0xFFull] ^ (state >> 8);
    return state;
}
inline uint64_t Crc64NvmeFinish(uint64_t state)
{
    return state ^ 0xFFFFFFFFFFFFFFFFull;
}
inline uint64_t Crc64Nvme(const uint8_t* data, size_t length)
{
    return Crc64NvmeFinish(Crc64NvmeUpdate(0xFFFFFFFFFFFFFFFFull, data, length));
}
inline uint64_t Crc64Nvme(const std::vector<uint8_t>& data)
{
    return Crc64Nvme(data.data(), data.size());
}

inline Hash32 Sha256Of(const uint8_t* data, size_t length)
{
    return bitlocker_crypto::Sha256(data, length);
}

inline Hash32 Sha256Of(const std::vector<uint8_t>& data)
{
    return bitlocker_crypto::Sha256(data);
}

// RefsIsBootSectorOurs: rotate-right-1 then add, over bytes 3..511, skipping
// the two checksum bytes at 0x16/0x17.  The stored value is little-endian u16.
inline uint16_t VbrChecksum(const uint8_t vbr[512])
{
    uint16_t c = 0;
    for (int i = 3; i < 512; ++i) {
        if (i == 0x16 || i == 0x17) continue;
        c = static_cast<uint16_t>((c >> 1) | (static_cast<uint16_t>(c << 15)));
        c = static_cast<uint16_t>(c + vbr[i]);
    }
    return c;
}

inline uint64_t Fnv1a64(const uint8_t* data, size_t length)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < length; ++i) {
        h ^= data[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

} // namespace refs
} // namespace phoenix
