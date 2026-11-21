# SQLite 专门恢复模块

## 分层

| 层 | 文件 | 职责 |
|----|------|------|
| 核心 | `repair/SqliteRecoverService.h` | 页级校验、截断修复、雕刻偏移定位（无 Qt） |
| UI/修复入口 | `repair/SqliteRepair.h` | `IFileRepair` → `RepairManager` → **文件修复**页 |

## 能力（当前 P0）

- 识别 `SQLite format 3` 文件头（含 RAW 雕刻前导垃圾内搜索，默认 64 KiB 窗口）
- 读取页大小（512 … 65536，2 的幂）
- 按页校验 B-tree 页类型（0x02/0x03/0x05/0x0a/0x0d/0x10）
- **默认策略**：从第 1 页起连续复制合法页，遇首个坏页 **截断**（修尾部损坏/不完整导出）
- 回写 header 中 **database size in pages**（偏移 28，大端）

## 尚未实现（后续）

| 项 | 说明 |
|----|------|
| WAL / journal 合并 | 需同目录 `-wal`、`-journal` 协同 |
| 页指针重映射 | 跳过中间坏页后保持库一致（需 freelist / schema 解析） |
| `.recover` SQL 导出 | 可选第二输出格式 |
| 恢复后自动流水线 | `RecoverEngine` 导出 `.db` 后可选调用（待产品开关） |

## 使用

**文件修复页**：添加 `.sqlite` / `.db` / `.sqlite3`，输出目录 → 开始修复。

```cpp
#include "repair/SqliteRecoverService.h"

std::vector<uint8_t> out;
auto report = SqliteRecoverService::RecoverDatabase(input, inputLen, out);
```

## 测试

- CTest：`SqliteRecover_PageLevel`（合成 512 字节页库，截断与雕刻偏移）
- 签名雕刻：`SignatureDB` 仍负责 **发现** `.sqlite`；修复走本模块

## 与碎片重组的关系

数据库 **内部** 页结构由本模块处理；**磁盘簇链断裂** 仍依赖 `FragmentStitchService`（FAT/NTFS）。二者串联：先拼出连续 `.db` 字节，再 `SqliteRecoverService`。
