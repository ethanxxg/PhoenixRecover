PhoenixRecover — 文件系统 / 取证向第三方库（规划索引）
========================================================

本目录仅为文档占位；不随仓库提供预编译的 TSK/ntfs-3g 等源码或二进制。

完整结论、决策矩阵、许可证与集成方式请见:
  docs/TECHNICAL_PLAN.md
  章节「3.2.1 文件系统子系统与第三方库（结论与选型）」

推荐后续动作（与里程碑一致）:
1. 需多 FS/对照或 Linux NTFS: 在 CMake 中开启 PHOENIX_USE_TSK 等（见根 CMakeLists.txt 注释块）前，先做法务评审。
2. 集成 The Sleuth Kit: 自 sleuthkit.org 或发行包取固定版本，在 SBOM/发版材料中记录 LICENSE/NOTICE。

与之并列的“媒体/视频”相关依赖说明见:
  third_party/ffmpeg/README.txt
