#include "PreviewPanel.h"

#include "../../controllers/ScanResultStore.h"
#include "../../disk/SectorReader.h"
#include "../../recover/FileNode.h"
#include "../../recover/RecoverEngine.h"
#include "../../media/MediaThumbnails.h"
#include "../../media/GridPreviewExtensions.h"

#include <QtCore/QDir>
#include <QtCore/QTemporaryFile>
#include <QtCore/QThreadPool>
#include <QtCore/QRunnable>
#include <QtCore/QTimer>
#include <QtGui/QImage>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QListWidgetItem>
#include <QtWidgets/QVBoxLayout>

#include <functional>

namespace {

class LambdaRunnable : public QRunnable {
public:
    explicit LambdaRunnable(std::function<void()> fn) : fn_(std::move(fn)) {}
    void run() override { if (fn_) fn_(); }
private:
    std::function<void()> fn_;
};

static std::wstring QStringToWString(const QString &s)
{
    return std::wstring(reinterpret_cast<const wchar_t *>(s.utf16()), static_cast<size_t>(s.size()));
}

static QByteArray loadPreviewThumbBytes(const QString &driveLetter, ScanResultStore *store, quint64 id)
{
    if (!store || driveLetter.isEmpty()) return {};
    ScanFileDto dto;
    if (!store->tryGetFile(id, &dto)) return {};
    const QString extLower = dto.extension.trimmed().toLower();
    const uint32_t byteCap = GridPreview::effectivePreviewReadCap(extLower, dto.sizeBytes);
    switch (dto.previewKind) {
    case ScanPreviewKind::None: return {};
    case ScanPreviewKind::ResidentBlob: {
        QByteArray b;
        if (!store->tryGetPreviewResidentBlob(id, &b)) return {};
        return b;
    }
    case ScanPreviewKind::VolumeSlice: {
        uint32_t len = dto.previewReadLength;
        if (len == 0) return {};
        if (len > byteCap) len = byteCap;
        SectorReader reader;
        if (!reader.OpenVolume(QStringToWString(driveLetter))) return {};
        QByteArray out(static_cast<int>(len), '\0');
        if (!reader.ReadBytes(dto.previewVolumeOffset, reinterpret_cast<uint8_t *>(out.data()), len))
            return {};
        return out;
    }
    case ScanPreviewKind::FullNode: {
        FileNode node;
        if (!store->tryGetPreviewFileNode(id, &node)) return {};
        Partition part;
        if (!store->tryGetScanVolumePartition(&part)) return {};
        SectorReader reader;
        if (!reader.OpenVolume(QStringToWString(driveLetter))) return {};
        std::vector<uint8_t> buf;
        if (!RecoverEngine::ReadFileDataPrefix(reader, part, node, byteCap, buf) || buf.empty())
            return {};
        return QByteArray(reinterpret_cast<const char *>(buf.data()), static_cast<int>(buf.size()));
    }
    }
    return {};
}

} // namespace

