#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <atomic>

struct ImageProgress
{
    uint64_t totalBytes = 0;
    uint64_t doneBytes  = 0;
    uint64_t badBytes   = 0;
};

struct ImageResult
{
    bool     ok          = false;
    bool     cancelled   = false;
    uint64_t totalBytes  = 0;
    uint64_t writtenBytes= 0;
    uint64_t badBytes    = 0;
    DWORD    lastError   = 0;
    std::wstring sha256;
    std::wstring message;
};

enum class ImageOutputSafety
{
    DifferentPhysicalDisk,
    SamePhysicalDisk,
    Unknown
};

class Imager
{
public:
    // Create a raw .img (sector-by-sector) from a physical drive.
    // srcDiskIndex: N for \\.\PhysicalDriveN
    // blockBytes: preferred sequential read size (e.g. 4 MiB)
    static ImageResult CreatePhysicalDiskImage(
        int srcDiskIndex,
        const std::wstring& outImgPath,
        uint32_t blockBytes,
        const std::atomic<bool>* cancelFlag,
        std::function<void(const ImageProgress&)> onProgress);

    // Resolve the volume containing outImgPath and compare its physical disk
    // number with srcDiskIndex. Unknown means the path could not be mapped.
    static ImageOutputSafety CheckOutputPath(
        int srcDiskIndex, const std::wstring& outImgPath);

private:
    static bool GetDiskSizeBytes(HANDLE hDisk, uint64_t& outSizeBytes);
    static std::wstring WinErr(DWORD err);
};

