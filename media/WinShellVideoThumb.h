#pragma once

class QString;
class QPixmap;

namespace WinShellVideoThumb {

// 使用系统 Shell 为本地文件生成缩略图（不依赖 ffmpeg；失败返回空图）。
QPixmap thumbnailForFile(const QString &localPath);

} // namespace WinShellVideoThumb
