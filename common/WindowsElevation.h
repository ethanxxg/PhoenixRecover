#pragma once

#include <string>

namespace WindowsElevation {

// True when current process token is elevated/admin.
bool IsRunningAsAdmin();

// Relaunch current executable with UAC prompt (runas). Returns true if launch was attempted.
bool RelaunchAsAdmin(const std::wstring& extraArgs = L"");

} // namespace WindowsElevation

