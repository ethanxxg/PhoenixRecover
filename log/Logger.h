#pragma once

#include <string>
#include <fstream>
#include <mutex>

enum class LogLevel { Debug, Info, Warning, Error, Fatal };

class Logger
{
public:
    static Logger& Instance();

    // ── Level control ──────────────────────────────────────────────────────
    // SetLogLevel         – sets BOTH console and file minimum level (legacy)
    // SetConsoleLogLevel  – console only
    // SetFileLogLevel     – log file only (default Debug)
    void SetLogLevel       (LogLevel level);      // backward compat: sets both
    void SetConsoleLogLevel(LogLevel level);
    void SetFileLogLevel   (LogLevel level);

    // ── Output control ─────────────────────────────────────────────────────
    void SetConsoleOutput(bool enabled);

    // Open an explicit log file (append mode)
    void SetLogFile(const std::wstring& path);

    // Create a timestamped log file automatically, returns the actual path.
    // Typically called once at program startup.
    std::wstring OpenAutoLogFile(const std::wstring& dir = L"");

    // ── Write ──────────────────────────────────────────────────────────────
    void Log(LogLevel level, const std::wstring& msg,
             const char* file = nullptr, int line = 0);

    void Debug  (const std::wstring& m, const char* f = nullptr, int l = 0) { Log(LogLevel::Debug,   m, f, l); }
    void Info   (const std::wstring& m, const char* f = nullptr, int l = 0) { Log(LogLevel::Info,    m, f, l); }
    void Warning(const std::wstring& m, const char* f = nullptr, int l = 0) { Log(LogLevel::Warning, m, f, l); }
    void Error  (const std::wstring& m, const char* f = nullptr, int l = 0) { Log(LogLevel::Error,   m, f, l); }
    void Fatal  (const std::wstring& m, const char* f = nullptr, int l = 0) { Log(LogLevel::Fatal,   m, f, l); }

    // Fast level check – call BEFORE constructing any message string.
    // Returns true if at least one output channel (console or file) would
    // record a message at this level.
    bool ShouldLog(LogLevel level) const noexcept
    {
        return level >= m_consolMinLevel || level >= m_fileMinLevel;
    }

private:
    Logger()  = default;
    ~Logger() = default;
    Logger(const Logger&)            = delete;
    Logger& operator=(const Logger&) = delete;

    static const wchar_t* LevelStr(LogLevel level);
    std::wstring GetTimeStr();
    std::wstring GetDateTimeTag();   // YYYYMMDD_HHMMSS for filenames
    static std::string WideToUtf8(const std::wstring& s);

    std::mutex      m_mutex;
    std::ofstream   m_file;
    LogLevel        m_consolMinLevel = LogLevel::Info;
    LogLevel        m_fileMinLevel   = LogLevel::Debug;
    bool            m_consoleOutput  = true;
};

// ── Macros ──────────────────────────────────────────────────────────────
// LOG_DEBUG / LOG_INFO etc. use lazy evaluation: the message expression is
// only evaluated (and all wstring concatenations performed) when the current
// log level actually requires it.  This is critical for hot-path code paths
// where the logger is at Info/Warning level – it makes LOG_DEBUG essentially
// free (one method call + one comparison + not-taken branch).
#define LOG_DEBUG(msg)  do { if (Logger::Instance().ShouldLog(LogLevel::Debug))   Logger::Instance().Debug  ((msg), __FILE__, __LINE__); } while(0)
#define LOG_INFO(msg)   do { if (Logger::Instance().ShouldLog(LogLevel::Info))    Logger::Instance().Info   ((msg), __FILE__, __LINE__); } while(0)
#define LOG_WARN(msg)   do { if (Logger::Instance().ShouldLog(LogLevel::Warning)) Logger::Instance().Warning((msg), __FILE__, __LINE__); } while(0)
#define LOG_ERROR(msg)  do { if (Logger::Instance().ShouldLog(LogLevel::Error))   Logger::Instance().Error  ((msg), __FILE__, __LINE__); } while(0)
#define LOG_FATAL(msg)  do { if (Logger::Instance().ShouldLog(LogLevel::Fatal))   Logger::Instance().Fatal  ((msg), __FILE__, __LINE__); } while(0)
