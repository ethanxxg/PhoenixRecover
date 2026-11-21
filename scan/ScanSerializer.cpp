#include "ScanSerializer.h"
#include "../log/Logger.h"
#include "../common/Utils.h"
#include <fstream>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <cwchar>

#if !(defined(__GNUC__) && __GNUC__ < 8)
#include <filesystem>
#endif

// std::basic_ofstream(std::wstring) is an MSVC extension; libstdc++ has no such
// overload.  C++17 std::filesystem::path is the portable, wide-character-safe
// form.  MinGW GCC 7 ships no <filesystem>, so it falls back to a narrow UTF-8
// path (ASCII-only session paths on that legacy toolchain).
namespace {
inline std::ofstream OpenSessionOutput(const std::wstring& path)
{
#if defined(__GNUC__) && __GNUC__ < 8
    return std::ofstream(Utils::WideToUtf8(path), std::ios::binary | std::ios::trunc);
#else
    return std::ofstream(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
#endif
}

inline std::ifstream OpenSessionInput(const std::wstring& path)
{
#if defined(__GNUC__) && __GNUC__ < 8
    return std::ifstream(Utils::WideToUtf8(path), std::ios::binary);
#else
    return std::ifstream(std::filesystem::path(path), std::ios::binary);
#endif
}
} // namespace

// ─── Low-level helpers ────────────────────────────────────────────────────────

namespace {

// ── Write primitives ─────────────────────────────────────────────────────────
void WriteU8 (std::ostream& os, uint8_t  v) { os.write(reinterpret_cast<const char*>(&v), 1); }
void WriteU16(std::ostream& os, uint16_t v) { os.write(reinterpret_cast<const char*>(&v), 2); }
void WriteU32(std::ostream& os, uint32_t v) { os.write(reinterpret_cast<const char*>(&v), 4); }
void WriteU64(std::ostream& os, uint64_t v) { os.write(reinterpret_cast<const char*>(&v), 8); }

// UTF-16 string: 4-byte char count + raw UTF-16 bytes
void WriteStr(std::ostream& os, const std::wstring& s)
{
    uint32_t len = static_cast<uint32_t>(s.size());
    WriteU32(os, len);
    if (len > 0)
        os.write(reinterpret_cast<const char*>(s.data()),
                 static_cast<std::streamsize>(len) * sizeof(wchar_t));
}

// ── Read primitives ──────────────────────────────────────────────────────────
bool ReadU8 (std::istream& is, uint8_t&  v) { return !!is.read(reinterpret_cast<char*>(&v), 1); }
bool ReadU16(std::istream& is, uint16_t& v) { return !!is.read(reinterpret_cast<char*>(&v), 2); }
bool ReadU32(std::istream& is, uint32_t& v) { return !!is.read(reinterpret_cast<char*>(&v), 4); }
bool ReadU64(std::istream& is, uint64_t& v) { return !!is.read(reinterpret_cast<char*>(&v), 8); }

bool ReadStr(std::istream& is, std::wstring& s)
{
    uint32_t len = 0;
    if (!ReadU32(is, len)) return false;
    if (len == 0) { s.clear(); return true; }
    // Sanity cap: 32767 chars (~65 KB per string) is more than enough for any path
    if (len > 32767) return false;
    s.resize(len);
    // 注意：在较旧的 C++ 标准下 std::wstring::data() 可能返回 const wchar_t*，
    // 这里用 &s[0] 获取可写缓冲区，避免 reinterpret_cast 到 char* 的 constness 问题。
    return !!is.read(reinterpret_cast<char*>(&s[0]),
                     static_cast<std::streamsize>(len) * sizeof(wchar_t));
}

// ── Partition ─────────────────────────────────────────────────────────────────
void WritePartition(std::ostream& os, const Partition& p)
{
    WriteU32(os, static_cast<uint32_t>(p.physicalDiskIdx));
    WriteU64(os, p.startSector);
    WriteU64(os, p.totalSectors);
    WriteU64(os, p.sizeBytes);
    WriteStr(os, p.fsType);
    WriteStr(os, p.driveLetter);
    WriteStr(os, p.label);
}

bool ReadPartition(std::istream& is, Partition& p)
{
    uint32_t diskIdx = 0;
    if (!ReadU32(is, diskIdx))       return false;
    p.physicalDiskIdx = static_cast<int>(diskIdx);
    if (!ReadU64(is, p.startSector))  return false;
    if (!ReadU64(is, p.totalSectors)) return false;
    if (!ReadU64(is, p.sizeBytes))    return false;
    if (!ReadStr(is, p.fsType))       return false;
    if (!ReadStr(is, p.driveLetter))  return false;
    if (!ReadStr(is, p.label))        return false;
    return true;
}

// ── FileNode ──────────────────────────────────────────────────────────────────
void WriteNode(std::ostream& os, const FileNode& n)
{
    WriteU64(os, n.id);
    WriteStr(os, n.name);
    WriteStr(os, n.originalPath);
    WriteStr(os, n.extension);
    WriteU64(os, n.size);
    WriteU64(os, n.createTime);
    WriteU64(os, n.modifyTime);
    WriteU64(os, n.accessTime);
    WriteU8 (os, static_cast<uint8_t>(n.status));
    WriteU8 (os, static_cast<uint8_t>(n.recoverability));
    WriteU8 (os, n.isDirectory      ? 1u : 0u);
    WriteU8 (os, n.isNtfsCompressed ? 1u : 0u);
    WriteU32(os, n.compressionUnit);
    WriteStr(os, n.fsType);
    WriteU64(os, n.parentId);
    WriteU64(os, n.rawOffset);
    WriteU64(os, n.rawSize);

    // Cluster list
    uint32_t clusterCount = static_cast<uint32_t>(
        std::min(n.clusters.size(), static_cast<size_t>(UINT32_MAX)));
    WriteU32(os, clusterCount);
    if (clusterCount > 0)
        os.write(reinterpret_cast<const char*>(n.clusters.data()),
                 static_cast<std::streamsize>(clusterCount) * sizeof(uint64_t));

    // Resident data
    uint32_t residentLen = static_cast<uint32_t>(
        std::min(n.residentData.size(), static_cast<size_t>(UINT32_MAX)));
    WriteU32(os, residentLen);
    if (residentLen > 0)
        os.write(reinterpret_cast<const char*>(n.residentData.data()),
                 residentLen);
}

bool ReadNode(std::istream& is, FileNode& n)
{
    if (!ReadU64(is, n.id))              return false;
    if (!ReadStr(is, n.name))            return false;
    if (!ReadStr(is, n.originalPath))    return false;
    if (!ReadStr(is, n.extension))       return false;
    if (!ReadU64(is, n.size))            return false;
    if (!ReadU64(is, n.createTime))      return false;
    if (!ReadU64(is, n.modifyTime))      return false;
    if (!ReadU64(is, n.accessTime))      return false;

    uint8_t status = 0, recov = 0, isDir = 0, isComp = 0;
    if (!ReadU8(is, status))  return false;
    if (!ReadU8(is, recov))   return false;
    if (!ReadU8(is, isDir))   return false;
    if (!ReadU8(is, isComp))  return false;
    n.status          = static_cast<FileStatus>(status);
    n.recoverability  = static_cast<RecoverabilityLevel>(recov);
    n.isDirectory     = (isDir  != 0);
    n.isNtfsCompressed= (isComp != 0);

    if (!ReadU32(is, n.compressionUnit)) return false;
    if (!ReadStr(is, n.fsType))          return false;
    if (!ReadU64(is, n.parentId))        return false;
    if (!ReadU64(is, n.rawOffset))       return false;
    if (!ReadU64(is, n.rawSize))         return false;

    // Clusters
    uint32_t clusterCount = 0;
    if (!ReadU32(is, clusterCount)) return false;
    if (clusterCount > 0) {
        // Sanity cap: 64M clusters max (~512 GB at 8 KB clusters)
        if (clusterCount > 64u * 1024 * 1024) return false;
        n.clusters.resize(clusterCount);
        if (!is.read(reinterpret_cast<char*>(&n.clusters[0]),
                     static_cast<std::streamsize>(clusterCount) * sizeof(uint64_t)))
            return false;
    }

    // Resident data
    uint32_t resLen = 0;
    if (!ReadU32(is, resLen)) return false;
    if (resLen > 0) {
        if (resLen > 4 * 1024 * 1024) return false;  // 4 MB cap
        n.residentData.resize(resLen);
        if (!is.read(reinterpret_cast<char*>(&n.residentData[0]), resLen))
            return false;
    }

    return true;
}

} // anonymous namespace

// ─────────────────────────────────────────────────────────────────────────────
// Save
// ─────────────────────────────────────────────────────────────────────────────
bool ScanSerializer::Save(const std::wstring& filePath,
                           const ScanResult&   result,
                           const Partition&    partition)
{
    std::ofstream ofs = OpenSessionOutput(filePath);
    if (!ofs) {
        LOG_ERROR(L"ScanSerializer::Save – cannot open file: " + filePath);
        return false;
    }

    // ── Header ────────────────────────────────────────────────────────────
    ofs.write("PHRS", 4);
    WriteU16(ofs, kVersion);
    WriteU16(ofs, 0);  // reserved

    // ── Partition ─────────────────────────────────────────────────────────
    WritePartition(ofs, partition);

    // ── ScanResult metadata ───────────────────────────────────────────────
    WriteU64(ofs, result.scannedBytes);
    WriteU32(ofs, result.foundCount);
    WriteU32(ofs, result.totalRecords);
    WriteU32(ofs, result.inUseCount);
    WriteU64(ofs, static_cast<uint64_t>(result.duration.count()));

    // ── File nodes ────────────────────────────────────────────────────────
    uint32_t count = static_cast<uint32_t>(result.files.size());
    WriteU32(ofs, count);
    for (const auto& node : result.files)
        WriteNode(ofs, node);

    bool ok = ofs.good();
    ofs.close();

    if (ok) {
        LOG_INFO(L"ScanSerializer::Save ok – " +
                 std::to_wstring(count) + L" files → " + filePath);
    } else {
        LOG_ERROR(L"ScanSerializer::Save – write error on " + filePath);
    }
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// Load
// ─────────────────────────────────────────────────────────────────────────────
bool ScanSerializer::Load(const std::wstring& filePath,
                           ScanResult&         outResult,
                           Partition&          outPartition)
{
    std::ifstream ifs = OpenSessionInput(filePath);
    if (!ifs) {
        LOG_ERROR(L"ScanSerializer::Load – cannot open file: " + filePath);
        return false;
    }

    // ── Header ────────────────────────────────────────────────────────────
    char magic[4]{};
    ifs.read(magic, 4);
    if (memcmp(magic, "PHRS", 4) != 0) {
        LOG_ERROR(L"ScanSerializer::Load – invalid magic");
        return false;
    }
    uint16_t version = 0, reserved = 0;
    if (!ReadU16(ifs, version) || !ReadU16(ifs, reserved)) return false;
    if (version != kVersion) {
        LOG_ERROR(L"ScanSerializer::Load – unsupported version " +
                  std::to_wstring(version));
        return false;
    }

    // ── Partition ─────────────────────────────────────────────────────────
    if (!ReadPartition(ifs, outPartition)) {
        LOG_ERROR(L"ScanSerializer::Load – error reading partition");
        return false;
    }

    // ── ScanResult metadata ───────────────────────────────────────────────
    outResult = ScanResult{};
    uint64_t durationMs = 0;
    if (!ReadU64(ifs, outResult.scannedBytes))  return false;
    if (!ReadU32(ifs, outResult.foundCount))    return false;
    if (!ReadU32(ifs, outResult.totalRecords))  return false;
    if (!ReadU32(ifs, outResult.inUseCount))    return false;
    if (!ReadU64(ifs, durationMs))              return false;
    outResult.duration = std::chrono::milliseconds(static_cast<int64_t>(durationMs));

    // ── File nodes ────────────────────────────────────────────────────────
    uint32_t count = 0;
    if (!ReadU32(ifs, count)) return false;

    // Sanity cap: 20 million files
    if (count > 20'000'000u) {
        LOG_ERROR(L"ScanSerializer::Load – file count too large: " +
                  std::to_wstring(count));
        return false;
    }

    outResult.files.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        FileNode node;
        if (!ReadNode(ifs, node)) {
            LOG_ERROR(L"ScanSerializer::Load – error reading node " +
                      std::to_wstring(i));
            return false;
        }
        outResult.files.push_back(std::move(node));
    }

    LOG_INFO(L"ScanSerializer::Load ok – " +
             std::to_wstring(count) + L" files from " + filePath);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// SuggestFilename
// ─────────────────────────────────────────────────────────────────────────────
std::wstring ScanSerializer::SuggestFilename(const Partition& part)
{
    // Prefix: drive letter without colon, or "Disk" + index
    std::wstring prefix;
    if (!part.driveLetter.empty() && part.driveLetter[0] != L'\0')
        prefix = std::wstring(1, part.driveLetter[0]);
    else
        prefix = L"Disk" + std::to_wstring(part.physicalDiskIdx);

    // FS type
    std::wstring fs = part.fsType.empty() ? L"UNK" : part.fsType;
    // Strip non-alphanumeric chars from fs string for safe filename
    std::wstring fsSafe;
    for (wchar_t c : fs)
        if (iswalnum(c)) fsSafe += c;
    if (fsSafe.empty()) fsSafe = L"UNK";

    // Timestamp.  Keep this serializer platform-neutral; the previous code
    // used Win32 SYSTEMTIME without including Windows.h, which broke the
    // normal GUI build and made this otherwise portable module Windows-only.
    std::time_t now = std::time(nullptr);
    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    wchar_t ts[24]{};
    std::swprintf(ts, sizeof(ts) / sizeof(ts[0]),
                  L"%04d%02d%02d_%02d%02d%02d",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec);

    return prefix + L"_" + fsSafe + L"_" + ts + L".prs";
}
