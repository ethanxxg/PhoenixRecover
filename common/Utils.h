#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace Utils
{
    // ── String conversion ─────────────────────────────────────────────────
    std::string  WideToUtf8(const std::wstring& wide);
    std::wstring Utf8ToWide(const std::string& utf8);
    std::wstring AnsiToWide(const char* str, int codePage = 0 /* CP_ACP */);

    // Trim trailing whitespace / null characters (useful for OEM ID fields)
    std::wstring TrimRight(std::wstring s);
    std::string  TrimRight(std::string s);

    // ── File system helpers ───────────────────────────────────────────────
    bool     CreateDirectoryRecursive(const std::wstring& path);
    bool     FileExists(const std::wstring& path);
    // If 'desiredPath' exists, returns a new path with " (n)" suffix before extension.
    // If it does not exist, returns desiredPath.
    // Example: "a.txt" -> "a (1).txt"
    std::wstring MakeUniqueFilePath(const std::wstring& desiredPath, uint32_t maxAttempts = 10000);
    uint64_t GetFreeDiskSpace(const std::wstring& path);

    // ── Formatting ────────────────────────────────────────────────────────
    std::string FormatBytes(uint64_t bytes);
    std::string FormatFiletime(uint64_t ft100ns); // Windows FILETIME (100-ns intervals since 1601)

    // ── Binary read helpers (little-endian, unaligned safe) ───────────────
    inline uint16_t ReadLE16(const void* p)
    {
        auto b = static_cast<const uint8_t*>(p);
        return static_cast<uint16_t>(b[0] | (b[1] << 8));
    }

    inline uint32_t ReadLE32(const void* p)
    {
        auto b = static_cast<const uint8_t*>(p);
        return b[0] | (static_cast<uint32_t>(b[1]) << 8)
                    | (static_cast<uint32_t>(b[2]) << 16)
                    | (static_cast<uint32_t>(b[3]) << 24);
    }

    inline uint64_t ReadLE64(const void* p)
    {
        auto b = static_cast<const uint8_t*>(p);
        return  static_cast<uint64_t>(b[0])
              | (static_cast<uint64_t>(b[1]) << 8)
              | (static_cast<uint64_t>(b[2]) << 16)
              | (static_cast<uint64_t>(b[3]) << 24)
              | (static_cast<uint64_t>(b[4]) << 32)
              | (static_cast<uint64_t>(b[5]) << 40)
              | (static_cast<uint64_t>(b[6]) << 48)
              | (static_cast<uint64_t>(b[7]) << 56);
    }

    // Read a variable-width signed integer (used for NTFS DataRun offsets)
    inline int64_t ReadSignedLE(const uint8_t* p, int byteCount)
    {
        if (byteCount <= 0 || byteCount > 8) return 0;
        uint64_t val = 0;
        for (int i = 0; i < byteCount; ++i)
            val |= static_cast<uint64_t>(p[i]) << (i * 8);
        if (byteCount < 8) {
            const uint64_t signBit = 1ULL << (byteCount * 8 - 1);
            if (val & signBit)
                val |= ~(signBit * 2 - 1);
        }
        return static_cast<int64_t>(val);
    }

    // Round up to next multiple of alignment
    inline uint64_t AlignUp(uint64_t value, uint64_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    // ── Hex dump (debug) ──────────────────────────────────────────────────
    std::string HexDump(const uint8_t* data, size_t length, size_t maxBytes = 256);

    // ── Hex formatting ──────────────────────────────────────────────────────
    inline std::wstring ToHexW(uint32_t val, int minDigits = 4)
    {
        wchar_t buf[12]{};
#if defined(_MSC_VER)
        swprintf_s(buf, L"%0*X", minDigits, val);
#else
        swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%0*X", minDigits, val);
#endif
        return buf;
    }

    // ── Time helpers ──────────────────────────────────────────────────────
    // 当前系统时间（FILETIME，单位 100ns，自 1601-01-01 起）
    uint64_t NowFileTime();

    // ── Platform helpers (tests / reports) ────────────────────────────────
    std::wstring GetTempDirectoryW();
    std::wstring GetExecutableDirectoryW();
    std::wstring GetEnvironmentVariableW(const wchar_t* name);
}
