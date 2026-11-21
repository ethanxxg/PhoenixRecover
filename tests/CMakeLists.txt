# PhoenixRecover ??????
#
# ??C++ ??????????Qt / Windows I/O??????????
# ????Qt ????????PHOENIX_BUILD_GUI=ON ???????

# ???? Helper: ??????????????????????????????????????????????????????????????????????????????????????????????????????????????
function(phoenix_add_test TARGET)
    set_target_properties(${TARGET} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    target_include_directories(${TARGET} PRIVATE ${CMAKE_SOURCE_DIR})
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE /utf-8)
    endif()
endfunction()

if(WIN32)
    set(PHOENIX_SECTOR_READER ${CMAKE_SOURCE_DIR}/disk/SectorReader_win.cpp)
    set(PHOENIX_PLATFORM_LIBS ole32 advapi32)
elseif(UNIX)
    set(PHOENIX_SECTOR_READER ${CMAKE_SOURCE_DIR}/disk/SectorReader_unix.cpp)
    set(PHOENIX_PLATFORM_LIBS "")
endif()

# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
# 1. UtilsNumeric ???????????? (??C++????????
# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
add_executable(phoenix_unit_tests
    test_utils_numeric.cpp
)
phoenix_add_test(phoenix_unit_tests)
add_test(NAME UtilsNumeric COMMAND phoenix_unit_tests)

add_executable(test_utils_unique_path test_utils_unique_path.cpp
    ${CMAKE_SOURCE_DIR}/common/Utils.cpp
)
phoenix_add_test(test_utils_unique_path)
add_test(NAME UtilsUniquePath COMMAND test_utils_unique_path)

add_executable(test_format_payload_score
    test_format_payload_score.cpp
    ${CMAKE_SOURCE_DIR}/recover/FormatPayloadScore.cpp
)
phoenix_add_test(test_format_payload_score)
add_test(NAME FormatPayloadScore COMMAND test_format_payload_score)

if(WIN32)
add_executable(test_fat_fragment_stitch_synthetic
    test_fat_fragment_stitch_synthetic.cpp
    ${CMAKE_SOURCE_DIR}/recover/FatFragmentStitcher.cpp
    ${CMAKE_SOURCE_DIR}/recover/NtfsFragmentStitcher.cpp
    ${CMAKE_SOURCE_DIR}/recover/GraphCarveFragmentStitcher.cpp
    ${CMAKE_SOURCE_DIR}/recover/FragmentStitchService.cpp
    ${CMAKE_SOURCE_DIR}/recover/FormatPayloadScore.cpp
    ${PHOENIX_SECTOR_READER}
    ${CMAKE_SOURCE_DIR}/log/Logger.cpp
    ${CMAKE_SOURCE_DIR}/common/Utils.cpp
)
phoenix_add_test(test_fat_fragment_stitch_synthetic)
if(PHOENIX_PLATFORM_LIBS)
    target_link_libraries(test_fat_fragment_stitch_synthetic PRIVATE ${PHOENIX_PLATFORM_LIBS})
endif()
add_test(NAME FatFragmentStitchSynthetic COMMAND test_fat_fragment_stitch_synthetic)
endif()

if(PHOENIX_SECTOR_READER AND WIN32)
    add_executable(test_graph_carve_fragment_stitch
        test_graph_carve_fragment_stitch.cpp
        ${CMAKE_SOURCE_DIR}/recover/GraphCarveFragmentStitcher.cpp
        ${CMAKE_SOURCE_DIR}/recover/FragmentStitchService.cpp
        ${CMAKE_SOURCE_DIR}/recover/FatFragmentStitcher.cpp
        ${CMAKE_SOURCE_DIR}/recover/NtfsFragmentStitcher.cpp
        ${CMAKE_SOURCE_DIR}/recover/FormatPayloadScore.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
        ${PHOENIX_SECTOR_READER}
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    )
    phoenix_add_test(test_graph_carve_fragment_stitch)
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_graph_carve_fragment_stitch PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME GraphCarveFragmentStitch COMMAND test_graph_carve_fragment_stitch)

    add_executable(test_carve_graph_merge
        test_carve_graph_merge.cpp
        ${CMAKE_SOURCE_DIR}/scan/CarveGraphMergeService.cpp
        ${CMAKE_SOURCE_DIR}/recover/GraphCarveFragmentStitcher.cpp
        ${CMAKE_SOURCE_DIR}/recover/FragmentStitchService.cpp
        ${CMAKE_SOURCE_DIR}/recover/FatFragmentStitcher.cpp
        ${CMAKE_SOURCE_DIR}/recover/NtfsFragmentStitcher.cpp
        ${CMAKE_SOURCE_DIR}/recover/FormatPayloadScore.cpp
        ${PHOENIX_SECTOR_READER}
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    )
    phoenix_add_test(test_carve_graph_merge)
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_carve_graph_merge PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME CarveGraphMerge COMMAND test_carve_graph_merge)
endif() # WIN32 graph carve tests

add_executable(test_validate_target_path
    test_validate_target_path.cpp
    ${CMAKE_SOURCE_DIR}/recover/RecoverTargetValidation.cpp
    ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    ${CMAKE_SOURCE_DIR}/log/Logger.cpp
)
phoenix_add_test(test_validate_target_path)
add_test(NAME ValidateTargetPath COMMAND test_validate_target_path)

add_executable(test_recover_build_target_path
    test_recover_build_target_path.cpp
    ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
)
phoenix_add_test(test_recover_build_target_path)
add_test(NAME RecoverBuildTargetPath COMMAND test_recover_build_target_path)

add_executable(test_scan_session_roundtrip
    test_scan_session_roundtrip.cpp
    ${CMAKE_SOURCE_DIR}/scan/ScanSerializer.cpp
    ${CMAKE_SOURCE_DIR}/log/Logger.cpp
    ${CMAKE_SOURCE_DIR}/common/Utils.cpp
)
phoenix_add_test(test_scan_session_roundtrip)
add_test(NAME ScanSession_RoundTrip COMMAND test_scan_session_roundtrip)

# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
# 2. FatStructs ??FAT ?????sizeof + ????+ FatDateTimeToFiletime
#    sizeof/???? ??C++??atDateTimeToFiletime: Windows-only (??.cpp ??#ifdef)
# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
add_executable(test_fat_structs test_fat_structs.cpp
    ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
)
phoenix_add_test(test_fat_structs)
add_test(NAME FatStructs COMMAND test_fat_structs)

# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
# 3. NtfsStructs ??NTFS ?????sizeof + DecodeDataRuns ????
#    ?????sizeof: Windows-only (NtfsStructs.h ????<Windows.h>)
#    DataRun ???? ??C++??????????????Utils::ReadSignedLE ????????
# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
add_executable(test_ntfs_structs test_ntfs_structs.cpp)
phoenix_add_test(test_ntfs_structs)
add_test(NAME NtfsStructs COMMAND test_ntfs_structs)

# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
# 4. CarveCommon ??CarveResult ??????????????????(??C++????????
# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
add_executable(test_carve_common test_carve_common.cpp)
phoenix_add_test(test_carve_common)
add_test(NAME CarveCommon COMMAND test_carve_common)

add_executable(test_embedded_jpeg_extract
    test_embedded_jpeg_extract.cpp
    ${CMAKE_SOURCE_DIR}/scan/EmbeddedJpegExtract.cpp
    ${CMAKE_SOURCE_DIR}/scan/CfbReader.cpp
    ${CMAKE_SOURCE_DIR}/scan/third_party/miniz_tinfl.c
    ${CMAKE_SOURCE_DIR}/scan/third_party/miniz_alloc.c
)
phoenix_add_test(test_embedded_jpeg_extract)
add_test(NAME EmbeddedJpegExtract COMMAND test_embedded_jpeg_extract)

# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
# 5. ScanProgress ??ScanProgress / UnifiedScanConfig ?????(??C++????????
# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
add_executable(test_scan_progress test_scan_progress.cpp)
phoenix_add_test(test_scan_progress)
add_test(NAME ScanProgress COMMAND test_scan_progress)

add_executable(test_recovery_modules test_recovery_modules.cpp)
phoenix_add_test(test_recovery_modules)
add_test(NAME RecoveryModules_FsTypeRouting COMMAND test_recovery_modules)

add_executable(test_sqlite_recover
    test_sqlite_recover.cpp
    ${CMAKE_SOURCE_DIR}/repair/SqliteRecoverService.cpp
)
phoenix_add_test(test_sqlite_recover)
add_test(NAME SqliteRecover_PageLevel COMMAND test_sqlite_recover)

# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
# 5b. UiTypeCoverage ??UI ?????????????????? C++??
# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
add_executable(test_ui_type_coverage
    test_ui_type_coverage.cpp
    ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
    ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
    ${CMAKE_SOURCE_DIR}/scan/ExtensionCoverage.cpp
    ${CMAKE_SOURCE_DIR}/common/UiFileTypeCatalog.cpp
    ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    ${CMAKE_SOURCE_DIR}/log/Logger.cpp
)
phoenix_add_test(test_ui_type_coverage)
target_compile_definitions(test_ui_type_coverage PRIVATE PHOENIX_BUILD_GUI=0)
add_test(NAME UiTypeCoverage COMMAND test_ui_type_coverage)

# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
# 6. SignatureDB ???????? (????Qt????PHOENIX_BUILD_GUI=ON ?????
# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
if(PHOENIX_BUILD_GUI)
    add_executable(test_signature_db
        test_signature_db.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
    )
    phoenix_add_test(test_signature_db)
    target_link_libraries(test_signature_db PRIVATE Qt5::Core)
    add_test(NAME SignatureDB COMMAND test_signature_db)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# 6b. SignatureDB AC-backed Match equivalence + speedup (no Qt, PHOENIX_BUILD_GUI=0)
#     Proves the Aho-Corasick-backed Match is behavior-identical to the legacy
#     linear scan over the real built-in signature set. Offline, no images.
add_executable(test_signature_db_ac
    test_signature_db_ac.cpp
    ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
    ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
    ${CMAKE_SOURCE_DIR}/scan/ExtensionCoverage.cpp
    ${CMAKE_SOURCE_DIR}/common/UiFileTypeCatalog.cpp
    ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    ${CMAKE_SOURCE_DIR}/log/Logger.cpp
)
phoenix_add_test(test_signature_db_ac)
target_compile_definitions(test_signature_db_ac PRIVATE PHOENIX_BUILD_GUI=0)
add_test(NAME SignatureDBAC COMMAND test_signature_db_ac)

# ─────────────────────────────────────────────────────────────────────────────
# 6c. A1-9 / task #59 — Qt-free custom signature loading in headless builds.
#     Proves LoadFromFile() works with PHOENIX_BUILD_GUI=0 (old code returned
#     -1 unconditionally), verifies SIGNATURE_FORMAT.md fields, strict JSON
#     rejection, and lazy AC matcher rebuild via Match()/ScanBatch().
add_executable(test_signature_db_load
    test_signature_db_load.cpp
    ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
    ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
    ${CMAKE_SOURCE_DIR}/scan/ExtensionCoverage.cpp
    ${CMAKE_SOURCE_DIR}/common/UiFileTypeCatalog.cpp
    ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    ${CMAKE_SOURCE_DIR}/log/Logger.cpp
)
phoenix_add_test(test_signature_db_load)
target_compile_definitions(test_signature_db_load PRIVATE
    PHOENIX_BUILD_GUI=0
    PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
)
add_test(NAME SignatureDBLoad COMMAND test_signature_db_load)

# ─────────────────────────────────────────────────────────────────────────────
# 6d. P0-6 — engine-only pipeline gate.
#     Real-disk recovery must DISCOVER with NtfsParser::ScanMetadata, RECOVER
#     with RecoverEngine::RecoverBatch, and CLASSIFY FORMAT with SignatureDB.
#     The independent MFT oracle is a comparison truth, never a candidate
#     source. Source-level guards make the pipeline claim falsifiable.
#     Negative guard: PHOENIX_NEGATIVE_ENGINE_FORMAT_GUARD=1 simulates the
#     forbidden regression (extension-name guessing / oracle-driven discovery)
#     and MUST fail the test.
add_executable(test_engine_scan_format_match
    test_engine_scan_format_match.cpp
    ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
    ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
    ${CMAKE_SOURCE_DIR}/scan/ExtensionCoverage.cpp
    ${CMAKE_SOURCE_DIR}/common/UiFileTypeCatalog.cpp
    ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    ${CMAKE_SOURCE_DIR}/log/Logger.cpp
    ${CMAKE_SOURCE_DIR}/recover/FormatPayloadScore.cpp
)
phoenix_add_test(test_engine_scan_format_match)
target_compile_definitions(test_engine_scan_format_match PRIVATE
    PHOENIX_BUILD_GUI=0
    PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
)
add_test(NAME EngineScanFormatMatch COMMAND test_engine_scan_format_match)

# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
# 7. ?????????????C++??????Windows ??Platform.h ???????Qt??
#    ????.dd ??? ?????????????????????????????????
# ?????????????????????????????????????????????????????????????????????????????????????????????????????????????????
if(PHOENIX_SECTOR_READER)

    # ???? ??????????????????????????????????????????????????????????????????????????????????????????
    set(INTEGRATION_COMMON_SOURCES
        ${PHOENIX_SECTOR_READER}
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/recover/FormatPayloadScore.cpp
        ${CMAKE_SOURCE_DIR}/recover/FatFragmentStitcher.cpp
        ${CMAKE_SOURCE_DIR}/recover/NtfsFragmentStitcher.cpp
        ${CMAKE_SOURCE_DIR}/recover/FragmentStitchService.cpp
        ${CMAKE_SOURCE_DIR}/recover/GraphCarveFragmentStitcher.cpp
    )

    # --- P0-4: real-disk output-path collision regression gate -------------
    # Pure in-memory NTFS resident files; no image / physical disk needed.
    # Locks the known defect and byte-verifies the headless isolation workaround.
    add_executable(test_recover_output_collision
        test_recover_output_collision.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_recover_output_collision)
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_recover_output_collision PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME RecoverOutputCollision COMMAND test_recover_output_collision)

    # --- P0-5: NTFS alternate data stream (Zone.Identifier) gate -----------
    # Synthetic 4 MiB NTFS image, no physical disk / golden image needed.
    # Locks: default $DATA selection, ADS isolation, byte recovery, and the
    # known FileNode::id collision between a file and its ADS node.
    add_executable(test_ntfs_ads_stream
        test_ntfs_ads_stream.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_ntfs_ads_stream)
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_ntfs_ads_stream PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME NtfsAdsStream COMMAND test_ntfs_ads_stream)

    # --- P0-7: $ATTRIBUTE_LIST extension MFT record gate --------------------
    # Synthetic 4 MiB NTFS image: base record with NO $DATA, two extension
    # records hold the runlist. Entries are written out of VCN order to prove
    # the parser sorts by lowestVCN before merging. Negative guard removes the
    # $ATTRIBUTE_LIST and MUST fail the cluster/byte checks.
    add_executable(test_ntfs_attribute_list
        test_ntfs_attribute_list.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_ntfs_attribute_list)
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_ntfs_attribute_list PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME NtfsAttributeList COMMAND test_ntfs_attribute_list)

    add_executable(test_recover_pause
        test_recover_pause.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_recover_pause)
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_recover_pause PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME Recover_PauseGate COMMAND test_recover_pause)

    if(WIN32)
    # ???? 7a. DFTT FAT Undelete ??????????????????????????????????????????????????????????????????????????????????????????
    add_executable(test_dftt_fat
        integration/test_dftt_fat_undelete.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
    )
    phoenix_add_test(test_dftt_fat)
    target_compile_definitions(test_dftt_fat PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_dftt_fat PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME DFTT_FAT_Undelete COMMAND test_dftt_fat)

    add_executable(test_dftt_fat_recover_bytes
        integration/test_dftt_fat_recover_bytes.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_dftt_fat_recover_bytes)
    target_compile_definitions(test_dftt_fat_recover_bytes PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_dftt_fat_recover_bytes PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME DFTT_FAT_RecoverBytes COMMAND test_dftt_fat_recover_bytes)

    # ???? 7b. DFTT NTFS Undelete ????????????????????????????????????????????????????????????????????????????????????????
    add_executable(test_dftt_ntfs
        integration/test_dftt_ntfs_undelete.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
    )
    phoenix_add_test(test_dftt_ntfs)
    target_compile_definitions(test_dftt_ntfs PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_dftt_ntfs PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME DFTT_NTFS_Undelete COMMAND test_dftt_ntfs)

    add_executable(test_dftt_ntfs_recover_bytes
        integration/test_dftt_ntfs_recover_bytes.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_dftt_ntfs_recover_bytes)
    target_compile_definitions(test_dftt_ntfs_recover_bytes PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_dftt_ntfs_recover_bytes PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME DFTT_NTFS_RecoverBytes COMMAND test_dftt_ntfs_recover_bytes)

    add_test(NAME DFTT_Recover_GoldenBundle
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/testdata/tools/Invoke-DfttRecoverGolden.ps1"
            -RepoRoot "${CMAKE_SOURCE_DIR}"
            -FatExe "$<TARGET_FILE:test_dftt_fat_recover_bytes>"
            -NtfsExe "$<TARGET_FILE:test_dftt_ntfs_recover_bytes>"
    )
    set_tests_properties(DFTT_Recover_GoldenBundle PROPERTIES
        DEPENDS "DFTT_FAT_RecoverBytes;DFTT_NTFS_RecoverBytes"
    )

    # ???? 7x. PartitionProbe + SectorReader ?????? ????????????????????????????????????????????
    add_executable(test_partition_probe
        test_partition_probe.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/disk/PartitionProbe.cpp
    )
    phoenix_add_test(test_partition_probe)
    target_compile_definitions(test_partition_probe PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_partition_probe PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME PartitionProbe COMMAND test_partition_probe)

    add_executable(test_partition_recovery_probe
        integration/test_partition_recovery_probe.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/disk/PartitionProbe.cpp
        ${CMAKE_SOURCE_DIR}/disk/PartitionRecoveryService.cpp
    )
    phoenix_add_test(test_partition_recovery_probe)
    target_compile_definitions(test_partition_recovery_probe PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_partition_recovery_probe PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME PartitionRecovery_ProbeImage COMMAND test_partition_recovery_probe)

    # ???? 7c. CFReDS DFR-01 FAT ?????? ??????????????????????????????????????????????????????????????????????
    add_executable(test_cfreds_fat
        integration/test_cfreds_fat.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/disk/PartitionProbe.cpp
    )
    phoenix_add_test(test_cfreds_fat)
    target_compile_definitions(test_cfreds_fat PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_cfreds_fat PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME CFReDS_FAT_DFR01 COMMAND test_cfreds_fat)

    # ???? 7d. CFReDS DFR-01 NTFS ?????? ????????????????????????????????????????????????????????????????????
    add_executable(test_cfreds_ntfs
        integration/test_cfreds_ntfs.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/disk/PartitionProbe.cpp
    )
    phoenix_add_test(test_cfreds_ntfs)
    target_compile_definitions(test_cfreds_ntfs PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_cfreds_ntfs PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME CFReDS_NTFS_DFR01 COMMAND test_cfreds_ntfs)

    add_executable(test_cfreds_ext4
        integration/test_cfreds_ext4.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/ext4/Ext4Parser.cpp
        ${CMAKE_SOURCE_DIR}/fs/exfat/ExfatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/disk/PartitionProbe.cpp
        ${CMAKE_SOURCE_DIR}/disk/PartitionRecoveryService.cpp
        ${CMAKE_SOURCE_DIR}/scan/FileSystemModuleService.cpp
        ${CMAKE_SOURCE_DIR}/fs/refs/RefsParser.cpp
    )
    phoenix_add_test(test_cfreds_ext4)
    target_compile_definitions(test_cfreds_ext4 PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_cfreds_ext4 PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME CFReDS_EXT4_DFR01 COMMAND test_cfreds_ext4)

    # W2/W3: exFAT must be reachable from the automatic FS probe chain,
    # pinned by synthetic boots + the real CFReDS dfr-01-xfat.dd golden.
    add_executable(test_exfat_probe_dispatch
        test_exfat_probe_dispatch.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/exfat/ExfatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ext4/Ext4Parser.cpp
        ${CMAKE_SOURCE_DIR}/scan/FileSystemModuleService.cpp
        ${CMAKE_SOURCE_DIR}/fs/refs/RefsParser.cpp
    )
    phoenix_add_test(test_exfat_probe_dispatch)
    target_compile_definitions(test_exfat_probe_dispatch PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_exfat_probe_dispatch PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME ExfatProbeDispatch COMMAND test_exfat_probe_dispatch)

    # W3: real exFAT recovery-byte / MD5 gate.  Compares the bytes written by
    # RecoverEngine against an independent hand-parsed exFAT oracle that reads
    # the raw image directly, and pins the whole-image read-only invariant.
    add_executable(test_exfat_recover_bytes
        test_exfat_recover_bytes.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/exfat/ExfatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ext4/Ext4Parser.cpp
        ${CMAKE_SOURCE_DIR}/scan/FileSystemModuleService.cpp
        ${CMAKE_SOURCE_DIR}/fs/refs/RefsParser.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_exfat_recover_bytes)
    target_compile_definitions(test_exfat_recover_bytes PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_exfat_recover_bytes PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME ExfatRecoverBytes COMMAND test_exfat_recover_bytes)

    # W6: independent ext2 / ext3 legacy golden.  Pinned values come from a
    # standalone Python oracle that walks superblock -> group descriptors ->
    # inode table -> linear dirents -> i_block[] directly on the raw images.
    # Covers legacy indirect block map, recovery-byte MD5, deleted dirent
    # names recovered from rec_len slack, and the read-only source invariant.
    add_executable(test_ext2_ext3_legacy_golden
        test_ext2_ext3_legacy_golden.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/ext4/Ext4Parser.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_ext2_ext3_legacy_golden)
    target_compile_definitions(test_ext2_ext3_legacy_golden PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_ext2_ext3_legacy_golden PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME Ext2Ext3LegacyGolden COMMAND test_ext2_ext3_legacy_golden)

    # --- ext4 depth: extents / inline_data / bigalloc / uninit extent --------
    # Fixtures come from tools/make_ext4_fixture.py, an independent oracle that
    # writes the on-disk structures itself.  Generated at build time into the
    # build tree when a Python 3 interpreter is available; otherwise the test
    # falls back to testdata/ext4-synthetic (run the script manually).
    set(EXT4_FIXTURE_DIR ${CMAKE_BINARY_DIR}/ext4-fixtures)
    find_program(PHOENIX_PY_EXE NAMES python3 python)
    if(PHOENIX_PY_EXE)
        set(PHOENIX_PY_CMD ${PHOENIX_PY_EXE})
    else()
        find_program(PHOENIX_PY_LAUNCHER NAMES py)
        if(PHOENIX_PY_LAUNCHER)
            set(PHOENIX_PY_CMD ${PHOENIX_PY_LAUNCHER} -3)
        endif()
    endif()

    set(EXT4_FIXTURE_OUTPUTS
        ${EXT4_FIXTURE_DIR}/ext4-base.img
        ${EXT4_FIXTURE_DIR}/ext4-uninit.img
        ${EXT4_FIXTURE_DIR}/ext4-bigalloc.img
        ${EXT4_FIXTURE_DIR}/ext4-inline.img
        ${EXT4_FIXTURE_DIR}/ext4-sparse.img
        ${EXT4_FIXTURE_DIR}/ext4-journal.img
        ${EXT4_FIXTURE_DIR}/ext4-journal-clean.img
    )
    if(PHOENIX_PY_CMD)
        add_custom_command(
            OUTPUT ${EXT4_FIXTURE_OUTPUTS}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${EXT4_FIXTURE_DIR}
            COMMAND ${PHOENIX_PY_CMD} ${CMAKE_SOURCE_DIR}/tools/make_ext4_fixture.py
                    --out ${EXT4_FIXTURE_DIR}
            DEPENDS ${CMAKE_SOURCE_DIR}/tools/make_ext4_fixture.py
            COMMENT "Generating synthetic ext4 fixtures (independent oracle)"
            VERBATIM)
        add_custom_target(ext4_fixtures DEPENDS ${EXT4_FIXTURE_OUTPUTS})
        set(EXT4_FIXTURE_DEF
            PHOENIX_EXT4_FIXTURE_DIR=L"${EXT4_FIXTURE_DIR}")
    else()
        set(EXT4_FIXTURE_DEF
            PHOENIX_EXT4_FIXTURE_DIR=L"${CMAKE_SOURCE_DIR}/testdata/ext4-synthetic")
    endif()

    add_executable(test_ext4_depth_features
        test_ext4_depth_features.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/exfat/ExfatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ext4/Ext4Parser.cpp
        ${CMAKE_SOURCE_DIR}/fs/refs/RefsParser.cpp
        ${CMAKE_SOURCE_DIR}/scan/FileSystemModuleService.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_ext4_depth_features)
    target_compile_definitions(test_ext4_depth_features PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
        ${EXT4_FIXTURE_DEF}
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_ext4_depth_features PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    if(TARGET ext4_fixtures)
        add_dependencies(test_ext4_depth_features ext4_fixtures)
    endif()
    add_test(NAME Ext4DepthFeatures COMMAND test_ext4_depth_features)

    # --- ReFS depth: nested tree + inline + extent recovery bytes -----------
    # Golden corpus testdata/refs-golden (A1-5a, independent pure-Python
    # oracle).  Drives the production probe chain (empty fsType) so the ReFS
    # detector itself is gated, then verifies every entry's SHA-256 through
    # RecoverEngine.  Locks two defects found while wiring the product path:
    #   * ReFS VBR geometry lives at +0x20/+0x24, not the NTFS BPB at 11/13
    #     (reading 11/13 returned 0 and degraded every extent to one sector)
    #   * directory entries do not carry a child object id, so the tree must be
    #     rebuilt from the parent-child table (nested subdir was lost)
    add_executable(test_refs_recover_bytes
        test_refs_recover_bytes.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/refs/RefsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/exfat/ExfatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ext4/Ext4Parser.cpp
        ${CMAKE_SOURCE_DIR}/scan/FileSystemModuleService.cpp
        ${CMAKE_SOURCE_DIR}/fs/refs/RefsParser.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetValidation.cpp
    )
    phoenix_add_test(test_refs_recover_bytes)
    target_compile_definitions(test_refs_recover_bytes PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_refs_recover_bytes PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME RefsRecoverBytes COMMAND test_refs_recover_bytes)


    # W7: multi-cluster / FAT-misleading exFAT recovery-byte gate.  Real DFR
    # images dfr-02/03/05/12; dfr-12's FAT entries point at unrelated clusters,
    # so following FAT instead of the NoFatChain rule produces wrong bytes.
    add_executable(test_exfat_multicluster_bytes
        test_exfat_multicluster_bytes.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/exfat/ExfatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ext4/Ext4Parser.cpp
        ${CMAKE_SOURCE_DIR}/scan/FileSystemModuleService.cpp
        ${CMAKE_SOURCE_DIR}/fs/refs/RefsParser.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_exfat_multicluster_bytes)
    target_compile_definitions(test_exfat_multicluster_bytes PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_exfat_multicluster_bytes PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME ExfatMulticlusterBytes COMMAND test_exfat_multicluster_bytes)

    # CFReDS DFR-05 (fragmentation) ??optional when testdata present
    add_executable(test_cfreds_dfr05_fat
        integration/test_cfreds_dfr05_fat.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/disk/PartitionProbe.cpp
    )
    phoenix_add_test(test_cfreds_dfr05_fat)
    target_compile_definitions(test_cfreds_dfr05_fat PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_cfreds_dfr05_fat PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME CFReDS_DFR05_FAT COMMAND test_cfreds_dfr05_fat)

    add_executable(test_cfreds_dfr05_ntfs
        integration/test_cfreds_dfr05_ntfs.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/disk/PartitionProbe.cpp
    )
    phoenix_add_test(test_cfreds_dfr05_ntfs)
    target_compile_definitions(test_cfreds_dfr05_ntfs PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_cfreds_dfr05_ntfs PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME CFReDS_DFR05_NTFS COMMAND test_cfreds_dfr05_ntfs)

    add_executable(test_cfreds_dfr05_braid_fat
        integration/test_cfreds_dfr05_braid_fat.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
    )
    phoenix_add_test(test_cfreds_dfr05_braid_fat)
    target_compile_definitions(test_cfreds_dfr05_braid_fat PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_cfreds_dfr05_braid_fat PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME CFReDS_DFR05_BRAID_FAT COMMAND test_cfreds_dfr05_braid_fat)

    add_executable(test_cfreds_dfr05_braid_ntfs
        integration/test_cfreds_dfr05_braid_ntfs.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
    )
    phoenix_add_test(test_cfreds_dfr05_braid_ntfs)
    target_compile_definitions(test_cfreds_dfr05_braid_ntfs PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_cfreds_dfr05_braid_ntfs PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME CFReDS_DFR05_BRAID_NTFS COMMAND test_cfreds_dfr05_braid_ntfs)

    add_executable(test_cfreds_dfr05_recover_smoke
        integration/test_cfreds_dfr05_recover_smoke.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_cfreds_dfr05_recover_smoke)
    target_compile_definitions(test_cfreds_dfr05_recover_smoke PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_cfreds_dfr05_recover_smoke PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME CFReDS_DFR05_RecoverSmoke COMMAND test_cfreds_dfr05_recover_smoke)

    add_executable(test_cfreds_dfr05_quality_curve
        integration/test_cfreds_dfr05_quality_curve.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
        ${CMAKE_SOURCE_DIR}/recover/FormatPayloadScore.cpp
        ${CMAKE_SOURCE_DIR}/recover/FatFragmentStitcher.cpp
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    )
    phoenix_add_test(test_cfreds_dfr05_quality_curve)
    target_compile_definitions(test_cfreds_dfr05_quality_curve PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_cfreds_dfr05_quality_curve PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME CFReDS_DFR05_QualityCurve COMMAND test_cfreds_dfr05_quality_curve)

    add_executable(test_fragment_fat_structured_quality
        integration/test_fragment_fat_structured_quality.cpp
        fixtures/fragment_fat_structured_fixture.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_fragment_fat_structured_quality)
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_fragment_fat_structured_quality PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME FragmentFatStructured_QualityGate COMMAND test_fragment_fat_structured_quality)

    add_executable(test_ntfs_fragment_stitch_dispatch
        test_ntfs_fragment_stitch_dispatch.cpp
        ${INTEGRATION_COMMON_SOURCES}
    )
    phoenix_add_test(test_ntfs_fragment_stitch_dispatch)
    add_test(NAME NtfsFragmentStitch_Dispatch COMMAND test_ntfs_fragment_stitch_dispatch)

    add_executable(test_cfreds_braid_ntfs_stitch
        integration/test_cfreds_braid_ntfs_stitch.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_cfreds_braid_ntfs_stitch)
    target_compile_definitions(test_cfreds_braid_ntfs_stitch PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_cfreds_braid_ntfs_stitch PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME CFReDS_BraidNtfs_FragmentStitch COMMAND test_cfreds_braid_ntfs_stitch)

    add_executable(test_fragment_ntfs_structured_quality
        integration/test_fragment_ntfs_structured_quality.cpp
        fixtures/fragment_ntfs_structured_fixture.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_fragment_ntfs_structured_quality)
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_fragment_ntfs_structured_quality PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME FragmentNtfsStructured_QualityGate COMMAND test_fragment_ntfs_structured_quality)

    add_executable(test_media_format_quality
        integration/test_media_format_quality.cpp
        ${CMAKE_SOURCE_DIR}/recover/FormatPayloadScore.cpp
    )
    phoenix_add_test(test_media_format_quality)
    target_compile_definitions(test_media_format_quality PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    add_test(NAME MediaFormatQualityBaseline COMMAND test_media_format_quality)

    add_executable(test_dfr_windows_batch
        integration/test_dfr_windows_batch.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/exfat/ExfatParser.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_dfr_windows_batch)
    target_compile_definitions(test_dfr_windows_batch PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_dfr_windows_batch PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME DFR_Windows_Batch COMMAND test_dfr_windows_batch)
    set_tests_properties(DFR_Windows_Batch PROPERTIES
        ENVIRONMENT "PHOENIX_DFR_WINDOWS_MAX_IMAGES=12;PHOENIX_DFR_MAX_RECOVER=16"
    )

    add_executable(test_dfr_nist_compare
        integration/test_dfr_nist_compare.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/exfat/ExfatParser.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_dfr_nist_compare)
    target_compile_definitions(test_dfr_nist_compare PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_dfr_nist_compare PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME DFR_NIST_Compare COMMAND test_dfr_nist_compare)
    set_tests_properties(DFR_NIST_Compare PROPERTIES TIMEOUT 7200)

    add_test(NAME DFR_NIST_BatchStrict COMMAND test_dfr_windows_batch)
    set_tests_properties(DFR_NIST_BatchStrict PROPERTIES
        ENVIRONMENT "PHOENIX_DFR_WINDOWS_MAX_IMAGES=0;PHOENIX_DFR_MAX_RECOVER=0;PHOENIX_DFR_NIST_STRICT=1;PHOENIX_DFR_ONLY_SUBDIR=dfr-test-images"
        TIMEOUT 7200
    )

    add_executable(test_dfr_nist_spotcheck
        integration/test_dfr_nist_spotcheck.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/exfat/ExfatParser.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_dfr_nist_spotcheck)
    target_compile_definitions(test_dfr_nist_spotcheck PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_dfr_nist_spotcheck PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME DFR_NIST_SpotCheck COMMAND test_dfr_nist_spotcheck)
    # Raw .dd/.raw open smoke across testdata subtrees (see PHOENIX_TESTDATA_SMOKE_ROOTS)
    add_executable(test_testdata_image_smoke
        integration/test_testdata_image_smoke.cpp
        ${INTEGRATION_COMMON_SOURCES}
    )
    phoenix_add_test(test_testdata_image_smoke)
    target_compile_definitions(test_testdata_image_smoke PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_testdata_image_smoke PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME Testdata_ImageSmoke COMMAND test_testdata_image_smoke)

    # DFTT #11 - raw partition carving (optional when 11-carve-fat.dd present)
    add_executable(test_dftt_11_carve_fat
        integration/test_dftt_11_carve_fat.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/scan/RawScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveZip.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveMp4.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveImage.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveStructure.cpp
    )
    phoenix_add_test(test_dftt_11_carve_fat)
    target_compile_definitions(test_dftt_11_carve_fat PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_dftt_11_carve_fat PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME DFTT_11_CARVE_FAT COMMAND test_dftt_11_carve_fat)

    # A1-6 / parallel-track: RawScan streaming AC batch — equivalence vs legacy linear
    add_executable(test_rawscan_ac
        test_rawscan_ac.cpp
        ${PHOENIX_SECTOR_READER}
        ${CMAKE_SOURCE_DIR}/scan/RawScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveZip.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveMp4.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveImage.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveStructure.cpp
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    )
    phoenix_add_test(test_rawscan_ac)
    target_compile_definitions(test_rawscan_ac PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_rawscan_ac PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME RawScanAC COMMAND test_rawscan_ac)

    # W5 / CAR-002: quantify RAW-carving false positives on seeded random noise
    # with a known ground truth (TP / FP / FN + deterministic replay).
    add_executable(test_rawscan_false_positive
        test_rawscan_false_positive.cpp
        ${PHOENIX_SECTOR_READER}
        ${CMAKE_SOURCE_DIR}/scan/RawScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveZip.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveMp4.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveImage.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveStructure.cpp
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    )
    phoenix_add_test(test_rawscan_false_positive)
    target_compile_definitions(test_rawscan_false_positive PRIVATE
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_rawscan_false_positive PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME RawScanFalsePositive COMMAND test_rawscan_false_positive)

    # W8 / CAR-002: quantify the OPT-IN strict header-structure gate on the
    # same seeded-noise corpus.  Structured plants = TP; bare plants = weak
    # matches the gate should reject.  Strict mode is OFF by default.
    add_executable(test_rawscan_strict_headers
        test_rawscan_strict_headers.cpp
        ${PHOENIX_SECTOR_READER}
        ${CMAKE_SOURCE_DIR}/scan/RawScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveZip.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveMp4.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveImage.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveStructure.cpp
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    )
    phoenix_add_test(test_rawscan_strict_headers)
    target_compile_definitions(test_rawscan_strict_headers PRIVATE
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_rawscan_strict_headers PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME RawScanStrictHeaders COMMAND test_rawscan_strict_headers)

    # ── A1-7 / task #51: DeepScan damaged-NTFS fallback → SignatureMatcher RAW carving ──
    # Proves DeepScan recognises a (damaged) NTFS volume, the structured MFT/FAT parse
    # recovers almost nothing, and it transparently falls back to content-based RAW
    # carving via the verified SignatureMatcher path. Contrast case asserts the SAME
    # magics are NOT carved without a recognised FS boot sector (no fallback fired).
    add_executable(test_deepscan_fallback
        test_deepscan_fallback.cpp
        ${PHOENIX_SECTOR_READER}
        ${CMAKE_SOURCE_DIR}/scan/DeepScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/RawScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveZip.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveMp4.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveImage.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveStructure.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    )
    phoenix_add_test(test_deepscan_fallback)
    target_compile_definitions(test_deepscan_fallback PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_deepscan_fallback PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME DeepScanFallback COMMAND test_deepscan_fallback)

    # W3: quick-formatted volume recovery golden.  Fresh boot + zeroed metadata
    # + residual payloads in the data area; verifies DeepScan RAW fallback finds
    # the planted offsets AND that the carved byte ranges match the oracle.
    add_executable(test_formatted_volume_recovery
        test_formatted_volume_recovery.cpp
        ${PHOENIX_SECTOR_READER}
        ${CMAKE_SOURCE_DIR}/scan/DeepScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/RawScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveZip.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveMp4.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveImage.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveStructure.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    )
    phoenix_add_test(test_formatted_volume_recovery)
    target_compile_definitions(test_formatted_volume_recovery PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_formatted_volume_recovery PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME FormattedVolumeRecovery COMMAND test_formatted_volume_recovery)

    # A1-8 / task #55 — parallel RAW scan equivalence.
    # Asserts RawScan::Run(reader, partition, N) recovers EXACTLY the same file
    # set (rawOffset/ext/size) as the serial Run() for N = 2 / 4 / 8, including
    # chunk-boundary magics that exercise the 512-byte overlap + merge dedup.
    add_executable(test_rawscan_parallel
        test_rawscan_parallel.cpp
        ${PHOENIX_SECTOR_READER}
        ${CMAKE_SOURCE_DIR}/scan/RawScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveZip.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveMp4.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveImage.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveStructure.cpp
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    )
    phoenix_add_test(test_rawscan_parallel)
    target_compile_definitions(test_rawscan_parallel PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_rawscan_parallel PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME RawScanParallel COMMAND test_rawscan_parallel)

    # A1-10 — parallel RAW scan wired into the PRODUCTION carving service.
    # Verifies the config chain (UnifiedScanConfig -> MakeCarveConfig ->
    # CarveConfig -> RawScan::SetThreadCount), that parallel workers clone the
    # effective (custom / extension-filtered) SignatureDB instead of the
    # built-ins, the targetExtensions filter fix, callback thread affinity and
    # cancellation propagation. Links CarveModuleService.cpp itself so the
    # production wiring is compiled and exercised by CTest.
    add_executable(test_rawscan_parallel_wiring
        test_rawscan_parallel_wiring.cpp
        ${PHOENIX_SECTOR_READER}
        ${CMAKE_SOURCE_DIR}/scan/CarveModuleService.cpp
        ${CMAKE_SOURCE_DIR}/scan/RawScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveZip.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveMp4.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveImage.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveStructure.cpp
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    )
    phoenix_add_test(test_rawscan_parallel_wiring)
    target_compile_definitions(test_rawscan_parallel_wiring PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_rawscan_parallel_wiring PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME RawScanParallelWiring COMMAND test_rawscan_parallel_wiring)

    set(PHOENIX_RAWSCAN_TEST_SOURCES
        ${CMAKE_SOURCE_DIR}/scan/RawScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveZip.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveMp4.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveImage.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveStructure.cpp
    )

    add_executable(test_dftt_8_jpeg_search
        integration/test_dftt_8_jpeg_search.cpp
        ${CMAKE_SOURCE_DIR}/scan/EmbeddedJpegExtract.cpp
        ${CMAKE_SOURCE_DIR}/scan/EmbeddedJpegExtractHosts.cpp
        ${CMAKE_SOURCE_DIR}/scan/CfbReader.cpp
        ${CMAKE_SOURCE_DIR}/scan/third_party/miniz_tinfl.c
        ${CMAKE_SOURCE_DIR}/scan/third_party/miniz_alloc.c
        ${INTEGRATION_COMMON_SOURCES}
        ${PHOENIX_RAWSCAN_TEST_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_dftt_8_jpeg_search)
    target_compile_definitions(test_dftt_8_jpeg_search PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_dftt_8_jpeg_search PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME DFTT_8_JPEG_SEARCH COMMAND test_dftt_8_jpeg_search)

    add_executable(test_dftt_12_carve_ext2
        integration/test_dftt_12_carve_ext2.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${PHOENIX_RAWSCAN_TEST_SOURCES}
    )
    phoenix_add_test(test_dftt_12_carve_ext2)
    target_compile_definitions(test_dftt_12_carve_ext2 PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_dftt_12_carve_ext2 PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME DFTT_12_CARVE_EXT2 COMMAND test_dftt_12_carve_ext2)

    add_executable(test_dftt_14_iso9660_smoke
        integration/test_dftt_14_iso9660_smoke.cpp
        ${INTEGRATION_COMMON_SOURCES}
    )
    phoenix_add_test(test_dftt_14_iso9660_smoke)
    target_compile_definitions(test_dftt_14_iso9660_smoke PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_dftt_14_iso9660_smoke PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME DFTT_14_ISO9660_SMOKE COMMAND test_dftt_14_iso9660_smoke)

    add_executable(test_custom_carving_manifest
        integration/test_custom_carving_manifest.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/scan/RawScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveZip.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveMp4.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveImage.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveStructure.cpp
    )
    phoenix_add_test(test_custom_carving_manifest)
    target_compile_definitions(test_custom_carving_manifest PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_custom_carving_manifest PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME CustomCarving_ManifestGolden COMMAND test_custom_carving_manifest)

    add_executable(test_custom_carving_batch
        integration/test_custom_carving_batch.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/scan/RawScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveZip.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveMp4.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveImage.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveStructure.cpp
    )
    phoenix_add_test(test_custom_carving_batch)
    target_compile_definitions(test_custom_carving_batch PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_custom_carving_batch PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME CustomCarving_Batch_All COMMAND test_custom_carving_batch)
    set_tests_properties(CustomCarving_Batch_All PROPERTIES
        ENVIRONMENT "PHOENIX_CUSTOM_CARVE_MAX_IMAGES=1"
    )

    add_test(NAME CustomCarving_ManifestLayout
        COMMAND python "${CMAKE_SOURCE_DIR}/testdata/tools/Validate-CustomCarvingManifest.py"
            --repo-root "${CMAKE_SOURCE_DIR}"
    )

    add_executable(test_file_carving_l0
        integration/test_file_carving_l0.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/scan/RawScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveZip.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveMp4.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveImage.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveStructure.cpp
    )
    phoenix_add_test(test_file_carving_l0)
    target_compile_definitions(test_file_carving_l0 PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_file_carving_l0 PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME FileCarving_L0_Graphic COMMAND test_file_carving_l0)

    add_executable(test_file_carving_graphic
        integration/test_file_carving_graphic.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/scan/RawScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveZip.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveMp4.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveImage.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveStructure.cpp
    )
    phoenix_add_test(test_file_carving_graphic)
    target_compile_definitions(test_file_carving_graphic PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_file_carving_graphic PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME FileCarving_Graphic_All COMMAND test_file_carving_graphic)

    add_executable(test_digital_corpora_batch
        integration/test_digital_corpora_batch.cpp
        ${INTEGRATION_COMMON_SOURCES}
        ${CMAKE_SOURCE_DIR}/fs/fat/FatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/fat/FatStructs.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/exfat/ExfatParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ext4/Ext4Parser.cpp
        ${CMAKE_SOURCE_DIR}/disk/PartitionProbe.cpp
        ${CMAKE_SOURCE_DIR}/disk/PartitionRecoveryService.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    phoenix_add_test(test_digital_corpora_batch)
    target_compile_definitions(test_digital_corpora_batch PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_digital_corpora_batch PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME DigitalCorpora_Batch_All COMMAND test_digital_corpora_batch)
    set_tests_properties(DigitalCorpora_Batch_All PROPERTIES TIMEOUT 3600)

    add_test(NAME Testdata_EwfExportReady
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/testdata/tools/Expand-EwfToRaw.ps1"
            -RepoRoot "${CMAKE_SOURCE_DIR}"
            -Roots sleuthkit_test_data
            -InPlace
    )

    # Crash diagnostics (minidump + log bundle)
    add_executable(test_crash_diagnostics
        test_crash_diagnostics.cpp
        ${CMAKE_SOURCE_DIR}/crash/CrashHandler.cpp
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    )
    phoenix_add_test(test_crash_diagnostics)
    target_link_libraries(test_crash_diagnostics PRIVATE dbghelp)
    add_test(NAME CrashDiagnostics COMMAND test_crash_diagnostics)

    add_test(NAME RepairSamplesIntegrity
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/testdata/tools/Verify-RepairSamplesIntegrity.ps1"
            -RepoRoot "${CMAKE_SOURCE_DIR}"
    )

    endif() # WIN32 integration tests

    # Linux ext2/3/4 code path (PHOENIX_LINUX_EXT_FS_ONLY). Also built on Windows to validate *-ext.dd images.
    if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(LINUX_EXT_INTEGRATION_SOURCES
            ${PHOENIX_SECTOR_READER}
            ${CMAKE_SOURCE_DIR}/common/Utils.cpp
            ${CMAKE_SOURCE_DIR}/log/Logger.cpp
            ${CMAKE_SOURCE_DIR}/disk/PartitionProbe.cpp
            ${CMAKE_SOURCE_DIR}/disk/PartitionRecoveryService.cpp
            ${CMAKE_SOURCE_DIR}/fs/ext4/Ext4Parser.cpp
            ${CMAKE_SOURCE_DIR}/scan/FileSystemModuleService.cpp
            ${CMAKE_SOURCE_DIR}/fs/refs/RefsParser.cpp
            ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
            ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
            ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
            ${CMAKE_SOURCE_DIR}/recover/FragmentStitchService.cpp
            ${CMAKE_SOURCE_DIR}/recover/GraphCarveFragmentStitcher.cpp
            ${CMAKE_SOURCE_DIR}/recover/FatFragmentStitcher.cpp
            ${CMAKE_SOURCE_DIR}/recover/NtfsFragmentStitcher.cpp
            ${CMAKE_SOURCE_DIR}/recover/FormatPayloadScore.cpp
        )
        if(PHOENIX_PLATFORM_LIBS)
            set(LINUX_EXT_RECOVER_PLATFORM_LIBS ${PHOENIX_PLATFORM_LIBS})
        endif()

        add_executable(test_dfr_linux_batch
            integration/test_dfr_linux_batch.cpp
            ${LINUX_EXT_INTEGRATION_SOURCES}
        )
        phoenix_add_test(test_dfr_linux_batch)
        target_compile_definitions(test_dfr_linux_batch PRIVATE
            PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
            PHOENIX_TEST_LINUX_EXT_ONLY=1
        )
        if(LINUX_EXT_RECOVER_PLATFORM_LIBS)
            target_link_libraries(test_dfr_linux_batch PRIVATE ${LINUX_EXT_RECOVER_PLATFORM_LIBS})
        endif()
        add_test(NAME DFR_Linux_Batch COMMAND test_dfr_linux_batch)

        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_executable(test_cfreds_ext4_linux
                integration/test_cfreds_ext4.cpp
                ${LINUX_EXT_INTEGRATION_SOURCES}
            )
            phoenix_add_test(test_cfreds_ext4_linux)
            target_compile_definitions(test_cfreds_ext4_linux PRIVATE
                PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
                PHOENIX_LINUX_EXT_FS_ONLY=1
            )
            add_test(NAME CFReDS_EXT4_DFR01 COMMAND test_cfreds_ext4_linux)

            add_executable(test_partition_recovery_probe_linux
                integration/test_partition_recovery_probe.cpp
                ${LINUX_EXT_INTEGRATION_SOURCES}
            )
            phoenix_add_test(test_partition_recovery_probe_linux)
            target_compile_definitions(test_partition_recovery_probe_linux PRIVATE
                PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
                PHOENIX_TEST_LINUX_EXT_ONLY=1
            )
            add_test(NAME PartitionRecovery_ProbeImage COMMAND test_partition_recovery_probe_linux)
        endif()
    endif()

endif()

# Qt-dependent tests (grid preview, repair samples, optional media thumbs)
find_package(Qt5 QUIET COMPONENTS Core Gui Widgets)
# Headless builds must not link Qt targets: this machine only ships MSVC Qt
# import libs, so a MinGW link of GridPreviewExtensions / ZipEntryList fails
# with unresolved Qt5Core symbols.  GUI builds keep these tests registered.
if(Qt5_FOUND AND PHOENIX_BUILD_GUI)
    # Make Qt runtime DLLs available for ctest runs on Windows.
    # Otherwise tests may fail at process start with 0xc0000135 (missing Qt5*.dll / platform plugin).
    set(_phoenix_qt_test_path_prefix "")
    if(WIN32)
        # Prefer explicit Qt bin provided by user/environment.
        if(DEFINED ENV{PHOENIX_QT_BIN} AND NOT "$ENV{PHOENIX_QT_BIN}" STREQUAL "")
            set(_phoenix_qt_test_path_prefix "$ENV{PHOENIX_QT_BIN};")
        else()
            get_target_property(_qt5core_loc Qt5::Core LOCATION)
            if(_qt5core_loc)
                get_filename_component(_qt5_bin_dir "${_qt5core_loc}" DIRECTORY)
                set(_phoenix_qt_test_path_prefix "${_qt5_bin_dir};")
            endif()
        endif()
    endif()

    add_executable(test_grid_preview_extensions
        test_grid_preview_extensions.cpp
        ${CMAKE_SOURCE_DIR}/media/GridPreviewExtensions.cpp
    )
    phoenix_add_test(test_grid_preview_extensions)
    target_link_libraries(test_grid_preview_extensions PRIVATE Qt5::Core)
    add_test(NAME GridPreviewExtensions COMMAND test_grid_preview_extensions)
    if(WIN32 AND NOT _phoenix_qt_test_path_prefix STREQUAL "")
        set_tests_properties(GridPreviewExtensions PROPERTIES
            ENVIRONMENT "PATH=${_phoenix_qt_test_path_prefix}$ENV{PATH}"
        )
    endif()

    add_executable(test_zip_entry_list
        test_zip_entry_list.cpp
        ${CMAKE_SOURCE_DIR}/repair/ZipEntryList.cpp
    )
    phoenix_add_test(test_zip_entry_list)
    target_link_libraries(test_zip_entry_list PRIVATE Qt5::Core)
    add_test(NAME ZipEntryList_PreviewStructure COMMAND test_zip_entry_list)
    if(WIN32 AND NOT _phoenix_qt_test_path_prefix STREQUAL "")
        set_tests_properties(ZipEntryList_PreviewStructure PROPERTIES
            ENVIRONMENT "PATH=${_phoenix_qt_test_path_prefix}$ENV{PATH}"
        )
    endif()

    if(TARGET Qt5::Gui AND TARGET Qt5::Widgets)
        add_executable(test_media_thumbnails_smoke
            test_media_thumbnails_smoke.cpp
            ${CMAKE_SOURCE_DIR}/media/MediaThumbnails.cpp
            ${CMAKE_SOURCE_DIR}/media/FfmpegVideoThumb.cpp
            ${CMAKE_SOURCE_DIR}/media/WinShellVideoThumb.cpp
            ${CMAKE_SOURCE_DIR}/media/WinWicImageDecoder.cpp
            ${CMAKE_SOURCE_DIR}/log/Logger.cpp
            ${CMAKE_SOURCE_DIR}/common/Utils.cpp
        )
        phoenix_add_test(test_media_thumbnails_smoke)
        target_link_libraries(test_media_thumbnails_smoke PRIVATE Qt5::Core Qt5::Gui Qt5::Widgets)
        if(WIN32)
            target_link_libraries(test_media_thumbnails_smoke PRIVATE ole32 windowscodecs uuid)
        endif()
        # NOTE: This is a GUI smoke executable. In many dev/CI environments, Qt runtime
        # DLLs / platform plugins (qwindows.dll) are not on PATH for ctest runs, which
        # causes 0xc0000135 at startup. Keep it buildable, but opt-in for ctest.
        if(DEFINED ENV{PHOENIX_RUN_MEDIA_THUMBNAILS_SMOKE} AND "$ENV{PHOENIX_RUN_MEDIA_THUMBNAILS_SMOKE}" STREQUAL "1")
            add_test(NAME MediaThumbnailsSmoke COMMAND test_media_thumbnails_smoke)
            if(WIN32 AND NOT _phoenix_qt_test_path_prefix STREQUAL "")
                set_tests_properties(MediaThumbnailsSmoke PROPERTIES
                    ENVIRONMENT "PATH=${_phoenix_qt_test_path_prefix}$ENV{PATH}"
                )
            endif()
        endif()
    endif()

    add_executable(test_repair_samples
        test_repair_samples.cpp
        ${CMAKE_SOURCE_DIR}/repair/RepairManager.cpp
        ${CMAKE_SOURCE_DIR}/repair/RepairValidate.cpp
        ${CMAKE_SOURCE_DIR}/repair/SqliteRecoverService.cpp
        ${CMAKE_SOURCE_DIR}/repair/SqliteRepair.cpp
        ${CMAKE_SOURCE_DIR}/repair/ZipEntryList.cpp
        ${CMAKE_SOURCE_DIR}/repair/OfficeZipRepair.cpp
        ${CMAKE_SOURCE_DIR}/repair/RepairFfmpeg.cpp
        ${CMAKE_SOURCE_DIR}/repair/ZipRepair.cpp
        ${CMAKE_SOURCE_DIR}/repair/JpegRepair.cpp
        ${CMAKE_SOURCE_DIR}/repair/PngRepair.cpp
        ${CMAKE_SOURCE_DIR}/repair/GifRepair.cpp
        ${CMAKE_SOURCE_DIR}/repair/Mp4Repair.cpp
        ${CMAKE_SOURCE_DIR}/repair/FfmpegVideoRepair.cpp
        ${CMAKE_SOURCE_DIR}/repair/FfmpegImageRepair.cpp
        ${CMAKE_SOURCE_DIR}/repair/FfmpegRawRepair.cpp
        ${CMAKE_SOURCE_DIR}/repair/RepairFormatUtil.cpp
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    )
    phoenix_add_test(test_repair_samples)
    target_link_libraries(test_repair_samples PRIVATE Qt5::Core)
    target_compile_definitions(test_repair_samples PRIVATE
        PHOENIX_SOURCE_DIR=L"${CMAKE_SOURCE_DIR}"
        PHOENIX_BUILD_GUI=0
    )
    if(PHOENIX_HAVE_LIBZIP)
        find_package(libzip REQUIRED)
        target_compile_definitions(test_repair_samples PRIVATE PHOENIX_HAVE_LIBZIP=1)
        target_link_libraries(test_repair_samples PRIVATE zip)
    endif()
    add_test(NAME RepairSamples COMMAND test_repair_samples)
    if(WIN32 AND NOT _phoenix_qt_test_path_prefix STREQUAL "")
        set_tests_properties(RepairSamples PROPERTIES
            ENVIRONMENT "PATH=${_phoenix_qt_test_path_prefix}$ENV{PATH}"
        )
    endif()
endif()



# --- A0 write-path safety base: WriteGuard (header-only, no Qt, no device I/O) ---
add_executable(test_write_guard_default_deny test_write_guard.cpp)
phoenix_add_test(test_write_guard_default_deny)
add_test(NAME WriteGuardDefaultDeny COMMAND test_write_guard_default_deny)

add_executable(test_write_guard_logic test_write_guard.cpp)
phoenix_add_test(test_write_guard_logic)
target_compile_definitions(test_write_guard_logic PRIVATE PHOENIX_ENABLE_DEVICE_WRITES=1)
add_test(NAME WriteGuard COMMAND test_write_guard_logic)


# --- A0-3 transaction journal + device-writer capability chain (zero I/O) ---
add_executable(test_a0_transaction_logic test_a0_transaction.cpp)
phoenix_add_test(test_a0_transaction_logic)
target_compile_definitions(test_a0_transaction_logic PRIVATE PHOENIX_ENABLE_DEVICE_WRITES=1)
add_test(NAME A0Transaction COMMAND test_a0_transaction_logic)

add_executable(test_a0_transaction_default test_a0_transaction.cpp)
phoenix_add_test(test_a0_transaction_default)
add_test(NAME A0TransactionDefaultDeny COMMAND test_a0_transaction_default)


# --- A1-1a read-only virtual disk sector mapping (no Qt, no device I/O) ---
add_executable(test_virtual_disk_source test_virtual_disk_source.cpp)
phoenix_add_test(test_virtual_disk_source)
add_test(NAME VirtualDiskSource COMMAND test_virtual_disk_source)


# --- A1-1b extended virtual disk formats (VHDX/VMDK sparse/QCOW2/VDI) ---
add_executable(test_virtual_disk_extended test_virtual_disk_extended.cpp)
phoenix_add_test(test_virtual_disk_extended)
add_test(NAME VirtualDiskExtended COMMAND test_virtual_disk_extended)


# --- A1-1c real-image golden regression (independent Python oracle) ---
add_executable(test_virtual_disk_golden test_virtual_disk_golden.cpp)
phoenix_add_test(test_virtual_disk_golden)
add_test(NAME VirtualDiskGolden COMMAND test_virtual_disk_golden)


# --- A1-2 virtual disk scan-source wiring (SectorReader + real golden corpus) ---
add_executable(test_virtual_disk_sector_source
    test_virtual_disk_sector_source.cpp
    ${PHOENIX_SECTOR_READER}
    ${CMAKE_SOURCE_DIR}/log/Logger.cpp
    ${CMAKE_SOURCE_DIR}/common/Utils.cpp
)
phoenix_add_test(test_virtual_disk_sector_source)
if(WIN32)
    target_link_libraries(test_virtual_disk_sector_source PRIVATE ${PHOENIX_PLATFORM_LIBS})
endif()
add_test(NAME VirtualDiskSectorSource COMMAND test_virtual_disk_sector_source)


# --- A1-4a read-only virtual RAID reassembly (independent forward oracle) ---
add_executable(test_virtual_raid test_virtual_raid.cpp)
phoenix_add_test(test_virtual_raid)
add_test(NAME VirtualRaid COMMAND test_virtual_raid)


# --- A1-3 BitLocker read-only crypto primitives (FIPS vectors + Python oracle) ---
add_executable(test_bitlocker_crypto test_bitlocker_crypto.cpp)
phoenix_add_test(test_bitlocker_crypto)
add_test(NAME BitLockerCrypto COMMAND test_bitlocker_crypto)


# --- A1-3 BitLocker read-only volume unlock (synthetic golden corpus) ---
add_executable(test_bitlocker_volume test_bitlocker_volume.cpp)
phoenix_add_test(test_bitlocker_volume)
add_test(NAME BitLockerVolume COMMAND test_bitlocker_volume)


# --- A1-5a ReFS integrity primitives (CRC32-C / CRC64-NVME / SHA-256 / VBR) ---
add_executable(test_refs_crypto test_refs_crypto.cpp)
phoenix_add_test(test_refs_crypto)
add_test(NAME RefsCrypto COMMAND test_refs_crypto)


# --- A1-5a ReFS read-only volume parser (synthetic golden corpus) ---
add_executable(test_refs_volume test_refs_volume.cpp)
phoenix_add_test(test_refs_volume)
add_test(NAME RefsVolume COMMAND test_refs_volume)


# --- A1-4b-a mdadm superblocks + conservative RAID layout probe ---
add_executable(test_raid_probe test_raid_probe.cpp)
phoenix_add_test(test_raid_probe)
add_test(NAME RaidProbe COMMAND test_raid_probe)


# --- A1-4b-a configuration-driven, non-GUI composed read-only source ---
add_executable(test_scan_source_chain test_scan_source_chain.cpp)
phoenix_add_test(test_scan_source_chain)
add_test(NAME ScanSourceChain COMMAND test_scan_source_chain)


# --- A1-6 / parallel-track: streaming Aho-Corasick multi-pattern signature matcher ---
add_executable(test_signature_matcher test_signature_matcher.cpp)
phoenix_add_test(test_signature_matcher)
add_test(NAME SignatureMatcher COMMAND test_signature_matcher)


# --- A1-11: optional standalone RAW-scan parallel throughput benchmark ---
# NOT registered with CTest (timing assertions would be flaky). Enable with
# cmake -DPHOENIX_BUILD_BENCH=ON, then build target `bench_rawscan_parallel`.
# Results are recorded manually in docs/PERFORMANCE_BASELINE.md section 8.
# ── Real damaged disk headless driver (opt-in; never in default build/CTest) ──
# Zero GUI changes. The driver opens PhysicalDriveN with GENERIC_READ only and
# feeds the LDM (0x42) NTFS volumes to the unmodified engine by constructing
# Partition explicitly. Kept behind an option so normal regression builds are
# byte-for-byte unaffected.
# --- A1-12: opt-in SectorReader read-ahead is a PURE cache (default OFF) ---
# Always built: guards the zero-behaviour-change guarantee for GUI/CTests.
if(PHOENIX_SECTOR_READER)
    add_executable(test_sectorreader_readahead test_sectorreader_readahead.cpp
        ${PHOENIX_SECTOR_READER}
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    )
    phoenix_add_test(test_sectorreader_readahead)
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(test_sectorreader_readahead PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
    add_test(NAME SectorReaderReadAhead COMMAND test_sectorreader_readahead)
endif()

option(PHOENIX_BUILD_REALDISK_TOOLS "Build headless real-disk recovery driver" OFF)
if(PHOENIX_BUILD_REALDISK_TOOLS AND PHOENIX_SECTOR_READER AND WIN32)
    add_executable(phoenix_headless
        ${CMAKE_SOURCE_DIR}/tools/phoenix_headless.cpp
        ${PHOENIX_SECTOR_READER}
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
        ${CMAKE_SOURCE_DIR}/common/UiFileTypeCatalog.cpp
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/recover/FormatPayloadScore.cpp
        ${CMAKE_SOURCE_DIR}/recover/FatFragmentStitcher.cpp
        ${CMAKE_SOURCE_DIR}/recover/NtfsFragmentStitcher.cpp
        ${CMAKE_SOURCE_DIR}/recover/FragmentStitchService.cpp
        ${CMAKE_SOURCE_DIR}/recover/GraphCarveFragmentStitcher.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/scan/ExtensionCoverage.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/NtfsParser.cpp
        ${CMAKE_SOURCE_DIR}/fs/ntfs/UsnJournalParser.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverEngine.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverOutputFile.cpp
        ${CMAKE_SOURCE_DIR}/recover/RecoverTargetPath.cpp
    )
    # P0-6: the GUI-less driver still links the PRODUCTION signature database,
    # so file-format matching is engine code, not an orchestrator guess.
    target_compile_definitions(phoenix_headless PRIVATE PHOENIX_BUILD_GUI=0)
    phoenix_add_test(phoenix_headless)
    # A live real-disk batch holds phoenix_headless.exe open on Windows, so a
    # relink cannot overwrite it. P0-6 built the SAME sources under a new
    # output name; the old binary stays as a read-only snapshot of batch 4.
    # P0-7 adds $ATTRIBUTE_LIST support, so build yet another immutable
    # snapshot name while the P0-6 loop is still running batches 5-7.
    set_target_properties(phoenix_headless PROPERTIES
        OUTPUT_NAME "phoenix_headless_p07")
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(phoenix_headless PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()

    # P0-13: bad-block-tolerant region imager. Same production SectorReader, but
    # strict policy so a chunk fails fast at the first unreadable sector instead
    # of grinding every sector through the device timeout. Used to image the two
    # NTFS volumes of the failing disk one range at a time (a whole-disk image
    # does not fit: 931.51 GiB source vs 661 + 137 GB free).
    add_executable(image_region
        ${CMAKE_SOURCE_DIR}/tools/image_region.cpp
        ${PHOENIX_SECTOR_READER}
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
    )
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(image_region PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
endif()

if(PHOENIX_BUILD_BENCH AND PHOENIX_SECTOR_READER)
    add_executable(bench_rawscan_parallel
        ${CMAKE_SOURCE_DIR}/tools/bench_rawscan_parallel.cpp
        ${PHOENIX_SECTOR_READER}
        ${CMAKE_SOURCE_DIR}/scan/CarveModuleService.cpp
        ${CMAKE_SOURCE_DIR}/scan/RawScan.cpp
        ${CMAKE_SOURCE_DIR}/scan/SignatureDB.cpp
        ${CMAKE_SOURCE_DIR}/scan/UiTypeSignatureSupplement.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveZip.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveMp4.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveImage.cpp
        ${CMAKE_SOURCE_DIR}/scan/carve/CarveStructure.cpp
        ${CMAKE_SOURCE_DIR}/log/Logger.cpp
        ${CMAKE_SOURCE_DIR}/common/Utils.cpp
    )
    set_target_properties(bench_rawscan_parallel PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
    )
    target_include_directories(bench_rawscan_parallel PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(bench_rawscan_parallel PRIVATE PHOENIX_BUILD_GUI=0)
    if(PHOENIX_PLATFORM_LIBS)
        target_link_libraries(bench_rawscan_parallel PRIVATE ${PHOENIX_PLATFORM_LIBS})
    endif()
endif()