PreviewPanel::PreviewPanel(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("scanPreviewPanel"));
    setStyleSheet(QStringLiteral(
        "#scanPreviewPanel { background: #ffffff; border: 1px solid #e5e7eb; border-radius: 12px; }"
        "#scanPreviewPanel QListWidget { background: transparent; border: none; padding: 10px; }"
        "#scanPreviewPanel QListWidget::item { border: none; background: transparent; }"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    gridView_ = new QListWidget(this);
    gridView_->setViewMode(QListView::IconMode);
    gridView_->setResizeMode(QListView::Adjust);
    gridView_->setMovement(QListView::Static);
    gridView_->setSpacing(12);
    gridView_->setWrapping(true);
    gridView_->setUniformItemSizes(false);
    gridView_->setWordWrap(true);
    gridView_->setSelectionMode(QAbstractItemView::NoSelection);
    gridView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    gridView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    gridView_->setIconSize(QSize(168, 126));
    layout->addWidget(gridView_);
}

void PreviewPanel::setResultStore(ScanResultStore *store)
{
    resultStore_ = store;
}

void PreviewPanel::setDiskLetter(const QString &letter)
{
    diskLetter_ = letter;
}

void PreviewPanel::showFilePreview(const ScanFileDto &file)
{
    Q_UNUSED(file);
}

void PreviewPanel::clear()
{
    if (gridView_) {
        gridView_->clear();
    }
}

void PreviewPanel::scheduleGridThumbnail(quint64 fileId)
{
    if (!resultStore_ || diskLetter_.isEmpty()) return;
    const quint64 listGen = currentGeneration_;
    const QString letter = diskLetter_;
    ScanResultStore *store = resultStore_;
    auto *job = new LambdaRunnable([this, fileId, listGen, letter, store]() {
        const QByteArray payload = loadPreviewThumbBytes(letter, store, fileId);
        ScanFileDto dto;
        if (!store->tryGetFile(fileId, &dto)) {
            QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                      Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                      Q_ARG(QPixmap, QPixmap()));
            return;
        }
        const QString ext = dto.extension.trimmed().toLower();
        if (GridPreview::isVideoExt(ext)) {
            if (payload.isEmpty()) {
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap()));
                return;
            }
            QString safeExt = ext;
            if (safeExt.isEmpty() || safeExt.size() > 8) safeExt = QStringLiteral("bin");
            QTemporaryFile tmp(QDir::tempPath() + QStringLiteral("/phxrv_thumb_XXXXXX.") + safeExt);
            tmp.setAutoRemove(false);
            if (!tmp.open() || tmp.write(payload) != payload.size()) {
                const QString badPath = tmp.fileName();
                tmp.close();
                if (!badPath.isEmpty()) QFile::remove(badPath);
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap()));
                return;
            }
            tmp.flush();
            const QString path = tmp.fileName();
            tmp.close();
            QMetaObject::invokeMethod(this, "decodeVideoThumbnailOnMainThread", Qt::QueuedConnection,
                                      Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                      Q_ARG(QString, path));
            return;
        }
        if (GridPreview::isDocumentExt(ext)) {
            if (payload.isEmpty()) {
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap()));
                return;
            }
            QString safeExt = ext;
            if (safeExt.isEmpty() || safeExt.size() > 8) safeExt = QStringLiteral("bin");
            QTemporaryFile tmp(QDir::tempPath() + QStringLiteral("/phxrv_doc_XXXXXX.") + safeExt);
            tmp.setAutoRemove(false);
            if (!tmp.open() || tmp.write(payload) != payload.size()) {
                const QString badPath = tmp.fileName();
                tmp.close();
                if (!badPath.isEmpty()) QFile::remove(badPath);
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap()));
                return;
            }
            tmp.flush();
            const QString path = tmp.fileName();
            tmp.close();
            QMetaObject::invokeMethod(this, "decodeDocumentThumbnailOnMainThread", Qt::QueuedConnection,
                                      Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                      Q_ARG(QString, path));
            return;
        }
        if (GridPreview::isImageExt(ext)) {
            if (payload.isEmpty()) {
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap()));
                return;
            }
            QImage img;
            if (img.loadFromData(payload)) {
                img = img.scaled(180, 110, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap::fromImage(img)));
                return;
            }
            QString safeExt = ext;
            if (safeExt.isEmpty() || safeExt.size() > 8) safeExt = QStringLiteral("bin");
            QTemporaryFile tmp(QDir::tempPath() + QStringLiteral("/phxrv_img_XXXXXX.") + safeExt);
            tmp.setAutoRemove(false);
            if (!tmp.open() || tmp.write(payload) != payload.size()) {
                const QString badPath = tmp.fileName();
                tmp.close();
                if (!badPath.isEmpty()) QFile::remove(badPath);
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap()));
                return;
            }
            tmp.flush();
            const QString path = tmp.fileName();
            tmp.close();
            QMetaObject::invokeMethod(this, "decodeImageThumbnailOnMainThread", Qt::QueuedConnection,
                                      Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                      Q_ARG(QString, path));
            return;
        }
        QPixmap pm;
        if (!payload.isEmpty()) {
            QImage img;
            if (img.loadFromData(payload)) {
                img = img.scaled(180, 110, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                pm = QPixmap::fromImage(img);
            }
        }
        QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                  Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                  Q_ARG(QPixmap, pm));
    });
    job->setAutoDelete(true);
    QThreadPool::globalInstance()->start(job);
}

void PreviewPanel::decodeVideoThumbnailOnMainThread(quint64 fileId, quint64 listGeneration,
                                                     QString tempFilePath)
{
    QTimer::singleShot(0, this, [this, fileId, listGeneration, tempFilePath]() {
        const QPixmap pm = MediaThumbnails::videoPreviewPixmapForLocalFile(tempFilePath);
        QFile::remove(tempFilePath);
        applyGridThumbnail(fileId, listGeneration, pm);
    });
}

void PreviewPanel::decodeDocumentThumbnailOnMainThread(quint64 fileId, quint64 listGeneration,
                                                      QString tempFilePath)
{
    QTimer::singleShot(0, this, [this, fileId, listGeneration, tempFilePath]() {
        const QPixmap pm = MediaThumbnails::documentPreviewPixmapForLocalFile(tempFilePath);
        QFile::remove(tempFilePath);
        applyGridThumbnail(fileId, listGeneration, pm);
    });
}

void PreviewPanel::decodeImageThumbnailOnMainThread(quint64 fileId, quint64 listGeneration,
                                                     QString tempFilePath)
{
    QTimer::singleShot(0, this, [this, fileId, listGeneration, tempFilePath]() {
        const QPixmap pm = MediaThumbnails::imagePreviewPixmapForLocalFile(tempFilePath);
        QFile::remove(tempFilePath);
        applyGridThumbnail(fileId, listGeneration, pm);
    });
}

void PreviewPanel::applyGridThumbnail(quint64 fileId, quint64 listGeneration, QPixmap pixmap)
{
    if (listGeneration != currentGeneration_) return;
    if (!gridView_) return;
    for (int i = 0; i < gridView_->count(); ++i) {
        QListWidgetItem *item = gridView_->item(i);
        if (!item || item->data(Qt::UserRole).toULongLong() != fileId) continue;
        QWidget *card = gridView_->itemWidget(item);
        if (!card) continue;
        QLabel *lab = card->findChild<QLabel *>(QStringLiteral("gridThumbPix"));
        if (!lab) continue;
        if (!pixmap.isNull()) {
            lab->setPixmap(pixmap);
            lab->setText(QString());
            lab->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        } else {
            lab->setText(QStringLiteral("无法预览"));
            lab->setStyleSheet(QStringLiteral(
                "color: #9ca3af; font-size: 12px; font-weight: 600; background: transparent;"));
        }
        break;
    }
    emit thumbnailReady(fileId, listGeneration, pixmap);
}
