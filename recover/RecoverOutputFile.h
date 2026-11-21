#pragma once

#include "../common/Platform.h"

#include <cstdint>
#include <string>

// Cross-platform write handle for RecoverEngine (Windows HANDLE / Unix fd).
class RecoverOutputFile
{
public:
    RecoverOutputFile() = default;
    ~RecoverOutputFile();

    RecoverOutputFile(const RecoverOutputFile&)            = delete;
    RecoverOutputFile& operator=(const RecoverOutputFile&) = delete;

    bool Open(const std::wstring& path, std::wstring& outError);
    bool Write(const void* data, uint32_t size);
    void SetFileTimes(uint64_t createTime, uint64_t modifyTime, uint64_t accessTime);
    void Close();

    bool IsOpen() const;

private:
#if PHOENIX_OS_WINDOWS
    void* m_handle = reinterpret_cast<void*>(static_cast<intptr_t>(-1)); // INVALID_HANDLE_VALUE
#else
    int m_fd = -1;
#endif
};
