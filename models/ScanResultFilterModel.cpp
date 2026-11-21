#include "ScanResultFilterModel.h"
#include "ScanResultModel.h"

ScanResultFilterModel::ScanResultFilterModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    setDynamicSortFilter(true);
}

void ScanResultFilterModel::setNameFilter(const QString &keyword)
{
    if (nameKeyword_ == keyword) return;
    nameKeyword_ = keyword;
    invalidateFilter();
}

void ScanResultFilterModel::setExtensionFilter(const QStringList &extensions)
{
    if (extensionFilter_ == extensions) return;
    extensionFilter_ = extensions;
    invalidateFilter();
}

void ScanResultFilterModel::setSizeRange(qint64 minBytes, qint64 maxBytes)
{
    if (minSize_ == minBytes && maxSize_ == maxBytes) return;
    minSize_ = minBytes;
    maxSize_ = maxBytes;
    invalidateFilter();
}

void ScanResultFilterModel::setDateRange(const QDateTime &from, const QDateTime &to)
{
    if (dateFrom_ == from && dateTo_ == to) return;
    dateFrom_ = from;
    dateTo_ = to;
    invalidateFilter();
}

void ScanResultFilterModel::setRecoverabilityFilter(int minLevel)
{
    if (minRecoverability_ == minLevel) return;
    minRecoverability_ = minLevel;
    invalidateFilter();
}

void ScanResultFilterModel::setPathPrefix(const QString &relPath)
{
    if (pathPrefix_ == relPath) return;
    pathPrefix_ = relPath;
    invalidateFilter();
}

void ScanResultFilterModel::setTypeCategory(const QString &category)
{
    if (typeCategory_ == category) return;
    typeCategory_ = category;
    invalidateFilter();
}

void ScanResultFilterModel::clearAllFilters()
{
    nameKeyword_.clear();
    extensionFilter_.clear();
    minSize_ = -1;
    maxSize_ = -1;
    dateFrom_ = QDateTime();
    dateTo_ = QDateTime();
    minRecoverability_ = -1;
    pathPrefix_.clear();
    typeCategory_.clear();
    invalidateFilter();
}

bool ScanResultFilterModel::filterAcceptsRow(int sourceRow,
                                              const QModelIndex &sourceParent) const
{
    auto idx = [&](int col) {
        return sourceModel()->index(sourceRow, col, sourceParent);
    };

    if (!nameKeyword_.isEmpty()) {
        const QString name = idx(ScanResultModel::ColName)
                                 .data(Qt::DisplayRole).toString();
        if (!name.contains(nameKeyword_, Qt::CaseInsensitive))
            return false;
    }

    if (!extensionFilter_.isEmpty()) {
        const QString ext = idx(ScanResultModel::ColName)
                                .data(ScanResultModel::ExtensionRole).toString();
        bool found = false;
        for (const QString &e : extensionFilter_) {
            if (e.compare(ext, Qt::CaseInsensitive) == 0) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }

    if (minSize_ >= 0 || maxSize_ >= 0) {
        const qint64 size = idx(ScanResultModel::ColSize)
                                .data(ScanResultModel::SizeBytesRole).toLongLong();
        if (minSize_ >= 0 && size < minSize_) return false;
        if (maxSize_ >= 0 && size > maxSize_) return false;
    }

    if (dateFrom_.isValid() || dateTo_.isValid()) {
        const quint64 ft = idx(ScanResultModel::ColModified)
                               .data(ScanResultModel::ModifyTimeFtRole).toULongLong();
        if (ft > 0) {
            const qint64 epoch100ns = 116444736000000000LL;
            const qint64 msSinceEpoch = static_cast<qint64>((ft - epoch100ns) / 10000);
            const QDateTime dt = QDateTime::fromMSecsSinceEpoch(msSinceEpoch);
            if (dateFrom_.isValid() && dt < dateFrom_) return false;
            if (dateTo_.isValid() && dt > dateTo_) return false;
        }
    }

    if (!pathPrefix_.isEmpty()) {
        const QString path = idx(ScanResultModel::ColPath)
                                 .data(ScanResultModel::RelativePathRole).toString();
        if (!path.startsWith(pathPrefix_, Qt::CaseInsensitive)) {
            if (pathPrefix_.compare(path, Qt::CaseInsensitive) != 0)
                return false;
        }
    }

    if (!typeCategory_.isEmpty()) {
        const QString ext = idx(ScanResultModel::ColName)
                                .data(ScanResultModel::ExtensionRole).toString();
        Q_UNUSED(ext);
    }

    return true;
}

bool ScanResultFilterModel::lessThan(const QModelIndex &left,
                                      const QModelIndex &right) const
{
    const int col = left.column();

    switch (col) {
    case ScanResultModel::ColSize: {
        const qint64 l = left.data(ScanResultModel::SizeBytesRole).toLongLong();
        const qint64 r = right.data(ScanResultModel::SizeBytesRole).toLongLong();
        return l < r;
    }
    case ScanResultModel::ColModified: {
        const quint64 l = left.data(ScanResultModel::ModifyTimeFtRole).toULongLong();
        const quint64 r = right.data(ScanResultModel::ModifyTimeFtRole).toULongLong();
        return l < r;
    }
    case ScanResultModel::ColRecoverability: {
        const int l = left.data(ScanResultModel::RecoverabilityRole).toInt();
        const int r = right.data(ScanResultModel::RecoverabilityRole).toInt();
        return l < r;
    }
    default:
        return QSortFilterProxyModel::lessThan(left, right);
    }
}
