#include "Imager.h"
#include "../common/Platform.h"

#if PHOENIX_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>
#include <bcrypt.h>
#endif
#include "../log/Logger.h"
#include <vector>
#include <algorithm>
#include <string>
#include <cstring>
#include <cstdio>

#if PHOENIX_OS_WINDOWS

#pragma comment(lib, "bcrypt.lib")

static std::wstring MakePhysPath(int idx)
{
    return L"\\\\.\\PhysicalDrive" + std::to_wstring(idx);
}

ImageOutputSafety Imager::CheckOutputPath(
    int srcDiskIndex, const std::wstring& outImgPath)
{
    if (srcDiskIndex < 0 || outImgPath.empty())
        return ImageOutputSafety::Unknown;

    wchar_t volumePath[32768]{};
    if (!::GetVolumePathNameW(outImgPath.c_str(), volumePath,
                              static_cast<DWORD>(sizeof(volumePath) /
                                                 sizeof(volumePath[0]))))
        return ImageOutputSafety::Unknown;

    HANDLE hVolume = ::CreateFileW(
        volumePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hVolume == INVALID_HANDLE_VALUE)
        return ImageOutputSafety::Unknown;

    STORAGE_DEVICE_NUMBER deviceNumber{};
    DWORD returned = 0;
    const bool ok = ::DeviceIoControl(
        hVolume, IOCTL_STORAGE_GET_DEVICE_NUMBER,
        nullptr, 0, &deviceNumber, sizeof(deviceNumber), &returned, nullptr);
    ::CloseHandle(hVolume);
    if (!ok)
        return ImageOutputSafety::Unknown;

    return deviceNumber.DeviceNumber == static_cast<DWORD>(srcDiskIndex)
        ? ImageOutputSafety::SamePhysicalDisk
        : ImageOutputSafety::DifferentPhysicalDisk;
}

static bool ComputeFileSha256(const std::wstring& path,
                              std::wstring& outHex,
                              DWORD& outError)
{
    outHex.clear();
    outError = ERROR_SUCCESS;

    HANDLE hFile = ::CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                  nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        outError = ::GetLastError();
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<UCHAR> object;
    std::vector<UCHAR> digest;
    bool ok = false;

    do {
        NTSTATUS status = ::BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (status < 0) {
            outError = static_cast<DWORD>(status);
            break;
        }

        DWORD objectLength = 0;
        DWORD resultLength = 0;
        status = ::BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
            &resultLength, 0);
        if (status < 0 || objectLength == 0) {
            outError = static_cast<DWORD>(status);
            break;
        }

        DWORD hashLength = 0;
        status = ::BCryptGetProperty(
            algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashLength), sizeof(hashLength),
            &resultLength, 0);
        if (status < 0 || hashLength == 0) {
            outError = static_cast<DWORD>(status);
            break;
        }

        object.resize(objectLength);
        digest.resize(hashLength);
        status = ::BCryptCreateHash(
            algorithm, &hash, object.data(), objectLength,
            nullptr, 0, 0);
        if (status < 0) {
            outError = static_cast<DWORD>(status);
            break;
        }

        std::vector<UCHAR> buffer(4u * 1024u * 1024u);
        for (;;) {
            DWORD bytesRead = 0;
            if (!::ReadFile(hFile, buffer.data(),
                            static_cast<DWORD>(buffer.size()),
                            &bytesRead, nullptr)) {
                outError = ::GetLastError();
                break;
            }
            if (bytesRead == 0)
                break;
            status = ::BCryptHashData(hash, buffer.data(), bytesRead, 0);
            if (status < 0) {
                outError = static_cast<DWORD>(status);
                break;
            }
        }
        if (outError != ERROR_SUCCESS)
            break;

        status = ::BCryptFinishHash(hash, digest.data(), hashLength, 0);
        if (status < 0) {
            outError = static_cast<DWORD>(status);
            break;
        }

        static constexpr wchar_t kHex[] = L"0123456789abcdef";
        outHex.reserve(hashLength * 2);
        for (UCHAR byte : digest) {
            outHex.push_back(kHex[(byte >> 4) & 0x0F]);
            outHex.push_back(kHex[byte & 0x0F]);
        }
        ok = true;
    } while (false);

    if (hash)
        ::BCryptDestroyHash(hash);
    if (algorithm)
        ::BCryptCloseAlgorithmProvider(algorithm, 0);
    ::CloseHandle(hFile);
    return ok;
}

