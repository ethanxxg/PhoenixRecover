#pragma once

#include <QtCore/QString>

namespace GridPreview {

bool isImageExt(const QString& lower);
bool isVideoExt(const QString& lower);
bool isDocumentExt(const QString& lower);
bool isThumbnailExt(const QString& lower);

quint32 previewByteCapForExtension(const QString& extLower);
quint32 effectivePreviewReadCap(const QString& extLower, qint64 logicalSizeBytes);

} // namespace GridPreview
