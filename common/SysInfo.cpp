#include "SysInfo.h"
#include "Platform.h"
#include "Utils.h"

#include <string>
#include <sstream>

#if PHOENIX_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <VersionHelpers.h>
#include <intrin.h>
#pragma comment(lib, "version.lib")
#elif PHOENIX_OS_LINUX_DESKTOP
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <fstream>
#endif

#if PHOENIX_OS_WINDOWS
// ─────────────────────────────────────────────────────────────────────────────
// OS 名称（通过 RtlGetVersion 获取真实版本，绕过兼容性 shim）
// ─────────────────────────────────────────────────────────────────────────────
static std::wstring GetOsName()
{
    // 使用 RtlGetVersion（ntdll）获取真实版本，避免 GetVersionEx 被 shim
    typedef LONG(WINAPI* PFN_RtlGetVersion)(PRTL_OSVERSIONINFOW);
    HMODULE hNtdll = ::GetModuleHandleW(L"ntdll.dll");
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (hNtdll) {
        auto fn = (PFN_RtlGetVersion)::GetProcAddress(hNtdll, "RtlGetVersion");
        if (fn) fn(&vi);
    }

    DWORD major = vi.dwMajorVersion;
    DWORD minor = vi.dwMinorVersion;
    DWORD build = vi.dwBuildNumber;

    std::wstring name;
    if (major == 10 && build >= 22000)
        name = L"Windows 11";
    else if (major == 10)
        name = L"Windows 10";
    else if (major == 6 && minor == 3)
        name = L"Windows 8.1";
    else if (major == 6 && minor == 2)
        name = L"Windows 8";
    else if (major == 6 && minor == 1)
        name = L"Windows 7";
    else {
        wchar_t buf[32];
        swprintf_s(buf, L"Windows %lu.%lu", major, minor);
        name = buf;
    }

    // 读取注册表 EditionID 区分 Pro / Home / Enterprise 等
    HKEY hKey{};
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
            0, KEY_READ, &hKey) == ERROR_SUCCESS)
    {
        wchar_t edition[128]{};
        DWORD sz = sizeof(edition);
        DWORD type = REG_SZ;
        if (::RegQueryValueExW(hKey, L"EditionID", nullptr,
                &type, (LPBYTE)edition, &sz) == ERROR_SUCCESS)
        {
            name += L" ";
            name += edition;
        }
        ::RegCloseKey(hKey);
    }

    // 64-bit 检测
    SYSTEM_INFO si{};
    ::GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
        si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64)
        name += L" 64-bit";
    else
        name += L" 32-bit";

    return name;
}