static bool IsValidMarker(const char* data, size_t length,
                          const std::string& prefix,
                          std::wstring& outSha256)
{
    outSha256.clear();
    constexpr size_t kHashChars = 64;
    if (length != prefix.size() + kHashChars + 2 ||
        std::memcmp(data, prefix.data(), prefix.size()) != 0 ||
        data[prefix.size() + kHashChars] != '\r' ||
        data[prefix.size() + kHashChars + 1] != '\n')
        return false;

    for (size_t i = 0; i < kHashChars; ++i) {
        const char c = data[prefix.size() + i];
        const bool hex = (c >= '0' && c <= '9') ||
                         (c >= 'a' && c <= 'f') ||
                         (c >= 'A' && c <= 'F');
        if (!hex)
            return false;
        outSha256.push_back(static_cast<wchar_t>(c));
    }
    return true;
}

std::wstring Imager::WinErr(DWORD err)
{
    if (err == 0) return L"";
    wchar_t* msg = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = ::FormatMessageW(flags, nullptr, err, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring s = (len && msg) ? std::wstring(msg, msg + len) : L"";
    if (msg) ::LocalFree(msg);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    return s;
}

bool Imager::GetDiskSizeBytes(HANDLE hDisk, uint64_t& outSizeBytes)
{
    outSizeBytes = 0;
    GET_LENGTH_INFORMATION gli{};
    DWORD ret = 0;
    if (::DeviceIoControl(hDisk, IOCTL_DISK_GET_LENGTH_INFO,
                          nullptr, 0,
                          &gli, sizeof(gli),
                          &ret, nullptr))
    {
        outSizeBytes = static_cast<uint64_t>(gli.Length.QuadPart);
        return outSizeBytes > 0;
    }
    return false;
}

ImageResult Imager::CreatePhysicalDiskImage(
    int srcDiskIndex,
    const std::wstring& outImgPath,
    uint32_t blockBytes,
    const std::atomic<bool>* cancelFlag,
    std::function<void(const ImageProgress&)> onProgress)
{
    ImageResult res{};
    res.totalBytes = 0;

    if (srcDiskIndex < 0) {
        res.message = L"无效的物理磁盘索引";
        return res;
    }
    if (outImgPath.empty()) {
        res.message = L"输出路径为空";
        return res;
    }
    if (blockBytes < 64 * 1024) blockBytes = 64 * 1024;

    const std::wstring srcPath = MakePhysPath(srcDiskIndex);

    HANDLE hSrc = ::CreateFileW(srcPath.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr);
    if (hSrc == INVALID_HANDLE_VALUE) {
        res.lastError = ::GetLastError();
        res.message = L"打开源磁盘失败: " + srcPath + L"  (" + WinErr(res.lastError) + L")";
        return res;
    }

    uint64_t totalBytes = 0;
    if (!GetDiskSizeBytes(hSrc, totalBytes)) {
        res.lastError = ::GetLastError();
        ::CloseHandle(hSrc);
        res.message = L"获取磁盘大小失败: " + srcPath + L"  (" + WinErr(res.lastError) + L")";
        return res;
    }
    res.totalBytes = totalBytes;

    // ── 输出文件：支持简单断点续传 ─────────────────────────────────────────
    // 逻辑：
    //   1) 如果 .img 不存在 → 新建，预分配 totalBytes，从 0 开始写。
    //   2) 如果 .img 已存在：
    //        - 若大小 == totalBytes : 认为镜像已完整，直接返回成功。
    //        - 若 0 < 大小 < totalBytes : 从现有大小处继续写（简易续传）。
    //        - 其他情况（>totalBytes 或 0）: 覆盖重写。
    HANDLE hOut = INVALID_HANDLE_VALUE;
    uint64_t existingSize = 0;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (::GetFileAttributesExW(outImgPath.c_str(), GetFileExInfoStandard, &fad)) {
        LARGE_INTEGER sz{};
        sz.HighPart = fad.nFileSizeHigh;
        sz.LowPart  = fad.nFileSizeLow;
        existingSize = static_cast<uint64_t>(sz.QuadPart);
    }

    // A pre-existing full-size file is not enough to prove that imaging
    // completed: the old implementation preallocated the output file, so a
    // cancelled/failed run could also have the final size.  Require a small
    // completion marker written only after the last byte has been flushed.
    const std::wstring markerPath = outImgPath + L".phoenixcomplete";
    const std::string markerPrefix =
        std::string("PhoenixRecover image complete\r\nsource-disk=") +
        std::to_string(srcDiskIndex) + "\r\nsize=" +
        std::to_string(totalBytes) + "\r\nsha256=";

    bool hasCompletionMarker = false;
    std::wstring existingSha256;
    HANDLE hMarkerCheck = ::CreateFileW(markerPath.c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hMarkerCheck != INVALID_HANDLE_VALUE) {
        char markerBuf[256]{};
        DWORD markerRead = 0;
        if (::ReadFile(hMarkerCheck, markerBuf, sizeof(markerBuf) - 1,
                       &markerRead, nullptr)) {
            hasCompletionMarker = IsValidMarker(
                markerBuf, markerRead, markerPrefix, existingSha256);
        }
        ::CloseHandle(hMarkerCheck);
    }

    if (existingSize == totalBytes && existingSize > 0 && hasCompletionMarker) {
        // Image size and completion marker both match: skip safely.
        ::CloseHandle(hSrc);
        res.ok          = true;
        res.totalBytes  = totalBytes;
        res.writtenBytes= totalBytes;
        res.badBytes    = 0;
        res.sha256      = existingSha256;
        res.message     = L"目标镜像已存在且大小匹配，跳过创建。";
        return res;
    }

    // Resume only from a sector boundary.  An arbitrary partial-byte output
    // cannot be mapped safely back to a raw disk sector.
    bool resume = (existingSize > 0 && existingSize < totalBytes &&
                   (existingSize % 512u) == 0);

    // The marker is invalid as soon as we are going to rewrite or resume the
    // image.  Failure to delete it is not fatal yet; a partial file cannot be
    // accepted by the size+marker check above.
    if (hasCompletionMarker)
        ::DeleteFileW(markerPath.c_str());

    hOut = ::CreateFileW(outImgPath.c_str(),
                         GENERIC_WRITE,
                         0,
                         nullptr,
                         resume ? OPEN_EXISTING : CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                         nullptr);
    if (hOut == INVALID_HANDLE_VALUE) {
        res.lastError = ::GetLastError();
        ::CloseHandle(hSrc);
        res.message = L"打开/创建输出文件失败: " + outImgPath + L"  (" + WinErr(res.lastError) + L")";
        return res;
    }

    const std::wstring badLogPath = outImgPath + L".badsectors.log";
    if (!resume)
        ::DeleteFileW(badLogPath.c_str());
    HANDLE hBadLog = ::CreateFileW(
        badLogPath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hBadLog != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER end{};
        ::SetFilePointerEx(hBadLog, end, nullptr, FILE_END);
    }

    // Do not preallocate the output to totalBytes.  Its current length is the
    // resume checkpoint; preallocation would make a cancelled image look
    // complete on the next invocation.
    LARGE_INTEGER li{};
    if (!resume) {
        li.QuadPart = 0;
        ::SetFilePointerEx(hOut, li, nullptr, FILE_BEGIN);
    } else {
        // Resume exactly at the existing sector-aligned length.
        li.QuadPart = (LONGLONG)existingSize;
        ::SetFilePointerEx(hOut, li, nullptr, FILE_BEGIN);
    }

    ImageProgress prog{};
    prog.totalBytes = totalBytes;

    const uint32_t kMinBlock = 64 * 1024;
    const uint32_t kSector   = 512;

    uint64_t offset = resume ? existingSize : 0;
    uint64_t badBytes = 0;
    uint64_t written = 0;

    std::vector<uint8_t> buf;
    buf.resize(blockBytes);
    std::vector<uint8_t> zeroBuf;
    zeroBuf.resize(blockBytes, 0);

    auto report = [&]{
        if (onProgress) {
            prog.doneBytes = offset;
            prog.badBytes  = badBytes;
            onProgress(prog);
        }
    };

    LOG_INFO(L"Imager: CreatePhysicalDiskImage  src=" + srcPath +
             L"  out=" + outImgPath +
             L"  size=" + std::to_wstring(totalBytes));

    while (offset < totalBytes) {
        if (cancelFlag && cancelFlag->load()) {
            res.cancelled = true;
            break;
        }

        uint64_t remain = totalBytes - offset;
        uint32_t want = (uint32_t)(std::min<uint64_t>(remain, buf.size()));

        // Align down to 512 for reads; handle tail separately.
        uint32_t alignedWant = want - (want % kSector);
        if (alignedWant == 0) alignedWant = want; // last tiny tail, still try

        LARGE_INTEGER pos{}; pos.QuadPart = (LONGLONG)offset;
        ::SetFilePointerEx(hSrc, pos, nullptr, FILE_BEGIN);

        DWORD got = 0;
        bool okRead = false;
        DWORD lastReadError = ERROR_SUCCESS;
        uint32_t cur = alignedWant;
        int retries = 2;

        for (;;) {
            got = 0;
            if (::ReadFile(hSrc, buf.data(), cur, &got, nullptr) && got == cur) {
                okRead = true;
                break;
            }

            DWORD e = ::GetLastError();
            lastReadError = e;

            if (retries-- > 0) {
                ::Sleep(30);
                continue;
            }

            if (cur > kMinBlock) {
                cur = (std::max)(kMinBlock, cur / 2);
                retries = 2;
                continue;
            }

            // Give up this block; fill with zeros.
            okRead = false;
            got = cur;
            break;
        }

        DWORD toWrite = got;
        DWORD wrote = 0;

        if (!okRead) {
            // Ensure zeroBuf at least toWrite
            if (zeroBuf.size() < toWrite) zeroBuf.resize(toWrite, 0);
            if (!::WriteFile(hOut, zeroBuf.data(), toWrite, &wrote, nullptr) || wrote != toWrite) {
                res.lastError = ::GetLastError();
                res.message = L"写入输出文件失败（填0）: " + WinErr(res.lastError);
                break;
            }
            badBytes += toWrite;
            if (hBadLog != INVALID_HANDLE_VALUE) {
                const std::string line =
                    "offset=" + std::to_string(offset) +
                    ",bytes=" + std::to_string(toWrite) +
                    ",error=" + std::to_string(lastReadError) + "\r\n";
                DWORD logged = 0;
                ::WriteFile(hBadLog, line.data(),
                            static_cast<DWORD>(line.size()), &logged, nullptr);
            }
        } else {
            if (!::WriteFile(hOut, buf.data(), toWrite, &wrote, nullptr) || wrote != toWrite) {
                res.lastError = ::GetLastError();
                res.message = L"写入输出文件失败: " + WinErr(res.lastError);
                break;
            }
        }

        offset  += wrote;
        written += wrote;

        // Throttle progress callbacks a bit (every ~8 MiB)
        if ((offset % (8ull * 1024 * 1024)) < wrote) report();
    }

    // Final report
    prog.doneBytes = offset;
    prog.badBytes  = badBytes;
    if (onProgress) onProgress(prog);

    ::FlushFileBuffers(hOut);
    ::CloseHandle(hOut);
    ::CloseHandle(hSrc);
    if (hBadLog != INVALID_HANDLE_VALUE) {
        ::FlushFileBuffers(hBadLog);
        ::CloseHandle(hBadLog);
    }

    res.badBytes = badBytes;
    res.writtenBytes = offset;

    if (res.cancelled) {
        res.ok = false;
        if (res.message.empty()) res.message = L"已取消";
        return res;
    }

    if (!res.message.empty()) {
        res.ok = false;
        return res;
    }

    res.ok = (offset == totalBytes && written > 0);
    if (res.ok) {
        DWORD hashError = ERROR_SUCCESS;
        if (!ComputeFileSha256(outImgPath, res.sha256, hashError)) {
            res.ok = false;
            res.lastError = hashError;
            res.message = L"镜像已写完，但 SHA-256 校验失败: " +
                          WinErr(hashError);
        }
    }

    if (!res.ok) {
        if (res.message.empty())
            res.message = L"镜像创建未完成";
    } else if (badBytes > 0) {
        res.message = L"镜像完成，但存在读失败区域（已填0）";
    } else {
        res.message = L"镜像创建成功";
    }

    if (res.ok) {
        std::string hashAscii;
        hashAscii.reserve(res.sha256.size());
        for (wchar_t c : res.sha256)
            hashAscii.push_back(static_cast<char>(c));
        const std::string markerSignature =
            markerPrefix + hashAscii + "\r\n";
        HANDLE hMarker = ::CreateFileW(markerPath.c_str(), GENERIC_WRITE, 0,
                                       nullptr, CREATE_ALWAYS,
                                       FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL,
                                       nullptr);
        if (hMarker != INVALID_HANDLE_VALUE) {
            DWORD markerWritten = 0;
            const bool markerOk =
                ::WriteFile(hMarker, markerSignature.data(),
                            static_cast<DWORD>(markerSignature.size()),
                            &markerWritten, nullptr) &&
                markerWritten == markerSignature.size();
            ::FlushFileBuffers(hMarker);
            ::CloseHandle(hMarker);
            if (!markerOk)
                LOG_WARN(L"Imager: image completed but completion marker could not be written");
        } else {
            LOG_WARN(L"Imager: image completed but completion marker could not be created: "
                     + markerPath);
        }
    }
    return res;
}

#else

ImageResult Imager::CreatePhysicalDiskImage(int srcDiskIndex, const std::wstring& outImgPath,
                                            uint32_t blockBytes,
                                            const std::atomic<bool>* cancelFlag,
                                            std::function<void(const ImageProgress&)> onProgress)
{
    (void)srcDiskIndex;
    (void)outImgPath;
    (void)blockBytes;
    (void)cancelFlag;
    (void)onProgress;
    ImageResult res;
    res.ok      = false;
    res.message = L"Physical disk imaging is only available on Windows";
    return res;
}

ImageOutputSafety Imager::CheckOutputPath(
    int srcDiskIndex, const std::wstring& outImgPath)
{
    (void)srcDiskIndex;
    (void)outImgPath;
    return ImageOutputSafety::Unknown;
}

#endif
