#pragma once

#include <QSortFilterProxyModel>
#include <QDateTime>
#include <QStringList>

class ScanResultFilterModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit ScanResultFilterModel(QObject *parent = nullptr);

    void setNameFilter(const QString &keyword);
    void setExtensionFilter(const QStringList &extensions);
    void setSizeRange(qint64 minBytes, qint64 maxBytes);
    void setDateRange(const QDateTime &from, const QDateTime &to);
    void setRecoverabilityFilter(int minLevel);
    void setPathPrefix(const QString &relPath);
    void setTypeCategory(const QString &category);
    void clearAllFilters();

    QString nameFilter() const { return nameKeyword_; }
    QString pathPrefix() const { return pathPrefix_; }
    QString typeCategory() const { return typeCategory_; }

protected:
    bool filterAcceptsRow(int sourceRow,
                          const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &left,
                  const QModelIndex &right) const override;

private:
    QString nameKeyword_;
    QStringList extensionFilter_;
    qint64 minSize_ = -1;
    qint64 maxSize_ = -1;
    QDateTime dateFrom_;
    QDateTime dateTo_;
    int minRecoverability_ = -1;
    QString pathPrefix_;
    QString typeCategory_;
};