// ─────────────────────────────────────────────────────────────────────────────
// 是否以管理员权限运行
// ─────────────────────────────────────────────────────────────────────────────
static bool IsRunningAsAdmin()
{
    BOOL isAdmin = FALSE;
    PSID pAdminSid = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    if (::AllocateAndInitializeSid(&ntAuth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &pAdminSid))
    {
        ::CheckTokenMembership(nullptr, pAdminSid, &isAdmin);
        ::FreeSid(pAdminSid);
    }
    return isAdmin == TRUE;
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU 名称（通过 CPUID）
// ─────────────────────────────────────────────────────────────────────────────
static std::wstring GetCpuName()
{
    char brand[49]{};
    int info[4]{};
    __cpuid(info, 0x80000000);
    if ((unsigned)info[0] >= 0x80000004) {
        __cpuid((int*)(brand +  0), 0x80000002);
        __cpuid((int*)(brand + 16), 0x80000003);
        __cpuid((int*)(brand + 32), 0x80000004);
    }
    // 去掉首尾空格
    std::string s(brand);
    size_t b = s.find_first_not_of(' ');
    size_t e = s.find_last_not_of(' ');
    if (b == std::string::npos) return L"Unknown CPU";
    s = s.substr(b, e - b + 1);
    // 转 wstring
    std::wstring ws(s.begin(), s.end());
    return ws;
}

// ─────────────────────────────────────────────────────────────────────────────
// 物理内存（MB）
// ─────────────────────────────────────────────────────────────────────────────
static uint32_t GetRamMB()
{
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    ::GlobalMemoryStatusEx(&ms);
    return (uint32_t)(ms.ullTotalPhys / (1024 * 1024));
}

// ─────────────────────────────────────────────────────────────────────────────
// GPU 名称（通过注册表 Display 枚举）
// ─────────────────────────────────────────────────────────────────────────────
static std::wstring GetGpuName()
{
    HKEY hKey{};
    // 枚举 HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}
    const wchar_t* kPath =
        L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
        L"{4d36e968-e325-11ce-bfc1-08002be10318}";
    if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, kPath, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return L"Unknown GPU";

    std::wstring result;
    wchar_t subName[16]{};
    DWORD idx = 0;
    while (result.empty()) {
        DWORD subSz = _countof(subName);
        if (::RegEnumKeyExW(hKey, idx++, subName, &subSz,
                nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;

        HKEY hSub{};
        if (::RegOpenKeyExW(hKey, subName, 0, KEY_READ, &hSub) != ERROR_SUCCESS)
            continue;

        wchar_t desc[256]{};
        DWORD sz = sizeof(desc);
        DWORD type = REG_SZ;
        if (::RegQueryValueExW(hSub, L"DriverDesc", nullptr,
                &type, (LPBYTE)desc, &sz) == ERROR_SUCCESS)
        {
            std::wstring s(desc);
            // 跳过 Microsoft 基础渲染驱动
            if (s.find(L"Microsoft") == std::wstring::npos)
                result = s;
        }
        ::RegCloseKey(hSub);
    }
    ::RegCloseKey(hKey);
    return result.empty() ? L"Unknown GPU" : result;
}

// ─────────────────────────────────────────────────────────────────────────────
SysInfo SysInfo::Collect()
{
    SysInfo si;
    si.osName  = GetOsName();
    si.isAdmin = IsRunningAsAdmin();
    si.cpuName = GetCpuName();
    si.ramMB = GetRamMB();
    si.gpuName = GetGpuName();
    return si;
}

#elif PHOENIX_OS_LINUX_DESKTOP
SysInfo SysInfo::Collect()
{
    SysInfo si;
    struct utsname u {};
    if (uname(&u) == 0) {
        si.osName = Utils::Utf8ToWide(std::string(u.sysname) + " " + u.release);
    } else {
        si.osName = L"Linux";
    }
    si.isAdmin = (geteuid() == 0);
    {
        std::ifstream cpu("/proc/cpuinfo");
        std::string line;
        while (std::getline(cpu, line)) {
            if (line.rfind("model name", 0) == 0) {
                const auto pos = line.find(':');
                if (pos != std::string::npos) {
                    si.cpuName = Utils::Utf8ToWide(line.substr(pos + 1));
                    while (!si.cpuName.empty() && iswspace(si.cpuName.front()))
                        si.cpuName.erase(si.cpuName.begin());
                }
                break;
            }
        }
        if (si.cpuName.empty())
            si.cpuName = L"Unknown CPU";
    }
    struct sysinfo mem {};
    if (sysinfo(&mem) == 0)
        si.ramMB = static_cast<uint32_t>((mem.totalram * mem.mem_unit) / (1024 * 1024));
    si.gpuName = L"GPU";
    return si;
}
#else
SysInfo SysInfo::Collect()
{
    SysInfo si;
    si.osName  = L"Unknown OS";
    si.cpuName = L"Unknown CPU";
    si.gpuName = L"GPU";
    return si;
}
#endif

std::wstring SysInfo::Line1() const
{
    return L"Phoenix Recover  v1.0.0 (64-bit)";
}

std::wstring SysInfo::Line2() const
{
    std::wstring s = osName;
    if (isAdmin) s += L"  (Admin)";
    return s;
}

std::wstring SysInfo::Line3() const
{
    // RAM：四舍五入到最近的整 GB
    uint32_t gb = (ramMB + 512) / 1024;

    wchar_t buf[512]{};
#if defined(_MSC_VER)
    swprintf_s(buf, L"%s,  %u GB RAM,  %s", cpuName.c_str(), gb, gpuName.c_str());
#else
    swprintf(buf, sizeof(buf) / sizeof(buf[0]), L"%ls,  %u GB RAM,  %ls", cpuName.c_str(), gb,
             gpuName.c_str());
#endif
    return buf;
}
