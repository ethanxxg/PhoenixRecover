FFmpeg 可执行文件（用于视频首帧缩略图）

程序通过 QProcess 调用 **ffmpeg.exe**，不链接 libav。按以下任一种方式即可:

1) 将 ffmpeg.exe 放到可执行文件同目录（如 x64\Debug\），或
2) 放到 third_party\ffmpeg\bin\ffmpeg.exe（与 PhoenixRecover.vcxproj 中 FFmpegRoot 一致），
   成功编译后若存在该文件，会自动复制到输出目录；或
3) 在系统 PATH 中放入 ffmpeg，或
4) 设置环境变量 PHOENIX_FFMPEG 为 ffmpeg.exe 的完整路径。

建议从 https://www.gyan.dev/ffmpeg/builds/ 等下载 Windows 版，优先选带 **libheif / libwebp / 更多 demuxer**
的 **full** 或 **gpl** 变体中的 ffmpeg.exe，以便 RAW、HEIF、WebP、部分 PSD 等走 FFmpeg 解码回退。

无需 C/C++ 开发头文件与 .lib。

若未找到 ffmpeg.exe：视频仍可尝试 Shell；图片仍可尝试 Qt 与 Windows WIC。
