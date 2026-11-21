#pragma once

#include <QtCore/QtGlobal>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QVector>

// UI 缩略图加载策略（大字节仅在异步读取后存在于内存，不常驻于 DTO）
enum class ScanPreviewKind : quint8 {
    None = 0,
    ResidentBlob = 1, // 旁路表 previewResidentBlobs_
    VolumeSlice = 2,  // previewVolumeOffset + previewReadLength
    FullNode = 3        // 旁路表 previewFileNodes_，需 ReadFileDataPrefix
};

// 扫描结果在 UI 线程使用的轻量描述（由控制层从 FileNode 转换）
struct ScanFileDto {
    quint64 id = 0;
    QString name;
    QString fullPath;
    QString relativePath;
    QString extension;
    QString fsType;
    qint64 sizeBytes = 0;
    quint64 parentId = 0;
    bool isDirectory = false;
    bool isDeleted = false;
    bool isReconstructed = false;
    quint64 modifyTimeFt = 0; // Windows FILETIME（100ns）

    ScanPreviewKind previewKind = ScanPreviewKind::None;
    quint64 previewVolumeOffset = 0;
    quint32 previewReadLength = 0;
};

Q_DECLARE_METATYPE(ScanFileDto)
Q_DECLARE_METATYPE(QVector<ScanFileDto>)
