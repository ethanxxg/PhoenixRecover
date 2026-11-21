// A1-3 BitLocker crypto primitives regression.
//
// Two independent verification layers:
//   1. Published FIPS 197 / FIPS 180-4 vectors hard-coded below (no oracle
//      dependency, so a broken generator cannot make the test pass).
//   2. testdata/bitlocker-golden/primitives.tsv + stretch.tsv produced by
//      testdata/tools/Gen-BitLockerGolden.py, whose AES implementation is pure
//      Python with a GF-derived S-box and whose SHA-256 is Python hashlib.
//
// Every positive vector also has a negative counterpart (tampered ciphertext
// or tag must NOT reproduce the plaintext / must NOT authenticate).

#include "../disk/BitLockerCrypto.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace phoenix::bitlocker_crypto;

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, what)                                                        \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            std::printf("FAIL line %d: %s\n", __LINE__, (what));                 \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static std::string Hex(const uint8_t* p, size_t n)
{
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 0x0f]);
    }
    return s;
}

static std::vector<uint8_t> FromHex(const std::string& h)
{
    std::vector<uint8_t> out;
    if ((h.size() & 1u) != 0) return out;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < h.size(); i += 2) {
        const int a = nibble(h[i]);
        const int b = nibble(h[i + 1]);
        if (a < 0 || b < 0) return {};
        out.push_back(static_cast<uint8_t>((a << 4) | b));
    }
    return out;
}

static std::vector<std::string> SplitTabs(const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    std::istringstream is(line);
    while (std::getline(is, cur, '\t')) out.push_back(cur);
    return out;
}

static std::vector<std::string> ReadLines(const std::string& path)
{
    std::vector<std::string> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] != '#') out.push_back(line);
    }
    return out;
}

static std::string ResolveGoldenDir()
{
    std::vector<std::string> candidates;
    if (const char* env = std::getenv("PHOENIX_BITLOCKER_GOLDEN_DIR")) {
        if (*env) candidates.emplace_back(env);
    }
    candidates.emplace_back("testdata/bitlocker-golden/");
    candidates.emplace_back("../testdata/bitlocker-golden/");
    candidates.emplace_back("../../testdata/bitlocker-golden/");
    candidates.emplace_back("../../../testdata/bitlocker-golden/");
    candidates.emplace_back("../../../../testdata/bitlocker-golden/");
    for (std::string d : candidates) {
        if (d.empty()) continue;
        if (d.back() != '/' && d.back() != '\\') d += '/';
        if (std::ifstream(d + "primitives.tsv").good()) return d;
    }
    return "testdata/bitlocker-golden/";
}

// ---------------------------------------------------------------------------
static void TestFipsAes()
{
    struct Vec { const char* key; const char* pt; const char* ct; };
    static const Vec vectors[] = {
        { "000102030405060708090a0b0c0d0e0f",
          "00112233445566778899aabbccddeeff", "69c4e0d86a7b0430d8cdb78070b4c55a" },
        { "000102030405060708090a0b0c0d0e0f1011121314151617",
          "00112233445566778899aabbccddeeff", "dda97ca4864cdfe06eaf70a0ec0d7191" },
        { "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
          "00112233445566778899aabbccddeeff", "8ea2b7ca516745bfeafc49904b496089" },
    };
    for (const auto& v : vectors) {
        const auto key = FromHex(v.key);
        const auto pt  = FromHex(v.pt);
        const auto ct  = FromHex(v.ct);
        Aes aes;
        CHECK(aes.SetKey(key.data(), key.size()), "AES SetKey");
        uint8_t enc[16] = {}, dec[16] = {};
        aes.EncryptBlock(pt.data(), enc);
        CHECK(Hex(enc, 16) == v.ct, "AES encrypt vs FIPS 197");
        aes.DecryptBlock(ct.data(), dec);
        CHECK(Hex(dec, 16) == v.pt, "AES inverse cipher vs FIPS 197");
    }
    Aes bad;
    uint8_t k[17] = {};
    CHECK(!bad.SetKey(k, 17), "AES rejects unsupported key length");
    CHECK(!bad.SetKey(nullptr, 16), "AES rejects null key");
}

