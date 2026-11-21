# Linux 平台支持说明



PhoenixRecover **Linux 桌面端仅支持 ext2 / ext3 / ext4**（统一由 `Ext4Parser` 解析，魔数 `0xEF53`）。  

FAT、NTFS、exFAT 及对应碎片拼接、USN、S.M.A.R.T. 等能力仅在 **Windows** 提供。



## 构建



```bash

sudo apt install build-essential cmake qtbase5-dev   # GUI 可选

mkdir -p build && cd build

cmake .. -DPHOENIX_BUILD_GUI=OFF

cmake --build . -j

ctest -R 'CFReDS_EXT4|DFR_Linux|PartitionRecovery' --output-on-failure

```



带 Qt 界面（仅 ext 文件系统模块会链接进主程序）：



```bash

cmake .. -DCMAKE_PREFIX_PATH=/path/to/Qt/5.14.2/gcc_64

cmake --build . -j

```



## Linux 上可用的能力



| 能力 | 支持 |

|------|------|

| 打开 `.dd` 镜像 / `/dev/sdX` | 是 |

| 分区探测（ext 族） | 是，标签 `ext2` / `ext3` / `ext4` |

| 元数据扫描与恢复写出 | 是 |

| 签名雕刻 / Graph 碎片（RAW） | 是（不依赖 FAT/NTFS 解析器） |

| FAT / NTFS / exFAT 解析与恢复 | **否** |

| NTFS 压缩簇 / USN / SMART / 整盘 Imager | 否 |



## 测试



| CTest | 说明 |

|-------|------|

| `CFReDS_EXT4_DFR01` | 单镜像 ext 冒烟 |

| `DFR_Linux_Batch` | `testdata/dfr-test-images` 与 `testdata/ext4-variants` 下 `*-ext.dd` 批量 |

| `PartitionRecovery_ProbeImage` | 分区探测（ext 镜像） |



数据与脚本见 `docs/TEST_DATA_SOURCES.md`、`testdata/tools/Run-DfrLinuxValidation.sh`。



## Windows 主机验证 ext 样本



```powershell

powershell -File testdata\tools\Run-DfrLinuxValidation.ps1

```


