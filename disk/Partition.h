#pragma once

#include <string>
#include <cstdint>

struct Partition
{
    int          index            = 0;
    int          physicalDiskIdx  = 0;  // Parent PhysicalDisk::index

    uint64_t     startSector      = 0;  // Absolute sector offset on physical disk
    uint64_t     totalSectors     = 0;
    uint64_t     sizeBytes        = 0;

    std::wstring fsType;                // "NTFS" | "FAT32" | "FAT16" | "exFAT" | "RAW" | "Unknown"
    std::wstring driveLetter;           // "C:" – empty when not mounted
    std::wstring label;                 // Volume label

    bool         isActive         = false;  // MBR bootable flag
    bool         isSystem         = false;  // EFI / system partition

    std::wstring DisplayName() const
    {
        std::wstring n;
        if (!driveLetter.empty()) n += driveLetter + L" ";
        if (!label.empty())       n += L'"' + label + L"\" ";
        n += fsType;
        return n;
    }
};
