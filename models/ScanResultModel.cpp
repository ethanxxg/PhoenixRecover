#include "ScanResultModel.h"

#include "../controllers/ScanResultStore.h"
#include "../log/Logger.h"

#include <QtCore/QDateTime>
#include <QtCore/QLocale>
#include <QtGui/QColor>

ScanResultModel::ScanResultModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int ScanResultModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : rows_.size();
}

int ScanResultModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColCount;
}

QVariant ScanResultModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= rows_.size())
        return {};

    const auto &dto = rows_.at(index.row());

    if (role == Qt::CheckStateRole && index.column() == ColCheckBox)
        return checkedIds_.contains(dto.id) ? Qt::Checked : Qt::Unchecked;

    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case ColName:           return dto.name;
        case ColSize:           return dto.isDirectory ? QStringLiteral("\u2014") : formatSize(dto.sizeBytes);
        case ColType:           return dto.isDirectory ? QStringLiteral("\u6587\u4ef6\u5939") : fileTypeDisplay(dto);
        case ColModified:       return dto.isDirectory ? QStringLiteral("\u2014") : formatFileTime(dto.modifyTimeFt);
        case ColRecoverability: return recoverabilityText(dto);
        case ColPath:           return dto.relativePath;
        case ColPhase:          return phaseText(dto);
        default:                return {};
        }
    }

    if (role == Qt::ForegroundRole && index.column() == ColRecoverability) {
        return recoverabilityColor(dto);
    }

    if (role == FileIdRole)        return QVariant::fromValue(dto.id);
    if (role == IsDirectoryRole)   return dto.isDirectory;
    if (role == RelativePathRole)  return dto.relativePath;
    if (role == ExtensionRole)     return dto.extension;
    if (role == SizeBytesRole)     return QVariant::fromValue(dto.sizeBytes);
    if (role == ModifyTimeFtRole)  return QVariant::fromValue(dto.modifyTimeFt);
    if (role == PreviewKindRole)   return static_cast<int>(dto.previewKind);
    if (role == RecoverabilityRole) {
        return dto.isDeleted ? 1 : (dto.isReconstructed ? 2 : 3);
    }

    return {};
}

QVariant ScanResultModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case ColCheckBox:      return {};
    case ColName:          return tr("\u6587\u4ef6\u540d");
    case ColSize:          return tr("\u5927\u5c0f");
    case ColType:          return tr("\u7c7b\u578b");
    case ColModified:      return tr("\u4fee\u6539\u65f6\u95f4");
    case ColRecoverability:return tr("\u72b6\u6001");
    case ColPath:          return tr("\u8def\u5f84");
    case ColPhase:         return tr("\u53d1\u73b0\u9636\u6bb5");
    default:               return {};
    }
}

Qt::ItemFlags ScanResultModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags f = QAbstractTableModel::flags(index);
    if (index.isValid() && index.column() == ColCheckBox) {
        f |= Qt::ItemIsUserCheckable;
    }
    return f;
}

bool ScanResultModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() >= rows_.size())
        return false;

    if (role == Qt::CheckStateRole && index.column() == ColCheckBox) {
        const auto &dto = rows_.at(index.row());
        const bool checked = (value.toInt() == Qt::Checked);
        if (checked)
            checkedIds_.insert(dto.id);
        else
            checkedIds_.remove(dto.id);
        emit dataChanged(index, index, {Qt::CheckStateRole});
        emit checkedCountChanged(checkedIds_.size(), rows_.size());
        return true;
    }
    return false;
}

void ScanResultModel::appendBatch(const QVector<ScanFileDto> &batch)
{
    if (batch.isEmpty()) return;

    const int first = rows_.size();
    const int last  = first + batch.size() - 1;

    beginInsertRows(QModelIndex(), first, last);
    rows_.append(batch);
    endInsertRows();
}

void ScanResultModel::setResultStore(ScanResultStore *store)
{
    store_ = store;
}

void ScanResultModel::clear()
{
    if (rows_.isEmpty()) return;
    beginResetModel();
    rows_.clear();
    checkedIds_.clear();
    lastSyncRevision_ = 0;
    endResetModel();
    emit checkedCountChanged(0, 0);
}

void ScanResultModel::setChecked(int row, bool checked)
{
    if (row < 0 || row >= rows_.size()) return;
    setData(index(row, ColCheckBox),
            checked ? Qt::Checked : Qt::Unchecked,
            Qt::CheckStateRole);
}

