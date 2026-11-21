#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QHash>
#include <QtCore/QReadWriteLock>
#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QVector>

#include "../disk/Partition.h"
#include "../recover/FileNode.h"
#include "ScanTypes.h"

class ScanResultStore
{
public:
    void clear();
    bool upsert(const ScanFileDto &dto);
    quint64 revision() const;
    int fileCount() const;

    bool tryGetFile(quint64 id, ScanFileDto *out) const;
    QVector<ScanFileDto> filesByInsertOrder(int offset, int count) const;
    QVector<QString> typeCategories() const;
    QStringList pathChildDirectories(const QString &parentRel) const;
    int pathNodeCount(const QString &relPath) const;
    qint64 pathDirectFileBytes(const QString &parentRel) const;
    QVector<quint64> pathDirectChildIds(const QString &parentRel) const;
    QStringList typeCategoryChildren(const QString &category) const;
    int typeNodeCount(const QString &key) const;
    QVector<quint64> typeNodeFileIds(const QString &key) const;

    void setScanVolumePartition(const Partition &partition);
    bool tryGetScanVolumePartition(Partition *out) const;

    void setPreviewResidentBlob(quint64 id, const QByteArray &bytes);
    void setPreviewFileNodeSnapshot(quint64 id, const FileNode &node);
    bool tryGetPreviewResidentBlob(quint64 id, QByteArray *out) const;
    bool tryGetPreviewFileNode(quint64 id, FileNode *out) const;
    void setRecoveredFileNode(quint64 id, const FileNode &node);
    bool tryGetRecoveredFileNode(quint64 id, FileNode *out) const;
    QHash<quint64, FileNode> allRecoveredFileNodes() const;

private:
    void indexPathLocked(const ScanFileDto &dto);
    void indexTypeLocked(const ScanFileDto &dto);
    void rebuildIndexesLocked();
    void syncPreviewSupplementsLocked(const ScanFileDto &dto);

    mutable QReadWriteLock lock_;
    quint64 revision_ = 0;
    QHash<quint64, ScanFileDto> files_;
    QVector<quint64> insertOrder_;
    Partition scanVolume_;
    bool hasScanVolume_ = false;
    QHash<quint64, QByteArray> previewResidentBlobs_;
    QHash<quint64, FileNode> previewFileNodes_;
    QHash<quint64, FileNode> recoveredFileNodes_;
    QHash<QString, QStringList> pathChildDirectories_;
    QHash<QString, int> pathNodeCounts_;
    QHash<QString, QVector<quint64> > pathDirectChildIds_;
    QHash<QString, QStringList> typeCategoryChildren_;
    QHash<QString, int> typeNodeCounts_;
    QHash<QString, QVector<quint64> > typeNodeFileIds_;
    QSet<QString> typeCategories_;
};
