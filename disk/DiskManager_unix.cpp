#include "../common/Platform.h"
#if PHOENIX_OS_LINUX_DESKTOP || PHOENIX_OS_MACOS

#include "DiskManager.h"
#include "../log/Logger.h"
#include "../common/Utils.h"

#include <vector>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#if PHOENIX_OS_MACOS
#include <sys/ioctl.h>
#include <sys/disk.h>
#endif

namespace {

std::string WideToNarrow(const std::wstring& ws)
{
    std::string out;
    out.reserve(ws.size());
    for (wchar_t wc : ws) {
        if (wc < 0x80)
            out.push_back(static_cast<char>(wc));
        else
            out.push_back('?');
    }
    return out;
}

std::wstring NarrowToWide(const std::string& s)
{
    return std::wstring(s.begin(), s.end());
}

std::string ReadSysfsString(const std::string& path)
{
    std::ifstream f(path);
    std::string line;
    if (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
            line.pop_back();
    }
    return line;
}

uint64_t ReadSysfsUint64(const std::string& path)
{
    std::string s = ReadSysfsString(path);
    if (s.empty()) return 0;
    try { return std::stoull(s); }
    catch (...) { return 0; }
}

// Detect FS type by reading the boot sector from an open fd
std::wstring DetectFsTypeFromFd(int fd, uint64_t startSector, uint32_t sectorSize)
{
    if (fd < 0 || sectorSize == 0) return L"Unknown";

    std::vector<uint8_t> sector(sectorSize, 0);
    ssize_t rd = ::pread(fd, sector.data(), sectorSize,
                         static_cast<off_t>(startSector) * sectorSize);
    if (rd < 11) return L"Unknown";

    if (memcmp(sector.data() + 3, "NTFS    ", 8) == 0) return L"NTFS";
    if (memcmp(sector.data() + 3, "EXFAT   ", 8) == 0) return L"exFAT";

    if (rd >= 90 && memcmp(sector.data() + 82, "FAT32   ", 8) == 0)
        return L"FAT32";

    if (rd >= 62) {
        if (memcmp(sector.data() + 54, "FAT16   ", 8) == 0) return L"FAT16";
        if (memcmp(sector.data() + 54, "FAT12   ", 8) == 0) return L"FAT12";
        if (memcmp(sector.data() + 54, "FAT     ", 8) == 0) return L"FAT16";
    }

    return L"Unknown";
}

} // anonymous namespace

// ── DiskManager::GetDisks ───────────────────────────────────────────────────
std::vector<PhysicalDisk> DiskManager::GetDisks()
{
    std::vector<PhysicalDisk> disks;

#if PHOENIX_OS_LINUX_DESKTOP
    // Enumerate block devices from /sys/block/
    DIR* dir = opendir("/sys/block");
    if (!dir) {
        LOG_ERROR(L"DiskManager::GetDisks: cannot open /sys/block");
        return disks;
    }

    int diskIdx = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;

        // Filter: sd*, nvme*, hd*, vd* (skip loop, ram, dm, etc.)
        bool isBlockDev = false;
        if (name.rfind("sd", 0) == 0 || name.rfind("hd", 0) == 0 ||
            name.rfind("vd", 0) == 0 || name.rfind("nvme", 0) == 0) {
            // For nvme, only take nvmeXnY, not nvmeXnYpZ (partitions)
            if (name.rfind("nvme", 0) == 0 && name.find('p') != std::string::npos) {
                size_t npos = name.find('n', 4);
                if (npos != std::string::npos && name.find('p', npos + 1) != std::string::npos)
                    continue;
            }
            isBlockDev = true;
        }
        if (!isBlockDev) continue;

        std::string basePath = std::string("/sys/block/") + name;

        PhysicalDisk disk;
        disk.index      = diskIdx++;
        disk.devicePath = NarrowToWide("/dev/" + name);

        // Size in 512-byte sectors (from kernel)
        uint64_t sizeSectors = ReadSysfsUint64(basePath + "/size");
        uint64_t hwSectorSize = ReadSysfsUint64(basePath + "/queue/hw_sector_size");
        if (hwSectorSize == 0) hwSectorSize = 512;

        disk.sectorSize   = static_cast<uint32_t>(hwSectorSize);
        // /sys/block/*/size is always in 512-byte units regardless of hw sector size
        disk.sizeBytes    = sizeSectors * 512;
        disk.totalSectors = disk.sizeBytes / disk.sectorSize;

        // Model
        std::string model = ReadSysfsString(basePath + "/device/model");
        if (model.empty())
            model = ReadSysfsString(basePath + "/device/name");
        disk.model = NarrowToWide(model);

        // Vendor
        std::string vendor = ReadSysfsString(basePath + "/device/vendor");
        if (!vendor.empty())
            disk.model = NarrowToWide(vendor) + L" " + disk.model;

        // Removable
        disk.isRemovable = (ReadSysfsUint64(basePath + "/removable") != 0);

        // Interface type
        std::string rotational = ReadSysfsString(basePath + "/queue/rotational");
        if (name.rfind("nvme", 0) == 0)
            disk.interfaceType = L"NVMe";
        else if (rotational == "0")
            disk.interfaceType = L"SSD";
        else
            disk.interfaceType = L"HDD";

        // SMART not implemented for Linux v0
        disk.smart.available  = false;
        disk.smart.statusText = L"未知";

        disks.push_back(std::move(disk));
    }
    closedir(dir);