static void TestFipsSha256()
{
    struct Vec { const char* text; const char* digest; };
    static const Vec vectors[] = {
        { "", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
        { "abc", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" },
        { "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1" },
    };
    for (const auto& v : vectors) {
        const auto h = Sha256(reinterpret_cast<const uint8_t*>(v.text), std::strlen(v.text));
        CHECK(Hex(h.data(), h.size()) == v.digest, "SHA-256 vs FIPS 180-4");
    }
    CHECK(GfMul(0x57, 0x83) == 0xc1, "GF(2^8) multiplication");
    CHECK(Sbox()[0] == 0x63 && Sbox()[0x53] == 0xed, "AES S-box derived from GF inverse");
    CHECK(InverseSbox()[Sbox()[0x7f]] == 0x7f, "AES inverse S-box is consistent");
}

static void TestPrimitiveVectors(const std::string& dir)
{
    const auto lines = ReadLines(dir + "primitives.tsv");
    CHECK(!lines.empty(), "primitives.tsv is present and readable");
    int xts = 0, cbc = 0, ccm = 0;
    for (const auto& line : lines) {
        const auto f = SplitTabs(line);
        if (f.size() < 6) { CHECK(false, "primitives.tsv column count"); continue; }
        const auto key    = FromHex(f[1]);
        const auto plain  = FromHex(f[3]);
        const auto cipher = FromHex(f[4]);
        CHECK(cipher.size() == plain.size(), "primitive ciphertext length");

        if (f[0] == "xts128" || f[0] == "xts256") {
            ++xts;
            const size_t half = key.size() / 2;
            Aes data, tweak;
            CHECK(data.SetKey(key.data(), half), "XTS data key");
            CHECK(tweak.SetKey(key.data() + half, half), "XTS tweak key");
            const uint64_t sector = std::strtoull(f[2].c_str(), nullptr, 10);
            std::vector<uint8_t> out(plain.size(), 0);
            CHECK(XtsDecryptSector(data, tweak, sector, cipher.data(), out.data(), out.size()),
                  "XtsDecryptSector succeeds");
            CHECK(out == plain, "AES-XTS decryption matches independent Python oracle");
            auto tampered = cipher;
            tampered[7] ^= 0x01;
            std::vector<uint8_t> bad(plain.size(), 0);
            (void)XtsDecryptSector(data, tweak, sector, tampered.data(), bad.data(), bad.size());
            CHECK(bad != plain, "tampered XTS ciphertext does not reproduce plaintext");
            // A wrong sector number must not reproduce the plaintext.
            std::vector<uint8_t> wrongTweak(plain.size(), 0);
            (void)XtsDecryptSector(data, tweak, sector + 1, cipher.data(),
                                   wrongTweak.data(), wrongTweak.size());
            CHECK(wrongTweak != plain, "wrong XTS sector number changes plaintext");
        } else if (f[0] == "cbc128" || f[0] == "cbc256") {
            ++cbc;
            Aes aes;
            CHECK(aes.SetKey(key.data(), key.size()), "CBC key");
            const uint64_t offset = std::strtoull(f[2].c_str(), nullptr, 10);
            std::vector<uint8_t> out(plain.size(), 0);
            CHECK(CbcDecryptSector(aes, offset, cipher.data(), out.data(), out.size()),
                  "CbcDecryptSector succeeds");
            CHECK(out == plain, "AES-CBC decryption matches independent Python oracle");
            std::vector<uint8_t> wrongOffset(plain.size(), 0);
            (void)CbcDecryptSector(aes, offset + 512, cipher.data(),
                                   wrongOffset.data(), wrongOffset.size());
            CHECK(wrongOffset != plain, "wrong CBC physical offset changes plaintext");
        } else if (f[0] == "ccm128" || f[0] == "ccm256") {
            ++ccm;
            Aes aes;
            CHECK(aes.SetKey(key.data(), key.size()), "CCM key");
            const auto nonce = FromHex(f[2]);
            const auto tag   = FromHex(f[5]);
            CHECK(nonce.size() == 12 && tag.size() == 16, "CCM nonce/tag sizes");
            std::vector<uint8_t> out(plain.size(), 0);
            CHECK(CcmDecrypt(aes, nonce.data(), tag.data(), cipher.data(),
                             cipher.size(), out.data()),
                  "AES-CCM tag verifies");
            CHECK(out == plain, "AES-CCM decryption matches independent Python oracle");
            auto badTag = tag;
            badTag[3] ^= 0x80;
            std::vector<uint8_t> out2(plain.size(), 0);
            CHECK(!CcmDecrypt(aes, nonce.data(), badTag.data(), cipher.data(),
                              cipher.size(), out2.data()),
                  "tampered CCM tag is rejected");
            auto badCipher = cipher;
            badCipher[2] ^= 0x01;
            std::vector<uint8_t> out3(plain.size(), 0);
            CHECK(!CcmDecrypt(aes, nonce.data(), tag.data(), badCipher.data(),
                              badCipher.size(), out3.data()),
                  "tampered CCM ciphertext is rejected");
            auto badNonce = nonce;
            badNonce[11] ^= 0x01;
            std::vector<uint8_t> out4(plain.size(), 0);
            CHECK(!CcmDecrypt(aes, badNonce.data(), tag.data(), cipher.data(),
                              cipher.size(), out4.data()),
                  "tampered CCM nonce is rejected");
        } else {
            CHECK(false, "unknown primitive kind");
        }
    }
    CHECK(xts == 2, "both XTS key sizes covered");
    CHECK(cbc == 2, "both CBC key sizes covered");
    CHECK(ccm == 6, "CCM 128/256 x 3 payload lengths covered");
}

static void TestStretchVectors(const std::string& dir)
{
    const auto lines = ReadLines(dir + "stretch.tsv");
    CHECK(lines.size() == 2, "stretch.tsv has 2 rows");
    for (const auto& line : lines) {
        const auto f = SplitTabs(line);
        if (f.size() < 3) { CHECK(false, "stretch.tsv column count"); continue; }
        const auto initial  = FromHex(f[0]);
        const auto salt     = FromHex(f[1]);
        const auto expected = FromHex(f[2]);
        if (initial.size() != 32 || salt.size() != 16) {
            CHECK(false, "stretch vector sizes");
            continue;
        }
        Hash32 init{};
        std::memcpy(init.data(), initial.data(), 32);
        const auto got = StretchKey(init, salt.data());
        CHECK(Hex(got.data(), got.size()) == f[2],
              "0x100000-iteration stretch matches Python hashlib oracle");
        CHECK(expected.size() == 32, "stretch digest length");
    }
}

static void TestGfAlpha()
{
    uint8_t t[16] = {};
    t[15] = 0x80;             // x^127 -> multiplying by alpha overflows
    GfMulAlpha(t);
    CHECK(t[0] == 0x87 && t[15] == 0x00, "GF(2^128) alpha reduction with 0x87");
    uint8_t u[16] = {};
    u[0] = 0x01;
    GfMulAlpha(u);
    CHECK(u[0] == 0x02, "GF(2^128) alpha multiply without carry");
}

int main()
{
    const std::string dir = ResolveGoldenDir();
    std::printf("BitLockerCrypto golden dir: %s\n", dir.c_str());

    TestFipsAes();
    TestFipsSha256();
    TestGfAlpha();
    TestPrimitiveVectors(dir);
    TestStretchVectors(dir);

    std::printf("BitLockerCrypto: %d checks, %d failure(s)\n", g_checks, g_fail);
    if (g_fail != 0) {
        std::printf("  corpus missing? run: python testdata/tools/Gen-BitLockerGolden.py\n");
    }
    return g_fail == 0 ? 0 : 1;
}
