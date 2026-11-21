#pragma once

#if PHOENIX_ENABLE_VOLUME_IO

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <vector>

#pragma comment(lib, "advapi32.lib")

inline bool DfttHexEqI(const char* a, const char* b)
{
    for (; *a && *b; ++a, ++b) {
        if (std::tolower(static_cast<unsigned char>(*a)) !=
            std::tolower(static_cast<unsigned char>(*b)))
            return false;
    }
    return *a == *b;
}

// MD5 helpers shared by the DFTT / carving golden tests.
//
// The original implementation used `goto done` for cleanup, which jumps over
// initialisations of md5[], cb, hexd etc.  MSVC accepts that (C2362 warning),
// but libstdc++ rejects it as a hard error ("jump to label crosses
// initialisation").  RAII + early returns are equivalent and portable across
// MSVC, MinGW GCC 7 and modern GCC.
namespace dftt_md5_detail {

static const char kHexDigits[] = "0123456789abcdef";

struct CryptoCtx
{
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    ~CryptoCtx()
    {
        if (hash) CryptDestroyHash(hash);
        if (prov) CryptReleaseContext(prov, 0);
    }
};

struct FileCtx
{
    HANDLE h;
    explicit FileCtx(HANDLE handle) : h(handle) {}
    ~FileCtx() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};

inline bool Finish(CryptoCtx& c, char out33[33])
{
    BYTE  md5[16]{};
    DWORD cb = sizeof(md5);
    if (!CryptGetHashParam(c.hash, HP_HASHVAL, md5, &cb, 0) || cb != 16)
        return false;
    for (int i = 0; i < 16; ++i) {
        out33[i * 2]     = kHexDigits[md5[i] >> 4];
        out33[i * 2 + 1] = kHexDigits[md5[i] & 15];
    }
    out33[32] = '\0';
    return true;
}

} // namespace dftt_md5_detail

inline bool DfttMd5BufferHexLower(const uint8_t* data, size_t len, char out33[33])
{
    dftt_md5_detail::CryptoCtx c;
    if (!CryptAcquireContextW(&c.prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return false;
    if (!CryptCreateHash(c.prov, CALG_MD5, 0, 0, &c.hash))
        return false;

    size_t off = 0;
    while (off < len) {
        const DWORD chunk = static_cast<DWORD>((std::min)(size_t(1u << 28), len - off));
        if (!CryptHashData(c.hash, data + off, chunk, 0))
            return false;
        off += chunk;
    }
    return dftt_md5_detail::Finish(c, out33);
}

inline bool DfttMd5FileHexLower(const wchar_t* path, char out33[33])
{
    std::vector<uint8_t> buf(4 * 1024 * 1024);
    dftt_md5_detail::FileCtx file(
        CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.h == INVALID_HANDLE_VALUE)
        return false;

    dftt_md5_detail::CryptoCtx c;
    if (!CryptAcquireContextW(&c.prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        return false;
    if (!CryptCreateHash(c.prov, CALG_MD5, 0, 0, &c.hash))
        return false;

    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file.h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) || got == 0)
            break;
        if (!CryptHashData(c.hash, buf.data(), got, 0))
            return false;
    }
    return dftt_md5_detail::Finish(c, out33);
}

#endif
