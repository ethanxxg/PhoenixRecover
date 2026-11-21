#pragma once

#include "Partition.h"
#include <vector>
#include <string>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// SmartHealth – key S.M.A.R.T. health indicators for a physical disk.
// Queried via IOCTL_STORAGE_PREDICT_FAILURE (works for ATA and most NVMe).
// ─────────────────────────────────────────────────────────────────────────────
struct SmartHealth
{
    bool     available      = false;  // false → SMART not accessible
    bool     predictFailure = false;  // drive reports imminent failure

    // Raw values of critical SMART attributes (0 = not found / unknown)
    uint32_t reallocSectors = 0;  // 0x05 Reallocated Sector Count
    uint32_t pendingSectors = 0;  // 0xC5 Current Pending Sector Count
    uint32_t uncorrectable  = 0;  // 0xC6 (Off-line) Uncorrectable Sector Count
    uint32_t powerOnHours   = 0;  // 0x09 Power-On Hours
    uint32_t temperature    = 0;  // 0xBE / 0xC2 Temperature (°C)

    // Composite health label computed from the above values
    // "良好" / "注意" / "危险" / "未知"
    std::wstring statusText = L"未知";

    // 0-100 health score (100 = perfect)
    int healthScore = -1;

    // Human-readable tooltip / detail string
    std::wstring detail;
};

struct PhysicalDisk
{
    int          index         = 0;     // \\.\PhysicalDriveN  index
    std::wstring devicePath;            // e.g.  \\.\PhysicalDrive0
    std::wstring model;
    std::wstring serialNumber;
    std::wstring interfaceType;         // "SATA" | "NVMe" | "USB" | "Unknown"

    uint64_t     totalSectors  = 0;
    uint32_t     sectorSize    = 512;
    uint64_t     sizeBytes     = 0;

    bool         isGPT         = false;
    bool         isRemovable   = false;

    std::vector<Partition> partitions;

    SmartHealth  smart;  // populated by DiskManager::GetDisks()

    std::wstring DisplayName() const
    {
        return L"Disk " + std::to_wstring(index) +
               L"  " + model +
               L"  [" + interfaceType + L']';
    }
};
