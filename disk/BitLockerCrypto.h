#pragma once

// ---------------------------------------------------------------------------
// BitLockerCrypto -- A1-3 read-only BitLocker crypto primitives.
//
// Pure C++17, header-only, zero Qt / zero Win32 / zero I/O / zero allocation
// in the hot sector path. Everything here is DECRYPTION oriented: the product
// must never be able to re-encrypt or write a BitLocker volume through this
// layer. (AES encryption is exposed only because AES-CCM verification, the
// AES-CBC IV and the AES-XTS tweak all require the forward block cipher.)
//
// Primitives and their on-disk contract (verified against libbde's BDE format
// specification, libbde_password.c / libbde_sector_data.c and dislocker's
// decrypt.c / datums.h on 2026-11-07):
//
//   * AES-128/192/256 ECB + inverse cipher (FIPS 197)
//   * SHA-256 (FIPS 180-4) and the 0x100000-iteration stretch key loop
//   * AES-CCM with a 12-byte nonce, 16-byte tag, no AAD, L=3. dislocker uses
//     A_0 = [0x02 || nonce || 00 00 00] for S0, A_i = [0x02 || nonce || i]
//     for CTR (i starts at 1), and B0 = [0x3A || nonce || len_be24] for the
//     CBC-MAC. Stored tag T = CBC-MAC(plaintext) XOR S0.
//   * AES-XTS (IEEE 1619): T = E_k2(LE128(sector_number)); each 16-byte block
//     P = D_k1(C XOR T) XOR T, T *= alpha in GF(2^128) with 0x87 reduction.
//     BitLocker uses one full sector as the XTS data unit.
//   * AES-CBC without diffuser: IV = E_FVEK(LE128(physical byte offset)).
//
// Elephant Diffuser (methods 0x8000 / 0x8001) is intentionally NOT implemented
// here; BitLockerVolume rejects those methods explicitly instead of returning
// wrong plaintext. See ROADMAP_RECOVERY.md A1-3 coverage boundary.
// ---------------------------------------------------------------------------

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace phoenix {
namespace bitlocker_crypto {

using Block16 = std::array<uint8_t, 16>;
using Hash32  = std::array<uint8_t, 32>;

inline uint8_t GfMul(uint8_t a, uint8_t b)
{
    uint8_t r = 0;
    for (int i = 0; i < 8; ++i) {
        if ((b & 1u) != 0) r = static_cast<uint8_t>(r ^ a);
        const uint8_t hi = static_cast<uint8_t>(a & 0x80u);
        a = static_cast<uint8_t>(a << 1);
        if (hi != 0) a = static_cast<uint8_t>(a ^ 0x1bu);
        b = static_cast<uint8_t>(b >> 1);
    }
    return r;
}

inline uint8_t GfPow(uint8_t a, int e)
{
    uint8_t r = 1;
    while (e > 0) {
        if ((e & 1) != 0) r = GfMul(r, a);
        a = GfMul(a, a);
        e >>= 1;
    }
    return r;
}

// S-box derived from the GF(2^8) inverse + affine transform rather than from
// a pasted literal, so a transcription typo cannot silently corrupt AES.
inline std::array<uint8_t, 256> MakeSbox()
{
    std::array<uint8_t, 256> s{};
    for (int x = 0; x < 256; ++x) {
        const uint8_t inv = (x == 0) ? 0 : GfPow(static_cast<uint8_t>(x), 254);
        auto rol = [](uint8_t v, unsigned n) -> uint8_t {
            return static_cast<uint8_t>((v << n) | (v >> (8u - n)));
        };
        s[static_cast<size_t>(x)] = static_cast<uint8_t>(
            inv ^ rol(inv, 1) ^ rol(inv, 2) ^ rol(inv, 3) ^ rol(inv, 4) ^ 0x63u);
    }
    return s;
}

inline const std::array<uint8_t, 256>& Sbox()
{
    static const std::array<uint8_t, 256> table = MakeSbox();
    return table;
}

inline std::array<uint8_t, 256> MakeInverseSbox()
{
    std::array<uint8_t, 256> inv{};
    const auto& s = Sbox();
    for (int i = 0; i < 256; ++i) inv[s[static_cast<size_t>(i)]] = static_cast<uint8_t>(i);
    return inv;
}

inline const std::array<uint8_t, 256>& InverseSbox()
{
    static const std::array<uint8_t, 256> table = MakeInverseSbox();
    return table;
}

class Aes
{
public:
    bool SetKey(const uint8_t* key, size_t keyLength)
    {
        if (key == nullptr || (keyLength != 16 && keyLength != 24 && keyLength != 32)) return false;
        const size_t nk = keyLength / 4;
        m_rounds = static_cast<int>(nk + 6);
        const size_t words = 4 * (static_cast<size_t>(m_rounds) + 1);
        std::array<uint8_t, 240> w{};
        for (size_t i = 0; i < nk * 4; ++i) w[i] = key[i];

        uint8_t rcon = 1;
        const auto& sbox = Sbox();
        for (size_t i = nk; i < words; ++i) {
            uint8_t t[4] = { w[4 * (i - 1)], w[4 * (i - 1) + 1],
                             w[4 * (i - 1) + 2], w[4 * (i - 1) + 3] };
            if (i % nk == 0) {
                const uint8_t first = t[0];
                t[0] = static_cast<uint8_t>(sbox[t[1]] ^ rcon);
                t[1] = sbox[t[2]];
                t[2] = sbox[t[3]];
                t[3] = sbox[first];
                rcon = GfMul(rcon, 2);
            } else if (nk > 6 && i % nk == 4) {
                for (int j = 0; j < 4; ++j) t[j] = sbox[t[j]];
            }
            for (size_t j = 0; j < 4; ++j)
                w[4 * i + j] = static_cast<uint8_t>(w[4 * (i - nk) + j] ^ t[j]);
        }
        m_roundKey = w;
        m_valid = true;
        return true;
    }

    bool IsValid() const { return m_valid; }

    void EncryptBlock(const uint8_t in[16], uint8_t out[16]) const
    {
        uint8_t s[16];
        std::memcpy(s, in, 16);
        AddRoundKey(s, 0);
        for (int r = 1; r < m_rounds; ++r) {
            for (int i = 0; i < 16; ++i) s[i] = Sbox()[s[i]];
            ShiftRows(s);
            MixColumns(s);
            AddRoundKey(s, r);
        }
        for (int i = 0; i < 16; ++i) s[i] = Sbox()[s[i]];
        ShiftRows(s);
        AddRoundKey(s, m_rounds);
        std::memcpy(out, s, 16);
    }

    void DecryptBlock(const uint8_t in[16], uint8_t out[16]) const
    {
        uint8_t s[16];
        std::memcpy(s, in, 16);
        AddRoundKey(s, m_rounds);
        for (int r = m_rounds - 1; r >= 1; --r) {
            InvShiftRows(s);
            for (int i = 0; i < 16; ++i) s[i] = InverseSbox()[s[i]];
            AddRoundKey(s, r);
            InvMixColumns(s);
        }
        InvShiftRows(s);
        for (int i = 0; i < 16; ++i) s[i] = InverseSbox()[s[i]];
        AddRoundKey(s, 0);
        std::memcpy(out, s, 16);
    }

private:
    void AddRoundKey(uint8_t s[16], int round) const
    {
        const size_t base = 16u * static_cast<size_t>(round);
        for (int i = 0; i < 16; ++i)
            s[i] = static_cast<uint8_t>(s[i] ^ m_roundKey[base + static_cast<size_t>(i)]);
    }

    static void ShiftRows(uint8_t s[16])
    {
        uint8_t t[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                t[4 * c + r] = s[4 * ((c + r) % 4) + r];
        std::memcpy(s, t, 16);
    }

    static void InvShiftRows(uint8_t s[16])
    {
        uint8_t t[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                t[4 * c + r] = s[4 * ((c - r + 4) % 4) + r];
        std::memcpy(s, t, 16);
    }

    static void MixColumns(uint8_t s[16])
    {
        for (int c = 0; c < 4; ++c) {
            uint8_t* p = s + 4 * c;
            const uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
            p[0] = static_cast<uint8_t>(GfMul(a0, 2) ^ GfMul(a1, 3) ^ a2 ^ a3);
            p[1] = static_cast<uint8_t>(a0 ^ GfMul(a1, 2) ^ GfMul(a2, 3) ^ a3);
            p[2] = static_cast<uint8_t>(a0 ^ a1 ^ GfMul(a2, 2) ^ GfMul(a3, 3));
            p[3] = static_cast<uint8_t>(GfMul(a0, 3) ^ a1 ^ a2 ^ GfMul(a3, 2));
        }
    }

    static void InvMixColumns(uint8_t s[16])
    {
        for (int c = 0; c < 4; ++c) {
            uint8_t* p = s + 4 * c;
            const uint8_t a0 = p[0], a1 = p[1], a2 = p[2], a3 = p[3];
            p[0] = static_cast<uint8_t>(GfMul(a0, 14) ^ GfMul(a1, 11) ^ GfMul(a2, 13) ^ GfMul(a3, 9));
            p[1] = static_cast<uint8_t>(GfMul(a0, 9) ^ GfMul(a1, 14) ^ GfMul(a2, 11) ^ GfMul(a3, 13));
            p[2] = static_cast<uint8_t>(GfMul(a0, 13) ^ GfMul(a1, 9) ^ GfMul(a2, 14) ^ GfMul(a3, 11));
            p[3] = static_cast<uint8_t>(GfMul(a0, 11) ^ GfMul(a1, 13) ^ GfMul(a2, 9) ^ GfMul(a3, 14));
        }
    }

    std::array<uint8_t, 240> m_roundKey{};
    int  m_rounds = 10;
    bool m_valid  = false;
};

// ---------------------------------------------------------------------------
// SHA-256
// ---------------------------------------------------------------------------
inline Hash32 Sha256(const uint8_t* data, size_t length)
{
    static const uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
        0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
        0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
        0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };

    uint32_t h[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };

    const uint64_t bitLength = static_cast<uint64_t>(length) * 8u;
    // The 0x100000-iteration stretch loop hashes an 88-byte struct; avoid a
    // million heap allocations for that hot path.
    uint8_t stackBuf[256] = {};
    std::vector<uint8_t> heapBuf;
    uint8_t* msg = stackBuf;
    if (length + 72 > sizeof(stackBuf)) {
        heapBuf.assign(length + 72, 0);
        msg = heapBuf.data();
    }
    if (length > 0) std::memcpy(msg, data, length);
    msg[length] = 0x80;
    size_t padded = length + 1;
    while ((padded % 64) != 56) ++padded;
    for (int i = 0; i < 8; ++i)
        msg[padded + static_cast<size_t>(i)] =
            static_cast<uint8_t>(bitLength >> (56 - 8 * i));

    auto rotr = [](uint32_t v, unsigned n) -> uint32_t {
        return (v >> n) | (v << (32u - n));
    };

    for (size_t off = 0; off <= padded; off += 64) {
        const uint8_t* block = msg + off;
        uint32_t w[64] = {};
        for (int i = 0; i < 16; ++i) {
            const size_t p = static_cast<size_t>(i) * 4;
            w[i] = (static_cast<uint32_t>(block[p]) << 24) |
                   (static_cast<uint32_t>(block[p + 1]) << 16) |
                   (static_cast<uint32_t>(block[p + 2]) << 8) |
                   static_cast<uint32_t>(block[p + 3]);
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }

    Hash32 out{};
    for (int i = 0; i < 8; ++i) {
        out[static_cast<size_t>(i) * 4 + 0] = static_cast<uint8_t>(h[i] >> 24);
        out[static_cast<size_t>(i) * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        out[static_cast<size_t>(i) * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        out[static_cast<size_t>(i) * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
    return out;
}

inline Hash32 Sha256(const std::vector<uint8_t>& data)
{
    return Sha256(data.empty() ? reinterpret_cast<const uint8_t*>("") : data.data(), data.size());
}

// BitLocker stretch key: struct { last[32], initial[32], salt[16], count LE64 }
// has 0x100000 iterations. Equivalent to libbde_password_calculate_key(), which
// runs 0xFFFFF in-loop hashes plus one final hash.
inline Hash32 StretchKey(const Hash32& initial, const uint8_t salt[16])
{
    uint8_t state[88] = {};
    std::memcpy(state + 32, initial.data(), 32);
    std::memcpy(state + 64, salt, 16);
    Hash32 last{};
    for (uint32_t i = 0; i < 0x100000u; ++i) {
        state[80] = static_cast<uint8_t>(i);
        state[81] = static_cast<uint8_t>(i >> 8);
        state[82] = static_cast<uint8_t>(i >> 16);
        state[83] = static_cast<uint8_t>(i >> 24);
        state[84] = state[85] = state[86] = state[87] = 0;
        last = Sha256(state, sizeof(state));
        std::memcpy(state, last.data(), 32);
    }
    return last;
}

// ---------------------------------------------------------------------------
// AES-CCM (12-byte nonce, 16-byte tag, no AAD, L=3) -- detached tag layout
// ---------------------------------------------------------------------------
inline bool CcmDecrypt(const Aes& aes,
                       const uint8_t nonce[12], const uint8_t storedTag[16],
                       const uint8_t* cipher, size_t cipherLength, uint8_t* plain)
{
    if (cipher == nullptr || plain == nullptr || nonce == nullptr || storedTag == nullptr)
        return false;
    if (!aes.IsValid() || cipherLength > 0xFFFFFFu) return false;

    uint8_t a0[16] = { 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    std::memcpy(a0 + 1, nonce, 12);
    uint8_t s0[16];
    aes.EncryptBlock(a0, s0);

    uint8_t ctr[16];
    std::memcpy(ctr, a0, 16);
    ctr[15] = 1;
    for (size_t off = 0; off < cipherLength; off += 16) {
        uint8_t stream[16];
        aes.EncryptBlock(ctr, stream);
        const size_t n = (cipherLength - off < 16) ? (cipherLength - off) : 16;
        for (size_t i = 0; i < n; ++i)
            plain[off + i] = static_cast<uint8_t>(cipher[off + i] ^ stream[i]);
        for (int i = 15; i >= 13; --i) {
            ctr[i] = static_cast<uint8_t>(ctr[i] + 1);
            if (ctr[i] != 0) break;
        }
    }

    // B0: flags 0x3A (M=16 => M'=7, Adata=0, L'=2), nonce, 24-bit big-endian length.
    uint8_t b0[16] = { 0x3A, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    std::memcpy(b0 + 1, nonce, 12);
    const uint32_t l = static_cast<uint32_t>(cipherLength);
    b0[13] = static_cast<uint8_t>(l >> 16);
    b0[14] = static_cast<uint8_t>(l >> 8);
    b0[15] = static_cast<uint8_t>(l);

    uint8_t mac[16];
    aes.EncryptBlock(b0, mac);
    size_t off = 0;
    while (off + 16 <= cipherLength) {
        for (int i = 0; i < 16; ++i) mac[i] = static_cast<uint8_t>(mac[i] ^ plain[off + i]);
        aes.EncryptBlock(mac, mac);
        off += 16;
    }
    if (off < cipherLength) {
        for (size_t i = 0; i < cipherLength - off; ++i)
            mac[i] = static_cast<uint8_t>(mac[i] ^ plain[off + i]);
        aes.EncryptBlock(mac, mac);
    }

    uint8_t diff = 0;
    for (int i = 0; i < 16; ++i)
        diff = static_cast<uint8_t>(diff | (mac[i] ^ s0[i] ^ storedTag[i]));
    return diff == 0;
}

// ---------------------------------------------------------------------------
// Sector transforms
// ---------------------------------------------------------------------------
inline void PutLe128(uint8_t out[16], uint64_t value)
{
    std::memset(out, 0, 16);
    for (int i = 0; i < 8; ++i) out[i] = static_cast<uint8_t>(value >> (8 * i));
}

inline void GfMulAlpha(uint8_t t[16])
{
    uint8_t carry = 0;
    for (int i = 0; i < 16; ++i) {
        const uint8_t next = static_cast<uint8_t>(t[i] >> 7);
        t[i] = static_cast<uint8_t>((t[i] << 1) | carry);
        carry = next;
    }
    if (carry != 0) t[0] = static_cast<uint8_t>(t[0] ^ 0x87u);
}

// One XTS data unit = one full sector (512 or 4096 bytes, multiple of 16).
// in/out may alias. dataKeyLength is 16 or 32; tweakKey has the same length.
inline bool XtsDecryptSector(const Aes& dataKey, const Aes& tweakKey,
                             uint64_t sectorNumber,
                             const uint8_t* in, uint8_t* out, size_t sectorSize)
{
    if (!dataKey.IsValid() || !tweakKey.IsValid()) return false;
    if (sectorSize == 0 || (sectorSize % 16) != 0 || in == nullptr || out == nullptr) return false;

    uint8_t iv[16];
    PutLe128(iv, sectorNumber);
    uint8_t t[16];
    tweakKey.EncryptBlock(iv, t);

    for (size_t off = 0; off < sectorSize; off += 16) {
        uint8_t c[16];
        for (int i = 0; i < 16; ++i)
            c[i] = static_cast<uint8_t>(in[off + static_cast<size_t>(i)] ^ t[i]);
        uint8_t p[16];
        dataKey.DecryptBlock(c, p);
        for (int i = 0; i < 16; ++i)
            out[off + static_cast<size_t>(i)] = static_cast<uint8_t>(p[i] ^ t[i]);
        GfMulAlpha(t);
    }
    return true;
}

// AES-CBC without diffuser: IV = E_FVEK(LE128(physical byte offset)).
inline bool CbcDecryptSector(const Aes& key, uint64_t physicalByteOffset,
                             const uint8_t* in, uint8_t* out, size_t sectorSize)
{
    if (!key.IsValid() || sectorSize == 0 || (sectorSize % 16) != 0 ||
        in == nullptr || out == nullptr) return false;

    uint8_t raw[16];
    PutLe128(raw, physicalByteOffset);
    uint8_t prev[16];
    key.EncryptBlock(raw, prev);

    for (size_t off = 0; off < sectorSize; off += 16) {
        uint8_t c[16];
        std::memcpy(c, in + off, 16);
        uint8_t p[16];
        key.DecryptBlock(c, p);
        for (int i = 0; i < 16; ++i)
            out[off + static_cast<size_t>(i)] = static_cast<uint8_t>(p[i] ^ prev[i]);
        std::memcpy(prev, c, 16);
    }
    return true;
}

// Constant-ish zeroing helper for key material (no optimizer elision).
inline void SecureZero(void* p, size_t n)
{
    volatile uint8_t* q = static_cast<volatile uint8_t*>(p);
    while (n-- > 0) *q++ = 0;
}

} // namespace bitlocker_crypto
} // namespace phoenix