#elif PHOENIX_OS_MACOS
    // Basic macOS disk enumeration via /dev/disk*
    // Full IOKit enumeration deferred to future iteration
    DIR* dir = opendir("/dev");
    if (!dir) {
        LOG_ERROR(L"DiskManager::GetDisks: cannot open /dev");
        return disks;
    }

    int diskIdx = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        // Match "disk0", "disk1", etc. (not "disk0s1" partitions)
        if (name.rfind("disk", 0) != 0) continue;
        if (name.find('s') != std::string::npos) continue;
        // Skip rdisk (raw device), we'll use it for reading
        if (name.rfind("rdisk", 0) == 0) continue;

        bool allDigits = true;
        for (size_t i = 4; i < name.size(); ++i) {
            if (!isdigit(static_cast<unsigned char>(name[i]))) {
                allDigits = false;
                break;
            }
        }
        if (!allDigits || name.size() <= 4) continue;

        PhysicalDisk disk;
        disk.index      = diskIdx++;
        disk.devicePath = NarrowToWide("/dev/" + name);
        disk.model      = NarrowToWide(name);
        disk.sectorSize = 512;
        disk.interfaceType = L"Unknown";

        // Try to get size from raw device
        std::string rdiskPath = "/dev/r" + name;
        int fd = ::open(rdiskPath.c_str(), O_RDONLY);
        if (fd >= 0) {
            uint32_t blockSize = 0;
            uint64_t blockCount = 0;
            if (ioctl(fd, DKIOCGETBLOCKSIZE, &blockSize) == 0 && blockSize > 0)
                disk.sectorSize = blockSize;
            if (ioctl(fd, DKIOCGETBLOCKCOUNT, &blockCount) == 0) {
                disk.totalSectors = blockCount;
                disk.sizeBytes = blockCount * disk.sectorSize;
            }
            ::close(fd);
        }

        disk.smart.available  = false;
        disk.smart.statusText = L"未知";

        disks.push_back(std::move(disk));
    }
    closedir(dir);
#endif

    return disks;
}

// ── LoadImageFile ─────────────────────────────────────────────────────────────
bool DiskManager::LoadImageFile(const std::wstring& imagePath, PhysicalDisk& outDisk)
{
    std::string path = WideToNarrow(imagePath);
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        LOG_ERROR(L"LoadImageFile: cannot open " + imagePath);
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        ::close(fd);
        LOG_ERROR(L"LoadImageFile: cannot stat " + imagePath);
        return false;
    }

    PhysicalDisk disk;
    disk.index         = -1;
    disk.devicePath    = imagePath;
    disk.sectorSize    = 512;
    disk.sizeBytes     = static_cast<uint64_t>(st.st_size);
    disk.totalSectors  = disk.sizeBytes / disk.sectorSize;
    disk.interfaceType = L"Image";

    // Extract filename as model
    size_t pos = imagePath.find_last_of(L"\\/");
    disk.model = (pos != std::wstring::npos)
                 ? imagePath.substr(pos + 1)
                 : imagePath;

    // Try to parse the partition table from the image
    std::vector<uint8_t> sector0(512, 0);
    ssize_t rd = ::pread(fd, sector0.data(), 512, 0);

    if (rd >= 512 && sector0[510] == 0x55 && sector0[511] == 0xAA) {
        auto* mbrParts = reinterpret_cast<uint8_t*>(sector0.data() + 446);
        bool hasPartitions = false;
        for (int i = 0; i < 4; ++i) {
            uint8_t type = mbrParts[i * 16 + 4];
            if (type == 0) continue;

            uint32_t startLBA = Utils::ReadLE32(mbrParts + i * 16 + 8);
            uint32_t sizeLBA  = Utils::ReadLE32(mbrParts + i * 16 + 12);
            if (sizeLBA == 0) continue;

            Partition part;
            part.index        = static_cast<int>(disk.partitions.size());
            part.physicalDiskIdx = -1;
            part.startSector  = startLBA;
            part.totalSectors = sizeLBA;
            part.sizeBytes    = static_cast<uint64_t>(sizeLBA) * disk.sectorSize;
            part.isActive     = (mbrParts[i * 16] & 0x80) != 0;
            part.fsType       = DetectFsTypeFromFd(fd, startLBA, disk.sectorSize);
            disk.partitions.push_back(part);
            hasPartitions = true;
        }

        if (!hasPartitions) {
            Partition p;
            p.index        = 0;
            p.physicalDiskIdx = -1;
            p.startSector  = 0;
            p.totalSectors = disk.totalSectors;
            p.sizeBytes    = disk.sizeBytes;
            p.fsType       = DetectFsTypeFromFd(fd, 0, disk.sectorSize);
            disk.partitions.push_back(p);
        }
    } else {
        Partition p;
        p.index        = 0;
        p.physicalDiskIdx = -1;
        p.startSector  = 0;
        p.totalSectors = disk.totalSectors;
        p.sizeBytes    = disk.sizeBytes;
        p.fsType       = L"Unknown";
        disk.partitions.push_back(p);
    }

    ::close(fd);
    outDisk = std::move(disk);
    return true;
}

#endif // PHOENIX_OS_LINUX_DESKTOP || PHOENIX_OS_MACOS
