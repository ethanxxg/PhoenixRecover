#pragma once

class QString;
class QPixmap;

namespace MediaThumbnails {

// 自本地路径生成网格用缩略图（180×110 量级），按平台选解码/系统 API。
QPixmap videoPreviewPixmapForLocalFile(const QString &localFilePath);
QPixmap imagePreviewPixmapForLocalFile(const QString &localFilePath);

QPixmap documentPreviewPixmapForLocalFile(const QString &localFilePath);

} // namespace MediaThumbnails
