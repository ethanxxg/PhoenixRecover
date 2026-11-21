#pragma once

#include <QAbstractTableModel>
#include <QSet>
#include <QVector>

#include "../controllers/ScanTypes.h"

class ScanResultStore;

class ScanResultModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        ColCheckBox = 0,
        ColName,
        ColSize,
        ColType,
        ColModified,
        ColRecoverability,
        ColPath,
        ColPhase,
        ColCount
    };

    enum Role {
        FileIdRole = Qt::UserRole + 1,
        IsDirectoryRole,
        RelativePathRole,
        ExtensionRole,
        SizeBytesRole,
        ModifyTimeFtRole,
        PreviewKindRole,
        RecoverabilityRole,
    };

    explicit ScanResultModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value,
                 int role = Qt::EditRole) override;

    void appendBatch(const QVector<ScanFileDto> &batch);
    void setResultStore(ScanResultStore *store);
    void clear();

    void setChecked(int row, bool checked);
    bool isChecked(int row) const;
    QVector<quint64> checkedIds() const;
    void setAllChecked(bool checked);
    int checkedCount() const;

    const ScanFileDto *dtoAtRow(int row) const;

public slots:
    void onStoreRevisionChanged(quint64 revision);

signals:
    void checkedCountChanged(int checkedCount, int totalCount);

private:
    void syncFromStoreStep();

    static QString formatSize(qint64 bytes);
    static QString formatFileTime(quint64 fileTime100ns);
    static QString fileTypeDisplay(const ScanFileDto &dto);
    static QString recoverabilityText(const ScanFileDto &dto);
    static QColor recoverabilityColor(const ScanFileDto &dto);
    static QString phaseText(const ScanFileDto &dto);

    ScanResultStore *store_ = nullptr;
    QVector<ScanFileDto> rows_;
    QSet<quint64> checkedIds_;
    quint64 lastSyncRevision_ = 0;
    quint64 desiredSyncRevision_ = 0;
    bool syncScheduled_ = false;
};
