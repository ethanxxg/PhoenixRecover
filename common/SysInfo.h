#pragma once
#include <string>
#include <cstdint>

// 收集本机系统信息，供 Header 条显示
struct SysInfo
{
    std::wstring osName;        // e.g. "Windows 10 Pro 64-bit"
    bool         isAdmin = false;
    std::wstring cpuName;       // e.g. "Intel Core i7-1185G7 @ 3.00GHz"
    uint32_t     ramMB   = 0;   // 物理内存（MB）
    std::wstring gpuName;       // e.g. "Intel Iris Xe Graphics"

    // 一次性收集所有信息
    static SysInfo Collect();

    // 格式化为 Header 三行文本
    std::wstring Line1() const; // "Phoenix Recover  v1.0.0 (64-bit)"
    std::wstring Line2() const; // "Windows 10 Pro 64-bit (Admin)"
    std::wstring Line3() const; // "Intel Core i7 @ 3.00GHz, 16 GB RAM, ..."
};
