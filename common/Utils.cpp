#include "Utils.h"
#include "Platform.h"

#if PHOENIX_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>

namespace Utils
{

#if !PHOENIX_OS_WINDOWS
namespace {

static void AppendUtf8(std::string& out, uint32_t cp)
{
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

static bool DecodeUtf8(const char*& p, const char* end, uint32_t& cp)
{
    if (p >= end)
        return false;
    const unsigned char c0 = static_cast<unsigned char>(*p++);
    if (c0 < 0x80) {
        cp = c0;
        return true;
    }
    if ((c0 & 0xE0) == 0xC0 && p < end) {
        cp = ((c0 & 0x1F) << 6) | (static_cast<unsigned char>(*p++) & 0x3F);
        return true;
    }
    if ((c0 & 0xF0) == 0xE0 && p + 1 <= end) {
        cp = ((c0 & 0x0F) << 12) | ((static_cast<unsigned char>(p[0]) & 0x3F) << 6) |
             (static_cast<unsigned char>(p[1]) & 0x3F);
        p += 2;
        return true;
    }
    if ((c0 & 0xF8) == 0xF0 && p + 2 <= end) {
        cp = ((c0 & 0x07) << 18) | ((static_cast<unsigned char>(p[0]) & 0x3F) << 12) |
             ((static_cast<unsigned char>(p[1]) & 0x3F) << 6) |
             (static_cast<unsigned char>(p[2]) & 0x3F);
        p += 3;
        return true;
    }
    cp = 0xFFFD;
    return true;
}

static void FileTimeToUtcParts(uint64_t ft100ns, int& year, int& month, int& day,
                               int& hour, int& minute, int& second)
{
    constexpr uint64_t kEpochDiff = 11644473600ULL;
    const uint64_t secs = ft100ns / 10000000ULL - kEpochDiff;
    time_t t = static_cast<time_t>(secs);
    struct tm utc {};
#if defined(_WIN32)
    gmtime_s(&utc, &t);
#else
    gmtime_r(&t, &utc);
#endif
    year   = utc.tm_year + 1900;
    month  = utc.tm_mon + 1;
    day    = utc.tm_mday;
    hour   = utc.tm_hour;
    minute = utc.tm_min;
    second = utc.tm_sec;
}

} // namespace
#endif

std::string WideToUtf8(const std::wstring& wide)
{
    if (wide.empty())
        return {};
#if PHOENIX_OS_WINDOWS
    int sz = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (sz <= 0)
        return {};
    std::string out(static_cast<size_t>(sz - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, out.data(), sz, nullptr, nullptr);
    return out;
#else
    std::string out;
    out.reserve(wide.size());
    for (wchar_t wc : wide) {
        const uint32_t cp = static_cast<uint32_t>(wc);
        AppendUtf8(out, cp);
    }
    return out;
#endif
}

std::wstring Utf8ToWide(const std::string& utf8)
{
    if (utf8.empty())
        return {};
#if PHOENIX_OS_WINDOWS
    int sz = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (sz <= 0)
        return {};
    std::wstring out(static_cast<size_t>(sz - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), sz);
    return out;
#else
    std::wstring out;
    const char* p = utf8.data();
    const char* end = p + utf8.size();
    while (p < end) {
        uint32_t cp = 0;
        if (!DecodeUtf8(p, end, cp))
            break;
        out.push_back(static_cast<wchar_t>(cp));
    }
    return out;
#endif
}

std::wstring AnsiToWide(const char* str, int codePage)
{
    if (!str || !*str)
        return {};
#if PHOENIX_OS_WINDOWS
    if (codePage == 0)
        codePage = CP_ACP;
    int sz = MultiByteToWideChar(codePage, 0, str, -1, nullptr, 0);
    if (sz <= 0)
        return {};
    std::wstring out(static_cast<size_t>(sz - 1), L'\0');
    MultiByteToWideChar(codePage, 0, str, -1, out.data(), sz);
    return out;
#else
    (void)codePage;
    return Utf8ToWide(std::string(str));
#endif
}

std::wstring TrimRight(std::wstring s)
{
    while (!s.empty() && (iswspace(s.back()) || s.back() == L'\0'))
        s.pop_back();
    return s;
}

std::string TrimRight(std::string s)
{
    while (!s.empty() && (isspace(static_cast<unsigned char>(s.back())) || s.back() == '\0'))
        s.pop_back();
    return s;
}

bool CreateDirectoryRecursive(const std::wstring& path)
{
    if (path.empty())
        return false;

#if PHOENIX_OS_WINDOWS
    std::wstring p = path;
    for (auto& ch : p) {
        if (ch == L'/')
            ch = L'\\';
    }
    while (!p.empty() && p.back() == L'\\')
        p.pop_back();
    if (p.empty())
        return false;

    std::wstring current;
    size_t start = 0;
    if (p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) {
        current = p.substr(0, 3);
        start   = 3;
    } else if (p.rfind(L"\\\\", 0) == 0) {
        const size_t posServerEnd = p.find(L'\\', 2);
        if (posServerEnd == std::wstring::npos)
            return false;
        const size_t posShareEnd = p.find(L'\\', posServerEnd + 1);
        if (posShareEnd == std::wstring::npos)
            return false;
        current = p.substr(0, posShareEnd + 1);
        start   = posShareEnd + 1;
    }

    size_t i = start;
    while (i < p.size()) {
        while (i < p.size() && p[i] == L'\\')
            ++i;
        if (i >= p.size())
            break;

        const size_t next = p.find(L'\\', i);
        const std::wstring part =
            (next == std::wstring::npos) ? p.substr(i) : p.substr(i, next - i);
        if (!part.empty()) {
            if (!current.empty() && current.back() != L'\\')
                current += L'\\';
            current += part;

            const DWORD attr = GetFileAttributesW(current.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES) {
                if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    return false;
            } else {
                if (!CreateDirectoryW(current.c_str(), nullptr)) {
                    const DWORD err = GetLastError();
                    if (err != ERROR_ALREADY_EXISTS)
                        return false;
                }
            }
        }
        if (next == std::wstring::npos)
            break;
        i = next + 1;
    }
    return true;
#else
    std::string p = WideToUtf8(path);
    if (p.empty())
        return false;
    for (auto& ch : p) {
        if (ch == '\\')
            ch = '/';
    }
    while (!p.empty() && p.back() == '/')
        p.pop_back();

    std::string current;
    size_t i = 0;
    if (!p.empty() && p[0] == '/') {
        current = "/";
        i       = 1;
    }

    while (i < p.size()) {
        while (i < p.size() && p[i] == '/')
            ++i;
        if (i >= p.size())
            break;
        const size_t next = p.find('/', i);
        const std::string part =
            (next == std::string::npos) ? p.substr(i) : p.substr(i, next - i);
        if (!part.empty()) {
            if (!current.empty() && current.back() != '/')
                current += '/';
            current += part;
            struct stat st {};
            if (stat(current.c_str(), &st) == 0) {
                if (!S_ISDIR(st.st_mode))
                    return false;
            } else if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
        if (next == std::string::npos)
            break;
        i = next + 1;
    }
    return true;
#endif
}

bool FileExists(const std::wstring& path)
{
    if (path.empty())
        return false;
#if PHOENIX_OS_WINDOWS
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES)
        return false;
    return (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
#else
    struct stat st {};
    if (stat(WideToUtf8(path).c_str(), &st) != 0)
        return false;
    return S_ISREG(st.st_mode);
#endif
}

static std::wstring DirNameOf(const std::wstring& path)
{
    const size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return L"";
    return path.substr(0, pos);
}

static std::wstring BaseNameOf(const std::wstring& path)
{
    const size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

static void SplitNameAndExt(const std::wstring& base, std::wstring& outName,
                            std::wstring& outExtWithDot)
{
    const size_t dot = base.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) {
        outName = base;
        outExtWithDot.clear();
        return;
    }
    outName       = base.substr(0, dot);
    outExtWithDot = base.substr(dot);
}

std::wstring MakeUniqueFilePath(const std::wstring& desiredPath, uint32_t maxAttempts)
{
    if (desiredPath.empty())
        return desiredPath;
    if (!FileExists(desiredPath))
        return desiredPath;

    const std::wstring dir  = DirNameOf(desiredPath);
    const std::wstring base = BaseNameOf(desiredPath);

    std::wstring name, ext;
    SplitNameAndExt(base, name, ext);
    if (name.empty())
        name = L"recovered";

#if PHOENIX_OS_WINDOWS
    const wchar_t sep = L'\\';
#else
    const wchar_t sep = L'/';
#endif

    for (uint32_t i = 1; i < maxAttempts; ++i) {
        const std::wstring candidateBase = name + L" (" + std::to_wstring(i) + L")" + ext;
        const std::wstring candidate =
            dir.empty() ? candidateBase : (dir + sep + candidateBase);
        if (!FileExists(candidate))
            return candidate;
    }
    return desiredPath;
}

uint64_t GetFreeDiskSpace(const std::wstring& path)
{
#if PHOENIX_OS_WINDOWS
    ULARGE_INTEGER freeBytes {};
    if (GetDiskFreeSpaceExW(path.c_str(), &freeBytes, nullptr, nullptr))
        return freeBytes.QuadPart;
    return 0;
#else
    struct statvfs vfs {};
    const std::string utf8 = WideToUtf8(path);
    if (statvfs(utf8.c_str(), &vfs) != 0)
        return 0;
    return static_cast<uint64_t>(vfs.f_bavail) * static_cast<uint64_t>(vfs.f_frsize);
#endif
}

std::string FormatBytes(uint64_t bytes)
{
    constexpr const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double val = static_cast<double>(bytes);
    int i      = 0;
    while (val >= 1024.0 && i < 4) {
        val /= 1024.0;
        ++i;
    }
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << val << ' ' << units[i];
    return oss.str();
}

std::string FormatFiletime(uint64_t ft100ns)
{
    if (ft100ns == 0)
        return "(unknown)";
#if PHOENIX_OS_WINDOWS
    FILETIME ft {};
    ft.dwLowDateTime  = static_cast<DWORD>(ft100ns & 0xFFFFFFFF);
    ft.dwHighDateTime = static_cast<DWORD>(ft100ns >> 32);
    SYSTEMTIME st {};
    FileTimeToSystemTime(&ft, &st);
    std::ostringstream oss;
    oss << st.wYear << '-' << std::setfill('0') << std::setw(2) << st.wMonth << '-'
        << std::setfill('0') << std::setw(2) << st.wDay << ' ' << std::setfill('0')
        << std::setw(2) << st.wHour << ':' << std::setfill('0') << std::setw(2) << st.wMinute
        << ':' << std::setfill('0') << std::setw(2) << st.wSecond;
    return oss.str();
#else
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    FileTimeToUtcParts(ft100ns, y, mo, d, h, mi, s);
    std::ostringstream oss;
    oss << y << '-' << std::setfill('0') << std::setw(2) << mo << '-' << std::setfill('0')
        << std::setw(2) << d << ' ' << std::setfill('0') << std::setw(2) << h << ':'
        << std::setfill('0') << std::setw(2) << mi << ':' << std::setfill('0') << std::setw(2)
        << s;
    return oss.str();
#endif
}

std::string HexDump(const uint8_t* data, size_t length, size_t maxBytes)
{
    std::ostringstream oss;
    const size_t count = std::min(length, maxBytes);
    for (size_t i = 0; i < count; i += 16) {
        oss << std::hex << std::setfill('0') << std::setw(4) << i << ": ";
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < count)
                oss << std::setw(2) << static_cast<int>(data[i + j]) << ' ';
            else
                oss << "   ";
            if (j == 7)
                oss << ' ';
        }
        oss << " |";
        for (size_t j = 0; j < 16 && i + j < count; ++j) {
            const char c = static_cast<char>(data[i + j]);
            oss << (isprint(static_cast<unsigned char>(c)) ? c : '.');
        }
        oss << "|\n";
    }
    return oss.str();
}

uint64_t NowFileTime()
{
#if PHOENIX_OS_WINDOWS
    FILETIME ftNow {};
    GetSystemTimeAsFileTime(&ftNow);
    return (static_cast<uint64_t>(ftNow.dwHighDateTime) << 32) |
           static_cast<uint64_t>(ftNow.dwLowDateTime);
#else
    using namespace std::chrono;
    const auto now   = system_clock::now().time_since_epoch();
    const auto secs  = duration_cast<seconds>(now).count();
    const auto ns    = duration_cast<nanoseconds>(now).count() % 1000000000LL;
    constexpr uint64_t kEpochDiff = 116444736000000000ULL;
    return static_cast<uint64_t>(secs) * 10000000ULL + kEpochDiff + static_cast<uint64_t>(ns / 100);
#endif
}

std::wstring GetTempDirectoryW()
{
#if PHOENIX_OS_WINDOWS
    wchar_t buf[MAX_PATH] {};
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    if (n == 0 || n >= MAX_PATH)
        return L".\\";
    std::wstring out(buf);
    if (!out.empty() && out.back() != L'\\' && out.back() != L'/')
        out += L'\\';
    return out;
#else
    const char* t = std::getenv("TMPDIR");
    if (!t || !*t)
        t = "/tmp";
    std::wstring out = Utf8ToWide(t);
    if (!out.empty() && out.back() != L'/')
        out += L'/';
    return out;
#endif
}

std::wstring GetExecutableDirectoryW()
{
#if PHOENIX_OS_WINDOWS
    wchar_t buf[MAX_PATH] {};
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH) == 0)
        return L".";
    std::wstring path(buf);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        path.resize(slash);
    return path;
#else
    char buf[4096] {};
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return L".";
    buf[n] = '\0';
    std::wstring path = Utf8ToWide(buf);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        path.resize(slash);
    return path;
#endif
}

std::wstring GetEnvironmentVariableW(const wchar_t* name)
{
    if (!name || !*name)
        return L"";
#if PHOENIX_OS_WINDOWS
    wchar_t buf[4096] {};
    const DWORD n = ::GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf))
        return L"";
    return std::wstring(buf);
#else
    const std::string key = WideToUtf8(name);
    const char* val       = std::getenv(key.c_str());
    if (!val)
        return L"";
    return Utf8ToWide(val);
#endif
}

} // namespace Utils

