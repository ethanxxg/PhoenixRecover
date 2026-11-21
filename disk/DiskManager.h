#pragma once

#include "PhysicalDisk.h"
#include "../common/Platform.h"
#include <vector>
#include <string>

#if PHOENIX_OS_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

class DiskManager
{
public:
    // Enumerate all physical disks visible to the system.
    // Requires elevated privileges; returns empty list on access denied.
    static std::vector<PhysicalDisk> GetDisks();

    // Treat a raw image file as a single-partition "disk".
    static bool LoadImageFile(const std::wstring& imagePath, PhysicalDisk& outDisk);

private:
#if PHOENIX_OS_WINDOWS
    static std::wstring DetectFsType(HANDLE hDisk, uint64_t startSector,
                                     uint32_t sectorSize);
    static void AssignDriveLetters(std::vector<PhysicalDisk>& disks);
    static void QuerySmart(HANDLE hDisk, PhysicalDisk& disk);
#endif
};