bool ScanResultModel::isChecked(int row) const
{
    if (row < 0 || row >= rows_.size()) return false;
    return checkedIds_.contains(rows_.at(row).id);
}

QVector<quint64> ScanResultModel::checkedIds() const
{
    QVector<quint64> ids;
    ids.reserve(checkedIds_.size());
    for (auto it = checkedIds_.cbegin(); it != checkedIds_.cend(); ++it) {
        ids.append(*it);
    }
    return ids;
}

void ScanResultModel::setAllChecked(bool checked)
{
    if (rows_.isEmpty()) return;

    if (checked) {
        for (const auto &dto : rows_) {
            checkedIds_.insert(dto.id);
        }
    } else {
        checkedIds_.clear();
    }
    emit dataChanged(index(0, ColCheckBox),
                     index(rows_.size() - 1, ColCheckBox),
                     {Qt::CheckStateRole});
    emit checkedCountChanged(checkedIds_.size(), rows_.size());
}

int ScanResultModel::checkedCount() const
{
    return checkedIds_.size();
}

const ScanFileDto *ScanResultModel::dtoAtRow(int row) const
{
    if (row < 0 || row >= rows_.size()) return nullptr;
    return &rows_.at(row);
}

void ScanResultModel::onStoreRevisionChanged(quint64 revision)
{
    if (!store_ || revision <= desiredSyncRevision_) {
        return;
    }
    desiredSyncRevision_ = revision;
    if (syncScheduled_) {
        return;
    }
    syncScheduled_ = true;
    QMetaObject::invokeMethod(this, [this]() { syncFromStoreStep(); }, Qt::QueuedConnection);
}

void ScanResultModel::syncFromStoreStep()
{
    if (!store_) {
        syncScheduled_ = false;
        return;
    }

    const int storeCount = store_->fileCount();
    const int localCount = rows_.size();
    if (storeCount <= localCount) {
        // Fully caught up to current store snapshot.
        lastSyncRevision_ = (std::max)(lastSyncRevision_, desiredSyncRevision_);
        syncScheduled_ = false;
        return;
    }

    // Chunked sync to keep UI responsive on 1e5+ results.
    constexpr int kMaxAppendPerTick = 2000;
    const int diff = storeCount - localCount;
    const int take = diff > kMaxAppendPerTick ? kMaxAppendPerTick : diff;

    QVector<ScanFileDto> batch = store_->filesByInsertOrder(localCount, take);
    if (!batch.isEmpty()) {
        appendBatch(batch);
    }

    // Continue until caught up.
    QMetaObject::invokeMethod(this, [this]() { syncFromStoreStep(); }, Qt::QueuedConnection);
}

QString ScanResultModel::formatSize(qint64 bytes)
{
    if (bytes < 0) return QStringLiteral("0 B");
    if (bytes < 1024) return QStringLiteral("%1 B").arg(bytes);
    if (bytes < 1024 * 1024)
        return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024)
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

QString ScanResultModel::formatFileTime(quint64 fileTime100ns)
{
    if (fileTime100ns == 0) return QStringLiteral("\u2014");
    const qint64 epoch100ns = 116444736000000000LL;
    const qint64 msSinceEpoch = static_cast<qint64>((fileTime100ns - epoch100ns) / 10000);
    return QLocale::system().toString(
        QDateTime::fromMSecsSinceEpoch(msSinceEpoch),
        QStringLiteral("yyyy-MM-dd HH:mm"));
}

QString ScanResultModel::fileTypeDisplay(const ScanFileDto &dto)
{
    if (dto.extension.isEmpty()) return QStringLiteral("\u5176\u4ed6");
    return dto.extension.toUpper();
}

QString ScanResultModel::recoverabilityText(const ScanFileDto &dto)
{
    if (dto.isDeleted)       return QStringLiteral("\u5df2\u5220\u9664");
    if (dto.isReconstructed) return QStringLiteral("\u5df2\u91cd\u5efa");
    return QStringLiteral("\u6b63\u5e38");
}

QColor ScanResultModel::recoverabilityColor(const ScanFileDto &dto)
{
    if (dto.isDeleted) return QColor(0xef, 0x44, 0x44);
    if (dto.isReconstructed) return QColor(0xf9, 0x73, 0x16);
    return QColor(0x22, 0xc5, 0x5e);
}

QString ScanResultModel::phaseText(const ScanFileDto &dto)
{
    if (dto.isReconstructed) return QStringLiteral("\u6df1\u5ea6\u626b\u63cf");
    return QStringLiteral("\u5feb\u901f\u626b\u63cf");
}
