#pragma once

#include "../../common/Platform.h"
#include "../../common/Utils.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#if PHOENIX_ENABLE_VOLUME_IO

namespace PhoenixTestIo {

inline std::wstring TempDir(const wchar_t* tag)
{
    return Utils::GetTempDirectoryW() + L"PhoenixRecover_" + tag + L"_" +
           std::to_wstring(
               static_cast<unsigned long long>(
                   std::chrono::steady_clock::now().time_since_epoch().count())
           ) + L"/";
}

inline bool WriteFileBytes(const std::wstring& path, const void* data, size_t size)
{
    const std::string utf8 = Utils::WideToUtf8(path);
    std::ofstream f(utf8, std::ios::binary | std::ios::trunc);
    if (!f)
        return false;
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return f.good();
}

inline uint64_t FileSizeOnDisk(const std::wstring& path)
{
    std::error_code ec;
    const auto sz = std::filesystem::file_size(std::filesystem::path(path), ec);
    return ec ? 0 : static_cast<uint64_t>(sz);
}

inline void RemoveTree(const std::wstring& root)
{
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(root), ec);
}

} // namespace PhoenixTestIo

#endif
