#include <cassert>
#include <string>

#ifdef _WIN32
#include <Windows.h>
#endif

#include "../disk/Partition.h"
#include "../recover/RecoverEngine.h"

static std::wstring DriveLetterFromPath(const std::wstring& path)
{
    if (path.size() >= 2 && path[1] == L':') {
        return path.substr(0, 2); // "C:"
    }
    return L"";
}

int main()
{
#ifndef _WIN32
    // ValidateTargetPath is Windows-only (GetVolumePathNameW).
    return 0;
#else
    // 1) When target is on the same volume as source, must reject.
    wchar_t tmpBuf[MAX_PATH]{};
    DWORD n = GetTempPathW(MAX_PATH, tmpBuf);
    assert(n > 0);

    std::wstring tmpPath(tmpBuf);
    std::wstring drive = DriveLetterFromPath(tmpPath);
    assert(!drive.empty());

    Partition src{};
    src.driveLetter = drive;

    bool okSameVolume = RecoverEngine::ValidateTargetPath(tmpPath, src);
    assert(okSameVolume == false);

    // 2) When target volume cannot be verified, current policy is "allow".
    //    Use an invalid drive to force GetVolumePathNameW to fail.
    bool okUnverifiable = RecoverEngine::ValidateTargetPath(L"Z:\\this-drive-should-not-exist\\", src);
    assert(okUnverifiable == true);

    return 0;
#endif
}

