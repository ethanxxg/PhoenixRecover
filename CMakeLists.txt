# PhoenixRecover 鈥?CMake 楠ㄦ灦锛屼笌 PhoenixRecover.vcxproj 骞惰銆?
#
# 璇存槑:
# - 濯掍綋瀛愮郴缁?(media/*) 宸叉寜 Q_OS_WIN 鍖哄垎锛汱inux/macOS 涓嬩负 QImage + FFmpeg 鍙墽琛屾枃浠剁瓑銆?
# - Linux 桌面端产品范围：ext2 / ext3 / ext4（Ext4Parser）；FAT/NTFS/exFAT 仅 Windows。
# - 非 Windows 平台已提供 SectorReader_unix、RecoverOutputFile 等 POSIX 实现。
#
# 鐢ㄦ硶绀轰緥 (浠呬綔鍚庣画瀵规帴):
#   mkdir build && cd build
#   cmake .. -DCMAKE_PREFIX_PATH=/path/to/Qt/5.14.2/gcc_64
#   cmake --build .
# 浠呭崟娴?鏃?Qt: cmake .. -DPHOENIX_BUILD_GUI=OFF

cmake_minimum_required(VERSION 3.16)
project(PhoenixRecover LANGUAGES CXX C)

option(PHOENIX_BUILD_GUI "Build Qt GUI (PhoenixRecover); OFF 鏃朵粎鏋勫缓 tests 绛夋棤 Qt 鐩爣" ON)
option(PHOENIX_HAVE_LIBZIP "Use libzip for optional ZIP archive validation after repair" OFF)
option(PHOENIX_BUILD_BENCH "Build standalone (non-CTest) RAW-scan benchmarks" OFF)

enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(PHOENIX_BUILD_GUI)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt5 5.14 REQUIRED COMPONENTS Core Gui Widgets)

if(WIN32)
    set(PHOENIX_PLATFORM_SOURCES
        disk/SectorReader_win.cpp
        disk/DiskManager_win.cpp
    )
else()
    set(PHOENIX_PLATFORM_SOURCES
        disk/SectorReader_unix.cpp
        disk/DiskManager_unix.cpp
    )
endif()

set(PHOENIX_FS_EXT_SOURCES
    fs/ext4/Ext4Parser.cpp
)
set(PHOENIX_FS_WIN_SOURCES
    fs/exfat/ExfatParser.cpp
    fs/fat/FatParser.cpp
    fs/fat/FatStructs.cpp
    fs/ntfs/NtfsParser.cpp
    fs/ntfs/UsnJournalParser.cpp
)
set(PHOENIX_RECOVER_WIN_SOURCES
    recover/FatFragmentStitcher.cpp
    recover/NtfsFragmentStitcher.cpp
)

if(WIN32)
    set(PHOENIX_FS_SOURCES ${PHOENIX_FS_EXT_SOURCES} ${PHOENIX_FS_WIN_SOURCES})
    set(PHOENIX_RECOVER_FRAGMENT_SOURCES ${PHOENIX_RECOVER_WIN_SOURCES})
else()
    set(PHOENIX_FS_SOURCES ${PHOENIX_FS_EXT_SOURCES})
    set(PHOENIX_RECOVER_FRAGMENT_SOURCES "")
endif()

set(PHOENIX_MINIZ_SOURCES
    scan/third_party/miniz_tinfl.c
    scan/third_party/miniz_alloc.c
)

