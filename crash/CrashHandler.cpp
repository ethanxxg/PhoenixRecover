#include "CrashHandler.h"
#include "../common/Platform.h"
#include "../common/Utils.h"
#include "../log/Logger.h"

#include <fstream>
#if !(defined(__GNUC__) && __GNUC__ < 8)
#include <filesystem>
#endif

#if PHOENIX_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <DbgHelp.h>
#include <sstream>

namespace {
std::wstring g_logPath;
std::wstring g_crashDir;

// std::wofstream(std::wstring) is an MSVC extension; libstdc++ needs an
// explicit std::filesystem::path.  MinGW GCC 7 has no <filesystem>, so it
// falls back to a narrow UTF-8 path on that legacy toolchain.
std::wofstream OpenWideTextFile(const std::wstring& path)
{
#if defined(__GNUC__) && __GNUC__ < 8
    return std::wofstream(Utils::WideToUtf8(path), std::ios::out | std::ios::trunc);
#else
    return std::wofstream(std::filesystem::path(path), std::ios::out | std::ios::trunc);
#endif
}

std::wstring NowTag()
{
    SYSTEMTIME st {};
    GetLocalTime(&st);
    wchar_t buf[64] {};
    swprintf_s(buf, L"%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour,
               st.wMinute, st.wSecond);
    return buf;
}

bool EnsureDir(const std::wstring& dir)
{
    return Utils::CreateDirectoryRecursive(dir);
}

bool CopyIfExists(const std::wstring& src, const std::wstring& dst)
{
    if (src.empty())
        return false;
    const DWORD a = GetFileAttributesW(src.c_str());
    if (a == INVALID_FILE_ATTRIBUTES)
        return false;
    return CopyFileW(src.c_str(), dst.c_str(), FALSE) != 0;
}

bool WriteMiniDump(const std::wstring& dumpPath, _EXCEPTION_POINTERS* ep)
{
    HANDLE hFile = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    MINIDUMP_EXCEPTION_INFORMATION mei {};
    mei.ThreadId           = GetCurrentThreadId();
    mei.ExceptionPointers  = ep;
    mei.ClientPointers     = FALSE;

    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                                      MiniDumpNormal, ep ? &mei : nullptr, nullptr, nullptr);
    CloseHandle(hFile);
    return ok != FALSE;
}

LONG WINAPI UnhandledFilter(_EXCEPTION_POINTERS* ep)
{
    CrashHandler::CreateDiagnosticBundle(L"unhandled_exception", g_logPath, g_crashDir, ep);
    return EXCEPTION_EXECUTE_HANDLER;
}
} // namespace

std::wstring CrashHandler::ResolveCrashDir(const std::wstring& explicitDir)
{
    if (!explicitDir.empty())
        return explicitDir;

    std::wstring dir = Utils::GetExecutableDirectoryW();
    if (!dir.empty() && dir.back() != L'\\')
        dir += L'\\';
    dir += L"crash_reports";
    return dir;
}

bool CrashHandler::Install(const std::wstring& logPath, const std::wstring& crashDir)
{
    g_logPath  = logPath;
    g_crashDir = ResolveCrashDir(crashDir);
    EnsureDir(g_crashDir);
    SetUnhandledExceptionFilter(UnhandledFilter);
    LOG_INFO(L"CrashHandler installed. dir=" + g_crashDir);
    return true;
}

bool CrashHandler::CreateDiagnosticBundle(const std::wstring& reason, const std::wstring& logPath,
                                          const std::wstring& crashDir, _EXCEPTION_POINTERS* ep)
{
    const std::wstring outDir = ResolveCrashDir(crashDir.empty() ? g_crashDir : crashDir);
    if (!EnsureDir(outDir))
        return false;

    const std::wstring tag     = NowTag();
    const std::wstring base    = outDir + L"\\crash_" + tag;
    const std::wstring meta    = base + L".txt";
    const std::wstring dump    = base + L".dmp";
    const std::wstring logCopy = base + L".log";

    std::wofstream mf = OpenWideTextFile(meta);
    if (mf.is_open()) {
        mf << L"reason=" << reason << L"\n";
        mf << L"pid=" << GetCurrentProcessId() << L"\n";
        mf << L"tid=" << GetCurrentThreadId() << L"\n";
        mf << L"log_path=" << (logPath.empty() ? g_logPath : logPath) << L"\n";
        mf << L"dump_path=" << dump << L"\n";
        mf.close();
    }

    const bool dumpOk = WriteMiniDump(dump, ep);
    const std::wstring lp = logPath.empty() ? g_logPath : logPath;
    const bool logOk    = CopyIfExists(lp, logCopy);

    if (dumpOk)
        LOG_ERROR(L"Crash dump written: " + dump);
    if (logOk)
        LOG_INFO(L"Crash log copy written: " + logCopy);
    return dumpOk;
}

#else

std::wstring CrashHandler::ResolveCrashDir(const std::wstring& explicitDir)
{
    if (!explicitDir.empty())
        return explicitDir;
    std::wstring dir = Utils::GetExecutableDirectoryW();
    if (!dir.empty() && dir.back() != L'/')
        dir += L'/';
    dir += L"crash_reports";
    return dir;
}

bool CrashHandler::Install(const std::wstring& logPath, const std::wstring& crashDir)
{
    (void)logPath;
    Utils::CreateDirectoryRecursive(ResolveCrashDir(crashDir));
    LOG_INFO(L"CrashHandler: minidump not available on this platform");
    return true;
}

bool CrashHandler::CreateDiagnosticBundle(const std::wstring& reason, const std::wstring& logPath,
                                          const std::wstring& crashDir, _EXCEPTION_POINTERS* ep)
{
    (void)reason;
    (void)logPath;
    (void)crashDir;
    (void)ep;
    return false;
}

#endif

