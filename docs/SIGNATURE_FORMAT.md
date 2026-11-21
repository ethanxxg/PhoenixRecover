# 自定义文件签名格式说明

> 本文档描述 `SignatureDB::LoadFromFile()` 所使用的 JSON 签名文件格式。
>
> 可直接使用的样例：[`testdata/custom_signatures.json`](../testdata/custom_signatures.json)（sqlite / pdf / wav 三条）；加载行为由 CTest `SignatureDBLoad` 逐字段验证。

---

## 文件结构

签名文件为标准 JSON 格式，根元素为对象，包含一个 `signatures` 数组：

```json
{
  "signatures": [
    {
      "extension": "xyz",
      "category": "Custom",
      "description": "XYZ Format",
      "magic": "58595A00",
      "magic_offset": 0,
      "footer": "454E44",
      "max_size": 10485760,
      "secondary_magic": "",
      "secondary_offset": 0
    }
  ]
}
```

---

## 字段说明

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `extension` | string | **是** | — | 文件扩展名（不含点号），如 `"jpg"`、`"sqlite"` |
| `category` | string | 否 | — | 分类名，用于文档和展示（如 `"Image"`、`"Database"`） |
| `description` | string | 否 | — | 人类可读的格式描述 |
| `magic` | string | **是** | — | 十六进制字符串表示的魔数字节（主签名），如 `"FFD8FFE0"` |
| `magic_offset` | int | 否 | `0` | 魔数在文件中的起始字节偏移 |
| `footer` | string | 否 | `""` | 十六进制字符串表示的文件尾部签名，用于精确定位文件边界 |
| `max_size` | number | 否 | `52428800`（50 MB） | 该类型文件的最大允许大小（字节） |
| `secondary_magic` | string | 否 | `""` | 十六进制字符串表示的二级签名（用于子类型区分） |
| `secondary_offset` | int | 否 | `-1` | 二级签名在文件中的偏移，`-1` 表示不检查 |

---

## 十六进制字符串规则

- 使用大写或小写均可：`FFD8FF` 和 `ffd8ff` 等价
- 每两个字符表示一个字节
- 字符串长度必须为偶数
- 仅允许 `0-9`、`A-F`、`a-f` 字符
- 空字符串 `""` 表示不设置

**示例对照：**

| 原始字节 | 十六进制字符串 |
|----------|---------------|
| `\xFF\xD8\xFF\xE0` | `"FFD8FFE0"` |
| `SQLite format 3\0` | `"53514C69746520666F726D6174203300"` |
| `PK\x03\x04` | `"504B0304"` |

---

## 加载行为

1. `SignatureDB::LoadFromFile()` 将新签名**追加**到已有的内建签名数据库后面
2. 匹配时按数组顺序和追加顺序依次检查，首个匹配即返回
3. 若需覆盖内建签名的优先级，需通过代码层面调整
4. 同一文件可包含任意数量的签名条目
5. 解析失败的条目会被跳过并记录警告日志，不影响其余条目
6. 返回值为成功加载的签名数量；文件打开失败或 JSON 格式错误时返回 `-1`
7. **严格 hex 校验**（A1-9 起）：`magic` / `footer` / `secondary_magic` 必须为偶数长度且只含 `0-9`、`A-F`、`a-f`；不合法的条目整条跳过并记警告，其余条目照常加载。旧 Qt 实现会把非法字符静默解码成 `0x00`，可能生成错误的匹配器，已修正
8. `max_size` 非正数（含 `NaN`）回退到默认 50 MiB；`extension` / `magic` 等字符串首尾空白会被裁剪
9. **无 Qt 依赖**：加载走 `std::ifstream` + 零依赖 `scan/SignatureJson.h` 严格 JSON 解析器（拒绝尾随逗号、尾随垃圾、未转义控制字符、前导零、错误指数、超 64 层嵌套），headless 与 GUI 构建行为一致；UTF-8 BOM 自动剥离
10. 加载后 AC 匹配器缓存失效，`Match()` / `ScanBatch()` 无需额外调用即可识别新签名

---

## 完整示例

```json
{
  "signatures": [
    {
      "extension": "sqlite",
      "category": "Database",
      "description": "SQLite 3 Database",
      "magic": "53514C69746520666F726D6174203300",
      "magic_offset": 0,
      "footer": "",
      "max_size": 4294967296,
      "secondary_magic": "",
      "secondary_offset": -1
    },
    {
      "extension": "pdf",
      "category": "Document",
      "description": "PDF Document",
      "magic": "25504446",
      "magic_offset": 0,
      "footer": "2525454F46",
      "max_size": 524288000,
      "secondary_magic": "",
      "secondary_offset": -1
    },
    {
      "extension": "wav",
      "category": "Audio",
      "description": "WAV Audio (RIFF/WAVE)",
      "magic": "52494646",
      "magic_offset": 0,
      "footer": "",
      "max_size": 524288000,
      "secondary_magic": "57415645",
      "secondary_offset": 8
    }
  ]
}
```