set(PHOENIX_SOURCES
    main.cpp
    PhoenixRecover.cpp
    common/SysInfo.cpp
    common/Utils.cpp
    common/WindowsElevation.cpp
    common/UiFileTypeCatalog.cpp
    controllers/RecoveryFlowController.cpp
    controllers/RecoveryScanController.cpp
    controllers/ScanResultStore.cpp
    controllers/RecoveryScanWorker.cpp
    ${PHOENIX_PLATFORM_SOURCES}
    disk/Imager.cpp
    disk/PartitionProbe.cpp
    disk/PartitionRecoveryService.cpp
    disk/SmartInfo.cpp
    ${PHOENIX_FS_SOURCES}
    log/Logger.cpp
    models/ScanResultModel.cpp
    models/ScanResultFilterModel.cpp
    media/FfmpegVideoThumb.cpp
    media/MediaThumbnails.cpp
    media/GridPreviewExtensions.cpp
    pages/MoreRecoveryToolsPage.cpp
    pages/SettingsPage.cpp
    pages/RecoveryLocationPage.cpp
    pages/RecoveryScanPage.cpp
    pages/SDCardConnectionPage.cpp
    pages/widgets/ScanProgressBar.cpp
    pages/widgets/PreviewPanel.cpp
    pages/widgets/ScanFooterBar.cpp
    pages/widgets/FilterPanel.cpp
    recover/RecoverEngine.cpp
    recover/RecoverOutputFile.cpp
    recover/RecoverTargetValidation.cpp
    recover/RecoverTargetPath.cpp
    recover/FormatPayloadScore.cpp
    ${PHOENIX_RECOVER_FRAGMENT_SOURCES}
    recover/FragmentStitchService.cpp
    recover/GraphCarveFragmentStitcher.cpp
    scan/carve/CarveMp4.cpp
    scan/carve/CarveZip.cpp
    scan/carve/CarveImage.cpp
    scan/carve/CarveStructure.cpp
    scan/EmbeddedJpegExtract.cpp
    scan/EmbeddedJpegExtractHosts.cpp
    scan/CfbReader.cpp
    ${PHOENIX_MINIZ_SOURCES}
    scan/DeepScan.cpp
    scan/QuickScan.cpp
    scan/RawScan.cpp
    scan/FileSystemModuleService.cpp
    scan/CarveModuleService.cpp
    scan/CarveGraphMergeService.cpp
    scan/ScanSerializer.cpp
    scan/SignatureDB.cpp
    scan/UiTypeSignatureSupplement.cpp
    scan/UnifiedVolumeScanner.cpp
    scan/VolumeSectorScanner.cpp
    repair/RepairManager.cpp
    repair/RepairReport.cpp
    repair/RepairValidate.cpp
    repair/ZipEntryList.cpp
    repair/OfficeZipRepair.cpp
    repair/RepairFfmpeg.cpp
    repair/ZipRepair.cpp
    repair/JpegRepair.cpp
    repair/PngRepair.cpp
    repair/GifRepair.cpp
    repair/Mp4Repair.cpp
    repair/FfmpegVideoRepair.cpp
    repair/FfmpegImageRepair.cpp
    repair/FfmpegRawRepair.cpp
    repair/RepairFormatUtil.cpp
    repair/SqliteRecoverService.cpp
    repair/SqliteRepair.cpp
    controllers/FileRepairController.cpp
    controllers/FileRepairWorker.cpp
    pages/FileRepairPage.cpp
    crash/CrashHandler.cpp
)

set(PHOENIX_RESOURCES
    PhoenixRecover.qrc
    PhoenixRecover.ui
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PHOENIX_VERSION_RAW)
string(STRIP "${PHOENIX_VERSION_RAW}" PHOENIX_VERSION)

if(WIN32)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/installer/PhoenixRecover.rc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/PhoenixRecover.rc"
        @ONLY
    )
endif()

if(WIN32)
    add_executable(PhoenixRecover WIN32
        ${PHOENIX_SOURCES}
        ${PHOENIX_RESOURCES}
        "${CMAKE_CURRENT_BINARY_DIR}/PhoenixRecover.rc"
    )
else()
    add_executable(PhoenixRecover
        ${PHOENIX_SOURCES}
        ${PHOENIX_RESOURCES}
    )
endif()

target_include_directories(PhoenixRecover PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(PhoenixRecover PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
)

if(WIN32)
    target_sources(PhoenixRecover PRIVATE
        media/WinShellVideoThumb.cpp
        media/WinWicImageDecoder.cpp
    )
    target_link_libraries(PhoenixRecover PRIVATE ole32 windowscodecs dbghelp bcrypt)
endif()

if(PHOENIX_HAVE_LIBZIP)
    find_package(libzip REQUIRED)
    target_compile_definitions(PhoenixRecover PRIVATE PHOENIX_HAVE_LIBZIP=1)
    target_link_libraries(PhoenixRecover PRIVATE zip)
endif()

if(MSVC)
    target_compile_options(PhoenixRecover PRIVATE /utf-8)
endif()

target_compile_definitions(PhoenixRecover PRIVATE
    PHOENIX_BUILD_GUI=1
    PHOENIX_VERSION_STRING="${PHOENIX_VERSION}"
)

endif() # PHOENIX_BUILD_GUI

add_subdirectory(tests)

# 鈹€鈹€ 鍙€夛細鏂囦欢绯荤粺 / 鍙栬瘉绗笁鏂癸紙榛樿 OFF锛屼笉閾炬帴浠讳綍搴擄級鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
# 璇存槑涓庨€夊瀷瑙? docs/TECHNICAL_PLAN.md 搂3.2.1
# 绱㈠紩: third_party/fs_thirdparty/README.txt
#
# option(PHOENIX_USE_TSK "Use The Sleuth Kit (libtsk) as optional FS metadata backend" OFF)
# if(PHOENIX_USE_TSK)
#   # 绀轰緥璺緞锛氱敱 vcpkg/Conan/鎵嬪伐 prefix 鎻愪緵
#   # find_path(TSK_INCLUDE NAMES libtsk.h ...)
#   # find_library(TSK_LIB NAMES tsk ...)
#   # target_include_directories(PhoenixRecover PRIVATE ${TSK_INCLUDE})
#   # target_link_libraries(PhoenixRecover PRIVATE ${TSK_LIB})
#   # target_compile_definitions(PhoenixRecover PRIVATE PHOENIX_HAVE_LIBTSK=1)
# endif()
#
# Linux 涓婂彧璇?NTFS 鑻ラ噰鐢?ntfs-3g / libntfs-3g锛岄』鍦ㄦ硶鍔¤瘎瀹″悗鍙﹀姞
# find_package / pkg-config 涓庨摼鎺ヨ鍒欙紱鍕夸笌涓荤▼搴忚鍙瘉鍋囪鍐茬獊銆?


