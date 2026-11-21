#pragma once

class QString;
class QImage;

namespace FfmpegVideoThumb {

// 使用 FFmpeg 从本地视频文件解码首帧（失败时 *outImage 置空）。
bool extractFirstFrame(const QString &localFilePath, QImage *outImage);

// 使用 FFmpeg 解码静态图 / RAW / HEIF 等（取决于本机 ffmpeg 编译能力）；失败时 *outImage 置空。
bool decodeRasterViaFfmpeg(const QString &localFilePath, QImage *outImage);

} // namespace FfmpegVideoThumb
