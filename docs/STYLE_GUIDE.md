# PhoenixRecover 编码规范（Qt Widgets 模块化）

## 目标
1. 页面模块化：每个“步骤/页面”必须是单独的类，便于扩展与维护。
2. UI 与逻辑解耦：页面类只负责界面展示、用户交互与发出意图信号；不参与任何业务/恢复/扫描等逻辑。

## 目录与分层（强制）
1. 页面层（UI）：`pages/`
   - 每个页面一个类：`pages/<PageName>Page.h/.cpp`
   - 页面只包含控件创建/排版、UI 状态切换（例如选中态）以及信号发射
2. 控制层（业务/流程）：`controllers/`
   - 负责串联步骤、校验参数、启动扫描/恢复任务、管理任务状态
3. 主窗口/路由层：`PhoenixRecover.*`
   - 只负责页面容器（例如 `QStackedWidget`）与把页面信号连接到控制层

> 如果你当前工程结构还没有上述目录，后续新增文件必须遵循该结构；不要继续把所有 UI 逻辑堆在 `PhoenixRecover.cpp` 里。

## 页面类契约（强制）
每个页面类必须满足以下要求：
1. 继承与职责
   - 继承：`class XXXPage : public QWidget`
   - 职责：界面构建与交互（按钮点击、选项切换、展示文本、错误提示等）
2. 不允许出现的代码（UI 类禁止）
   - 禁止：磁盘/文件系统扫描、读写文件、恢复/解析 RAW/文件系统、执行耗时任务
   - 禁止：调用底层“恢复服务/扫描服务”的业务函数
   - 禁止：对恢复任务的状态做持久化/重试/统计
3. 只通过信号表达“用户意图”
   - 页面在用户操作时，只发射信号（`signals:`）
   - 页面不直接调用控制层的业务方法
4. 对外暴露接口（可选，但建议统一）
   - 如果需要从控制层更新 UI（例如显示进度/错误/可用选项），提供简单的 public 方法或 slot：
     - `void setXxx(...)`
     - `void showError(const QString &msg)`
   - 这些方法只做 UI 更新，不包含业务计算/扫描逻辑

### 页面类命名
1. 类名：`<StepName>Page`（例如：`RecoveryLocationPage`）
2. 文件名与类名一致：`RecoveryLocationPage.h/.cpp`

## UI 与业务的接口约定（强制）
1. 定义“页面意图”的枚举/数据结构（建议放在 `models/` 或 `controllers/` 相关头文件）
   - 例如：`enum class LocationOption { RecycleBin, Desktop, PickFolder, Disk };`
2. 页面信号示例（仅示意，强制你用同样的模式）
   - `signals: void requestNextStep(LocationOption option, const QVariant &extra);`
   - 或者：`void locationConfirmed(LocationOption option);`
3. 控制层连接页面信号时才执行业务逻辑
   - 页面 `clicked` -> 发射信号
   - 主窗口/控制层收到信号 -> 执行业务 -> 更新 UI

## MainWindow/路由层契约（强制）
`PhoenixRecover`（或对应主窗口）必须：
1. 持有页面容器（例如 `QStackedWidget`）
2. 创建每个页面类并加入容器（一个页面 = 一个 QWidget 子类）
3. 只做信号连接与页面切换
4. 不做任何业务逻辑/扫描恢复调用

## 控件与样式规范（建议但强烈推荐）
1. UI 复用样式时使用局部工具函数（例如 `makeCard`/`makeOptionCard`），但这些函数的意义仅限“样式/控件创建”
2. UI 类里不要把样式字符串散落到所有点击回调里；尽量集中在页面构建阶段或样式函数中

## C++ 与 Qt 习惯（建议，但与模块化配套）
1. 字符串：优先 `QStringLiteral("...")`
2. 信号槽：
   - 连接使用新式连接
   - lambda 内只做轻量 UI 状态处理或信号发射
3. lambda 捕获：
   - 捕获控件指针建议按值捕获（例如 `[btn1, btn2]`），确保生命周期由 parent 管理

## 本项目当前需要立即遵循的落地要求
1. 把你在 `PhoenixRecover.cpp` 里当前“第一个页面”的 UI 搬到独立的页面类（例如 `pages/RecoveryLocationPage`）
2. `PhoenixRecover.cpp` 只负责：
   - 创建页面实例
   - `QStackedWidget` 切换
   - 页面发射的信号连接到控制层（或目前先连接到空槽用于占位）

## 例子：最小页面骨架（用于对齐你的写法）
```cpp
// pages/RecoveryLocationPage.h
class RecoveryLocationPage : public QWidget {
    Q_OBJECT
public:
    explicit RecoveryLocationPage(QWidget *parent = nullptr);

signals:
    void requestScan(const QString &selectedLocation); // UI 只发意图
};
```

```cpp
// pages/RecoveryLocationPage.cpp（只做 UI，不写扫描/恢复）
void RecoveryLocationPage::onFindClicked() {
    // 只根据选中态生成“意图参数”
    emit requestScan(selectedLocation);
}
```

