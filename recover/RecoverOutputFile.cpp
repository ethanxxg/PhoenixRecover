#include "RecoverOutputFile.h"

#include "../common/Platform.h"
#include "../common/Utils.h"

#if PHOENIX_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <cstring>

RecoverOutputFile::~RecoverOutputFile()
{
    Close();
}

bool RecoverOutputFile::IsOpen() const
{
#if PHOENIX_OS_WINDOWS
    return m_handle != reinterpret_cast<void*>(static_cast<intptr_t>(-1));
#else
    return m_fd >= 0;
#endif
}

bool RecoverOutputFile::Open(const std::wstring& path, std::wstring& outError)
{
    Close();
#if PHOENIX_OS_WINDOWS
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        outError = L"Cannot create file (error " + std::to_wstring(::GetLastError()) + L")";
        return false;
    }
    m_handle = h;
#else
    const std::string utf8 = Utils::WideToUtf8(path);
    m_fd = ::open(utf8.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (m_fd < 0) {
        outError = L"Cannot create file (errno " + std::to_wstring(errno) + L")";
        return false;
    }
#endif
    return true;
}

bool RecoverOutputFile::Write(const void* data, uint32_t size)
{
    if (!IsOpen() || size == 0)
        return true;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t left = size;
#if PHOENIX_OS_WINDOWS
    while (left > 0) {
        DWORD chunk = left;
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(m_handle), p, chunk, &written, nullptr) ||
            written == 0) {
            return false;
        }
        p += written;
        left -= written;
    }
#else
    while (left > 0) {
        const ssize_t n = ::write(m_fd, p, left);
        if (n <= 0)
            return false;
        p += static_cast<uint32_t>(n);
        left -= static_cast<uint32_t>(n);
    }
#endif
    return true;
}

void RecoverOutputFile::SetFileTimes(uint64_t createTime, uint64_t modifyTime, uint64_t accessTime)
{
    if (!IsOpen())
        return;
    if (createTime == 0 && modifyTime == 0 && accessTime == 0)
        return;
#if PHOENIX_OS_WINDOWS
    FILETIME ct{}, wt{}, at{};
    if (createTime) {
        ct.dwLowDateTime  = static_cast<DWORD>(createTime & 0xFFFFFFFF);
        ct.dwHighDateTime = static_cast<DWORD>(createTime >> 32);
    }
    if (modifyTime) {
        wt.dwLowDateTime  = static_cast<DWORD>(modifyTime & 0xFFFFFFFF);
        wt.dwHighDateTime = static_cast<DWORD>(modifyTime >> 32);
    }
    if (accessTime) {
        at.dwLowDateTime  = static_cast<DWORD>(accessTime & 0xFFFFFFFF);
        at.dwHighDateTime = static_cast<DWORD>(accessTime >> 32);
    }
    SetFileTime(static_cast<HANDLE>(m_handle), createTime ? &ct : nullptr,
                accessTime ? &at : nullptr, modifyTime ? &wt : nullptr);
#else
    (void)createTime;
    (void)modifyTime;
    (void)accessTime;
#endif
}

void RecoverOutputFile::Close()
{
#if PHOENIX_OS_WINDOWS
    if (IsOpen()) {
        CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }
#else
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
#endif
}
