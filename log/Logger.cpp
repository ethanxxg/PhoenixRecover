#include "Logger.h"



#include "../common/Platform.h"

#include "../common/Utils.h"



#if PHOENIX_OS_WINDOWS

#define WIN32_LEAN_AND_MEAN

#define NOMINMAX

#include <Windows.h>

#else

#include <chrono>

#include <ctime>

#endif



#include <iostream>

#include <sstream>

#include <iomanip>

#include <cstring>

#include <vector>



Logger& Logger::Instance()

{

    static Logger inst;

    return inst;

}



void Logger::SetLogLevel(LogLevel level)

{

    std::lock_guard<std::mutex> lk(m_mutex);

    m_consolMinLevel = level;

    m_fileMinLevel   = level;

}



void Logger::SetConsoleLogLevel(LogLevel level)

{

    std::lock_guard<std::mutex> lk(m_mutex);

    m_consolMinLevel = level;

}



void Logger::SetFileLogLevel(LogLevel level)

{

    std::lock_guard<std::mutex> lk(m_mutex);

    m_fileMinLevel = level;

}



void Logger::SetConsoleOutput(bool enabled)

{

    std::lock_guard<std::mutex> lk(m_mutex);

    m_consoleOutput = enabled;

}



void Logger::SetLogFile(const std::wstring& path)

{

    std::lock_guard<std::mutex> lk(m_mutex);

    if (m_file.is_open())

        m_file.close();

    const std::string utf8 = Utils::WideToUtf8(path);

    m_file.open(utf8, std::ios::out | std::ios::trunc | std::ios::binary);

    if (m_file.is_open()) {

        static const unsigned char bom[] = {0xEF, 0xBB, 0xBF};

        m_file.write(reinterpret_cast<const char*>(bom), sizeof(bom));

        m_file.flush();

    }

}



std::wstring Logger::OpenAutoLogFile(const std::wstring& dir)

{

    std::wstring folder = dir;

    if (folder.empty())

        folder = Utils::GetExecutableDirectoryW();

#if PHOENIX_OS_WINDOWS

    if (!folder.empty() && folder.back() != L'\\')

        folder += L'\\';

#else

    if (!folder.empty() && folder.back() != L'/')

        folder += L'/';

#endif



    const std::wstring tag  = GetDateTimeTag();

    const std::wstring path = folder + L"phoenix_" + tag + L".log";



    std::lock_guard<std::mutex> lk(m_mutex);

    if (m_file.is_open())

        m_file.close();

    const std::string utf8 = Utils::WideToUtf8(path);

    m_file.open(utf8, std::ios::out | std::ios::trunc | std::ios::binary);

    if (m_file.is_open()) {

        static const unsigned char bom[] = {0xEF, 0xBB, 0xBF};

        m_file.write(reinterpret_cast<const char*>(bom), sizeof(bom));

        const std::wstring header =

            L"=============================================================\n"

            L"  PhoenixRecover  scan log  " +

            tag + L"\n"

                  L"=============================================================\n";

        const std::string headerUtf8 = Utils::WideToUtf8(header);

        m_file.write(headerUtf8.data(), static_cast<std::streamsize>(headerUtf8.size()));

        m_file.flush();

    }

    return path;

}



void Logger::Log(LogLevel level, const std::wstring& msg, const char* file, int line)

{

    const std::wstring timeStr = GetTimeStr();

    const wchar_t* lvl         = LevelStr(level);



    std::wostringstream oss;

    oss << L'[' << timeStr << L"] [" << lvl << L"] " << msg;



    if (file) {

        const char* slash = strrchr(file, '\\');

        if (!slash)

            slash = strrchr(file, '/');

        const char* fname = slash ? slash + 1 : file;

        oss << L"  (" << Utils::Utf8ToWide(fname) << L':' << line << L')';

    }

    oss << L'\n';



    const std::wstring lineStr = oss.str();



    std::lock_guard<std::mutex> lk(m_mutex);

    const bool toConsole = m_consoleOutput && (level >= m_consolMinLevel);

    const bool toFile    = m_file.is_open() && (level >= m_fileMinLevel);

    if (!toConsole && !toFile)

        return;



    if (toConsole) {

        if (level >= LogLevel::Warning)

            std::wcerr << lineStr;

        else

            std::wcout << lineStr;

    }



    if (toFile && m_file.is_open()) {

        if (!m_file.good())

            m_file.clear();

        const std::string utf8 = Utils::WideToUtf8(lineStr);

        m_file.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));

        m_file.flush();

    }

}



const wchar_t* Logger::LevelStr(LogLevel level)

{

    switch (level) {

    case LogLevel::Debug:

        return L"DEBUG";

    case LogLevel::Info:

        return L"INFO ";

    case LogLevel::Warning:

        return L"WARN ";

    case LogLevel::Error:

        return L"ERROR";

    case LogLevel::Fatal:

        return L"FATAL";

    default:

        return L"?????";

    }

}



std::wstring Logger::GetTimeStr()

{

#if PHOENIX_OS_WINDOWS

    SYSTEMTIME st {};

    GetLocalTime(&st);

    wchar_t buf[32] {};

    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d.%03d", st.wYear, st.wMonth, st.wDay, st.wHour,

               st.wMinute, st.wSecond, st.wMilliseconds);

    return buf;

#else

    using namespace std::chrono;

    const auto now = system_clock::now();

    const auto ms  = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    const time_t t = system_clock::to_time_t(now);

    struct tm local {};

    localtime_r(&t, &local);

    wchar_t buf[32] {};

    swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%04d-%02d-%02d %02d:%02d:%02d.%03ld",

             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min,

             local.tm_sec, static_cast<long>(ms.count()));

    return buf;

#endif

}



std::wstring Logger::GetDateTimeTag()

{

#if PHOENIX_OS_WINDOWS

    SYSTEMTIME st {};

    GetLocalTime(&st);

    wchar_t buf[32] {};

    swprintf_s(buf, L"%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour,

               st.wMinute, st.wSecond);

    return buf;

#else

    const std::wstring ts = GetTimeStr();

    std::wstring tag;

    tag.reserve(15);

    for (wchar_t c : ts) {

        if (iswdigit(c))

            tag.push_back(c);

        if (tag.size() == 8)

            tag.push_back(L'_');

        if (tag.size() == 15)

            break;

    }

    return tag;

#endif

}



std::string Logger::WideToUtf8(const std::wstring& s)

{

    return Utils::WideToUtf8(s);

}


