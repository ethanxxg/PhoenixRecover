#pragma once

#include <string>

struct _EXCEPTION_POINTERS;

class CrashHandler
{
public:
    static bool Install(const std::wstring& logPath = L"", const std::wstring& crashDir = L"");

    static bool CreateDiagnosticBundle(const std::wstring& reason,
                                       const std::wstring& logPath = L"",
                                       const std::wstring& crashDir = L"",
                                       _EXCEPTION_POINTERS* ep = nullptr);

    static std::wstring ResolveCrashDir(const std::wstring& explicitDir = L"");
};
