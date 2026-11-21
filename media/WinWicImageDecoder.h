#pragma once

class QString;
class QImage;

namespace WinWicImageDecoder {

// 使用 Windows WIC 从本地文件解码为 QImage（依赖系统已安装的编解码器）。
bool loadFileToImage(const QString &localFilePath, QImage *outImage);

} // namespace WinWicImageDecoder
