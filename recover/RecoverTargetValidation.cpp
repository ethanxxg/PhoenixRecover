#include "RecoverEngine.h"

#ifdef _WIN32
#include <Windows.h>
#include <winioctl.h>
#endif

#include "../log/Logger.h"

#include <algorithm>

#ifdef _WIN32
namespace {

bool volumeUsesPhysicalDisk(const wchar_t *volumeRoot, int physicalDiskIdx, bool &verified)
{
    verified = false;
    if (!volumeRoot || physicalDiskIdx < 0) {
        return false;
    }

    std::wstring devicePath = L"\\\\.\\";
    devicePath += volumeRoot;
    while (!devicePath.empty() && devicePath.back() == L'\\') {
        devicePath.pop_back();
    }

    HANDLE handle = CreateFileW(devicePath.c_str(), 0,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    BYTE buffer[sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 31]{};
    DWORD bytesReturned = 0;
    const BOOL ok = DeviceIoControl(handle,
                                    IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                    nullptr, 0,
                                    buffer, sizeof(buffer),
                                    &bytesReturned, nullptr);
    CloseHandle(handle);
    if (!ok || bytesReturned < sizeof(VOLUME_DISK_EXTENTS)) {
        return false;
    }

    const auto *extents = reinterpret_cast<const VOLUME_DISK_EXTENTS *>(buffer);
    const DWORD count = (std::min)(extents->NumberOfDiskExtents,
                                   static_cast<DWORD>(31));
    verified = true;
    for (DWORD i = 0; i < count; ++i) {
        if (extents->Extents[i].DiskNumber == physicalDiskIdx) {
            return true;
        }
    }
    return false;
}

} // namespace
#endif

bool RecoverEngine::ValidateTargetPath(const std::wstring& targetDir,
                                       const Partition& source)
{
#ifndef _WIN32
    (void)targetDir;
    (void)source;
    return true;
#else
    if (targetDir.empty()) return false;

    wchar_t targetVolume[MAX_PATH]{};
    if (!GetVolumePathNameW(targetDir.c_str(), targetVolume, MAX_PATH)) {
        // Cannot verify – allow (best-effort safety)
        return true;
    }

    if (!source.driveLetter.empty()) {
        wchar_t srcVolume[MAX_PATH]{};
        std::wstring srcPath = source.driveLetter + L"\\";
        GetVolumePathNameW(srcPath.c_str(), srcVolume, MAX_PATH);

        if (_wcsicmp(targetVolume, srcVolume) == 0) {
            LOG_ERROR(L"RecoverEngine: target path is on the same volume as source!");
            return false;
        }
    }

    if (source.driveLetter.empty() && source.physicalDiskIdx >= 0) {
        bool verified = false;
        const bool samePhysicalDisk =
            volumeUsesPhysicalDisk(targetVolume, source.physicalDiskIdx, verified);
        if (!verified) {
            LOG_ERROR(L"RecoverEngine: cannot verify target physical disk for raw partition source");
            return false;
        }
        if (samePhysicalDisk) {
            LOG_ERROR(L"RecoverEngine: target path is on the same physical disk as raw partition source");
            return false;
        }
    }

    return true;
#endif
}

