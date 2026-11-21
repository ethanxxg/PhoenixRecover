#pragma once

#include <string>
#include <vector>
#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
// S.M.A.R.T Attribute
// ─────────────────────────────────────────────────────────────────────────────
struct SmartAttribute
{
    uint8_t     id              = 0;
    std::wstring name;
    uint8_t     current         = 0;   // Current value (0-253, higher is better)
    uint8_t     worst           = 0;   // Worst value ever recorded
    uint8_t     threshold       = 0;   // Threshold for failure
    uint64_t    rawValue        = 0;   // Raw data (12 bytes, typically 6 used)
    bool        preFail         = false; // True if attribute predicts failure
    bool        belowThreshold  = false; // True if current < threshold (failing)
};

// ─────────────────────────────────────────────────────────────────────────────
// Overall disk health status
// ─────────────────────────────────────────────────────────────────────────────
enum class DiskHealthStatus
{
    Unknown,        // Could not read S.M.A.R.T data
    Good,           // All attributes OK
    Warning,        // Some attributes degraded but not critical
    Critical,       // One or more attributes below threshold
    Failing         // Imminent failure predicted
};

// ─────────────────────────────────────────────────────────────────────────────
// S.M.A.R.T Information for a single disk
// ─────────────────────────────────────────────────────────────────────────────
struct SmartInfo
{
    int                 diskIndex       = -1;
    std::wstring        model;
    std::wstring        serial;
    std::wstring        firmware;
    bool                smartSupported  = false;
    bool                smartEnabled    = false;
    DiskHealthStatus    health          = DiskHealthStatus::Unknown;

    // Key attributes (commonly used)
    uint32_t            temperatureC    = 0;     // Temperature in Celsius
    uint32_t            powerOnHours    = 0;     // Total power-on hours
    uint32_t            powerCycleCount = 0;     // Power cycle count
    uint32_t            reallocatedSectors = 0;  // Reallocated sector count
    uint32_t            pendingSectors  = 0;     // Current pending sectors
    uint32_t            uncorrectableSectors = 0; // Uncorrectable sector count

    // All attributes
    std::vector<SmartAttribute> attributes;
};

// ─────────────────────────────────────────────────────────────────────────────
// SmartReader – reads S.M.A.R.T data from physical disks
// ─────────────────────────────────────────────────────────────────────────────
class SmartReader
{
public:
    // Read S.M.A.R.T info for a specific disk (by index, e.g., 0 for PhysicalDrive0)
    static SmartInfo ReadDiskSmart(int diskIndex);

    // Read S.M.A.R.T info for all physical disks
    static std::vector<SmartInfo> ReadAllDisks();

    // Get a human-readable health status string
    static std::wstring HealthStatusText(DiskHealthStatus status);

    // Get a color for the health status (for UI)
    static uint32_t HealthStatusColor(DiskHealthStatus status);

private:
    // Get attribute name by ID
    static std::wstring GetAttributeName(uint8_t id);
};
