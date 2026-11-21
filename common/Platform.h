#pragma once

// Platform detection — prefer compiler-defined macros so that pure-C++ code
// (unit tests, libraries) does not need to link against Qt.
// When Qt IS available, its Q_OS_* macros are also valid and consistent.

#if defined(_WIN32) || defined(Q_OS_WIN)
#define PHOENIX_OS_WINDOWS 1
#else
#define PHOENIX_OS_WINDOWS 0
#endif

#if (defined(__linux__) && !defined(__ANDROID__)) || \
    (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID))
#define PHOENIX_OS_LINUX_DESKTOP 1
#else
#define PHOENIX_OS_LINUX_DESKTOP 0
#endif

#if defined(__APPLE__) || defined(Q_OS_MACOS) || defined(Q_OS_MAC)
#define PHOENIX_OS_MACOS 1
#else
#define PHOENIX_OS_MACOS 0
#endif

// Disk images, partition probe, FS parsers, recover engine (non-GUI tests + core app).
#if PHOENIX_OS_WINDOWS || PHOENIX_OS_LINUX_DESKTOP || PHOENIX_OS_MACOS
#define PHOENIX_ENABLE_VOLUME_IO 1
#else
#define PHOENIX_ENABLE_VOLUME_IO 0
#endif

// Linux 产品范围：仅 ext2 / ext3 / ext4（共用 Ext4Parser）；不启用 FAT/NTFS/exFAT。
#ifndef PHOENIX_LINUX_EXT_FS_ONLY
#if PHOENIX_OS_LINUX_DESKTOP
#define PHOENIX_LINUX_EXT_FS_ONLY 1
#else
#define PHOENIX_LINUX_EXT_FS_ONLY 0
#endif
#endif

// CTest 在 Windows 上验证 Linux ext 代码路径时使用（见 test_dfr_linux_batch）。
#if !defined(PHOENIX_TEST_LINUX_EXT_ONLY)
#define PHOENIX_TEST_LINUX_EXT_ONLY 0
#endif

#if PHOENIX_LINUX_EXT_FS_ONLY || PHOENIX_TEST_LINUX_EXT_ONLY
#define PHOENIX_EXT_FS_CODEPATH 1
#else
#define PHOENIX_EXT_FS_CODEPATH 0
#endif
