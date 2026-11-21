#pragma once

#include <QtWidgets/QWidget>
#include <QtGui/QPixmap>

#include "../../controllers/ScanTypes.h"

class QLabel;
class QListWidget;
class QListWidgetItem;
class ScanResultStore;

class PreviewPanel : public QWidget
{
    Q_OBJECT

public:
    explicit PreviewPanel(QWidget *parent = nullptr);

    void setResultStore(ScanResultStore *store);
    void setDiskLetter(const QString &letter);
    void showFilePreview(const ScanFileDto &file);
    void clear();

    QListWidget *gridView() const { return gridView_; }

public slots:
    void applyGridThumbnail(quint64 fileId, quint64 listGeneration, QPixmap pixmap);
    void decodeVideoThumbnailOnMainThread(quint64 fileId, quint64 listGeneration, QString tempFilePath);
    void decodeImageThumbnailOnMainThread(quint64 fileId, quint64 listGeneration, QString tempFilePath);
    void decodeDocumentThumbnailOnMainThread(quint64 fileId, quint64 listGeneration, QString tempFilePath);

signals:
    void thumbnailReady(quint64 fileId, quint64 listGeneration, QPixmap pixmap);

private:
    void scheduleGridThumbnail(quint64 fileId);

    QListWidget *gridView_ = nullptr;
    ScanResultStore *resultStore_ = nullptr;
    QString diskLetter_;
    quint64 currentGeneration_ = 0;

    friend class RecoveryScanPage;
};
