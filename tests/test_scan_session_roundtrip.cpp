#include "scan/ScanSerializer.h"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool sameNode(const FileNode &a, const FileNode &b)
{
    return a.id == b.id &&
           a.name == b.name &&
           a.originalPath == b.originalPath &&
           a.extension == b.extension &&
           a.size == b.size &&
           a.createTime == b.createTime &&
           a.modifyTime == b.modifyTime &&
           a.accessTime == b.accessTime &&
           a.status == b.status &&
           a.recoverability == b.recoverability &&
           a.isDirectory == b.isDirectory &&
           a.fsType == b.fsType &&
           a.clusters == b.clusters &&
           a.rawOffset == b.rawOffset &&
           a.rawSize == b.rawSize &&
           a.residentData == b.residentData &&
           a.isNtfsCompressed == b.isNtfsCompressed &&
           a.compressionUnit == b.compressionUnit &&
           a.parentId == b.parentId;
}

} // namespace

int main()
{
    namespace fs = std::filesystem;

    const fs::path root = fs::temp_directory_path() / "phoenix_session_roundtrip_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    check(!ec, "create isolated temporary test directory");
    if (failures != 0) {
        return failures;
    }

    const fs::path exported = root / "roundtrip.prs";

    Partition partition;
    partition.physicalDiskIdx = 7;
    partition.startSector = 2048;
    partition.totalSectors = 65536;
    partition.sizeBytes = partition.totalSectors * 512;
    partition.fsType = L"NTFS";
    partition.driveLetter = L"TEST:";
    partition.label = L"Phoenix test volume";

    FileNode directory;
    directory.id = 1;
    directory.name = L"Recovered";
    directory.originalPath = L"TEST:\\Recovered";
    directory.extension = L"";
    directory.isDirectory = true;
    directory.fsType = L"NTFS";
    directory.status = FileStatus::Normal;
    directory.recoverability = RecoverabilityLevel::Excellent;

    FileNode file;
    file.id = 2;
    file.name = L"报告.txt";
    file.originalPath = L"TEST:\\Recovered\\报告.txt";
    file.extension = L"txt";
    file.size = 6;
    file.createTime = 11;
    file.modifyTime = 22;
    file.accessTime = 33;
    file.status = FileStatus::Deleted;
    file.recoverability = RecoverabilityLevel::Good;
    file.fsType = L"NTFS";
    file.parentId = directory.id;
    file.clusters = {100, 101, 105};
    file.residentData = {0x50, 0x68, 0x6f, 0x65, 0x6e, 0x69, 0x78};
    file.isNtfsCompressed = true;
    file.compressionUnit = 16;

    ScanResult source;
    source.scannedBytes = 123456;
    source.foundCount = 2;
    source.totalRecords = 3;
    source.inUseCount = 1;
    source.duration = std::chrono::milliseconds(9876);
    source.files = {directory, file};

    check(ScanSerializer::Save(exported.wstring(), source, partition),
          "save session to isolated temporary file");
    check(fs::is_regular_file(exported), "exported session exists");

    ScanResult loaded;
    Partition loadedPartition;
    check(ScanSerializer::Load(exported.wstring(), loaded, loadedPartition),
          "load exported session");
    check(loadedPartition.physicalDiskIdx == partition.physicalDiskIdx,
          "partition physical disk survives round trip");
    check(loadedPartition.startSector == partition.startSector &&
              loadedPartition.totalSectors == partition.totalSectors &&
              loadedPartition.sizeBytes == partition.sizeBytes,
          "partition geometry survives round trip");
    check(loadedPartition.fsType == partition.fsType &&
              loadedPartition.driveLetter == partition.driveLetter &&
              loadedPartition.label == partition.label,
          "partition labels survive round trip");
    check(loaded.scannedBytes == source.scannedBytes &&
              loaded.foundCount == source.foundCount &&
              loaded.totalRecords == source.totalRecords &&
              loaded.inUseCount == source.inUseCount &&
              loaded.duration == source.duration,
          "scan metadata survives round trip");
    check(loaded.files.size() == source.files.size(), "file count survives round trip");
    if (loaded.files.size() == source.files.size()) {
        check(sameNode(loaded.files[0], source.files[0]), "directory node survives round trip");
        check(sameNode(loaded.files[1], source.files[1]), "file node survives round trip");
    }

    std::error_code removeError;
    fs::remove_all(root, removeError);
    check(!removeError, "remove only this test's temporary directory");
    if (removeError) {
        std::cerr << "NOTE: temporary cleanup failed: " << root.string() << '\n';
    }

    if (failures == 0) {
        std::cout << "OK: scan session save/load round trip\n";
    }
    return failures;
}
