#include "WindowsElevation.h"

#ifdef _WIN32
#include <Windows.h>
#include <Shellapi.h>
#endif

namespace WindowsElevation {

bool IsRunningAsAdmin()
{
#ifndef _WIN32
    return true;
#else
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2,
                                 SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS,
                                 0, 0, 0, 0, 0, 0,
                                 &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
#endif
}

bool RelaunchAsAdmin(const std::wstring& extraArgs)
{
#ifndef _WIN32
    (void)extraArgs;
    return false;
#else
    wchar_t exePath[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) {
        return false;
    }

    std::wstring params;
    if (!extraArgs.empty()) {
        params = extraArgs;
    }

    HINSTANCE rc = ShellExecuteW(nullptr,
                                 L"runas",
                                 exePath,
                                 params.empty() ? nullptr : params.c_str(),
                                 nullptr,
                                 SW_SHOWNORMAL);
    return reinterpret_cast<intptr_t>(rc) > 32;
#endif
}

} // namespace WindowsElevation

