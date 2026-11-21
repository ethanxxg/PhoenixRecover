#include "MoreRecoveryToolsPage.h"

#include "../disk/PartitionProbe.h"

#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QStyle>
#include <QtWidgets/QSizePolicy>

namespace {

QString cardStyle()
{
    return QStringLiteral(
        "QToolButton {"
        "  background: #ffffff;"
        "  border: 1px solid #e5eaf1;"
        "  border-radius: 14px;"
        "  padding: 16px 12px;"
        "  color: #172033;"
        "  font-size: 14px;"
        "  text-align: left;"
        "}"
        "QToolButton:hover {"
        "  border-color: #b8c9ef;"
        "  background: #fbfdff;"
        "}"
        "QToolButton:pressed {"
        "  background: #eff6ff;"
        "}");
}

QLabel *makeEyebrow(QWidget *parent, const QString &text)
{
    auto *label = new QLabel(text, parent);
    label->setStyleSheet(QStringLiteral(
        "color: #2563eb; font-size: 11px; font-weight: 750; letter-spacing: 1px;"));
    return label;
}

} // namespace

MoreRecoveryToolsPage::MoreRecoveryToolsPage(QWidget *parent)
    : QWidget(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    stack_ = new QStackedWidget(this);
    root->addWidget(stack_, 1);

    // ── 工具主页 ───────────────────────────────────────────────────────────
    homePage_ = new QWidget(stack_);
    auto *homeLayout = new QVBoxLayout(homePage_);
    homeLayout->setContentsMargins(18, 22, 18, 18);
    homeLayout->setSpacing(12);

    auto *homeEyebrow = makeEyebrow(homePage_, QStringLiteral("TOOLS"));
    homeLayout->addWidget(homeEyebrow);
    auto *homeTitle = new QLabel(QStringLiteral("更多工具"), homePage_);
    homeTitle->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: 750; color: #172033;"));
    homeLayout->addWidget(homeTitle);
    auto *homeHint = new QLabel(
        QStringLiteral("常用操作集中在这里；普通恢复不需要进入工具页面。"), homePage_);
    homeHint->setStyleSheet(QStringLiteral("font-size: 13px; color: #718096;"));
    homeLayout->addWidget(homeHint);

    auto *grid = new QGridLayout();
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(14);
    const auto addCard = [this, grid](const QString &title,
                                      const QString &subtitle,
                                      QStyle::StandardPixmap icon,
                                      int row,
                                      int column) {
        auto *card = makeToolCard(title, subtitle, icon);
        grid->addWidget(card, row, column);
        connect(card, &QToolButton::clicked, this, [this, title]() {
            showToolPage(title);
        });
    };

    addCard(QStringLiteral("SMART 健康检测"),
            QStringLiteral("读取重映射、待定、不可校正扇区和温度。"),
            QStyle::SP_MessageBoxInformation, 0, 0);
    addCard(QStringLiteral("磁盘镜像"),
            QStringLiteral("制作只读副本，支持断点、坏区日志和 SHA-256。"),
            QStyle::SP_DriveHDIcon, 0, 1);
    addCard(QStringLiteral("文件修复"),
            QStringLiteral("修复图片、视频、ZIP、Office 和 SQLite 文件。"),
            QStyle::SP_FileDialogDetailedView, 0, 2);
    addCard(QStringLiteral("分区探测"),
            QStringLiteral("查找丢失或损坏的 MBR、GPT、VBR 和文件系统分区。"),
            QStyle::SP_DirIcon, 1, 0);
    addCard(QStringLiteral("扫描会话"),
            QStringLiteral("打开上次保存的结果，继续筛选和恢复文件。"),
            QStyle::SP_DialogOpenButton, 1, 1);
    addCard(QStringLiteral("恢复指南"),
            QStringLiteral("按风险从低到高了解推荐恢复顺序。"),
            QStyle::SP_DialogHelpButton, 1, 2);
    homeLayout->addLayout(grid, 1);

    auto *safeHint = new QLabel(
        QStringLiteral("只读检测 · 不会写入源盘 · 新功能可以独立加入工具页"), homePage_);
    safeHint->setStyleSheet(QStringLiteral("font-size: 12px; color: #9aa7b8;"));
    homeLayout->addWidget(safeHint);
    stack_->addWidget(homePage_);

    // ── 工具子页面 ─────────────────────────────────────────────────────────
    detailPage_ = new QWidget(stack_);
    auto *detailLayout = new QVBoxLayout(detailPage_);
    detailLayout->setContentsMargins(18, 22, 18, 18);
    detailLayout->setSpacing(12);

    auto *topRow = new QHBoxLayout();
    auto *backButton = new QPushButton(QStringLiteral("← 返回工具"), detailPage_);
    backButton->setStyleSheet(QStringLiteral(
        "QPushButton { border: 0; color: #475569; background: transparent; padding: 4px; }"
        "QPushButton:hover { color: #2563eb; }"));
    topRow->addWidget(backButton);
    topRow->addStretch(1);
    detailLayout->addLayout(topRow);

    auto *detailFrame = new QFrame(detailPage_);
    detailFrame->setStyleSheet(QStringLiteral(
        "QFrame { background: #ffffff; border: 1px solid #e5eaf1; border-radius: 16px; }"));
    auto *frameLayout = new QVBoxLayout(detailFrame);
    frameLayout->setContentsMargins(26, 24, 26, 24);
    frameLayout->setSpacing(10);

    detailTitle_ = new QLabel(detailFrame);
    detailTitle_->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: 750; color: #172033;"));
    frameLayout->addWidget(detailTitle_);
    detailDescription_ = new QLabel(detailFrame);
    detailDescription_->setWordWrap(true);
    detailDescription_->setStyleSheet(QStringLiteral("font-size: 13px; color: #718096;"));
    frameLayout->addWidget(detailDescription_);

    detailContent_ = new QPlainTextEdit(detailFrame);
    detailContent_->setReadOnly(true);
    detailContent_->setMinimumHeight(260);
    detailContent_->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background: #f8fafc; border: 1px solid #e5eaf1; border-radius: 12px;"
        " padding: 14px; color: #334155; font-size: 13px; }"));
    frameLayout->addWidget(detailContent_, 1);

    partitionCandidateCombo_ = new QComboBox(detailFrame);
    partitionCandidateCombo_->setVisible(false);
    partitionCandidateCombo_->setMinimumHeight(34);
    frameLayout->addWidget(partitionCandidateCombo_);

    sessionCombo_ = new QComboBox(detailFrame);
    sessionCombo_->setVisible(false);
    sessionCombo_->setMinimumHeight(34);
    frameLayout->addWidget(sessionCombo_);

    auto *sessionTools = new QWidget(detailFrame);
    auto *sessionToolsLayout = new QHBoxLayout(sessionTools);
    sessionToolsLayout->setContentsMargins(0, 0, 0, 0);
    sessionToolsLayout->setSpacing(8);
    sessionSearch_ = new QLineEdit(sessionTools);
    sessionSearch_->setPlaceholderText(QStringLiteral("搜索设备、日期或会话名称"));
    sessionSearch_->setMinimumHeight(34);
    sessionSearch_->setStyleSheet(QStringLiteral(
        "QLineEdit { background: #f8fafc; border: 1px solid #e5eaf1; border-radius: 9px;"
        " padding: 0 10px; color: #334155; }"
        "QLineEdit:focus { border-color: #93b4f3; background: #ffffff; }"));
    sessionToolsLayout->addWidget(sessionSearch_, 1);
    sessionRefreshButton_ = new QPushButton(QStringLiteral("刷新"), sessionTools);
    sessionRefreshButton_->setMinimumHeight(34);
    sessionDeleteButton_ = new QPushButton(QStringLiteral("删除会话"), sessionTools);
    sessionDeleteButton_->setMinimumHeight(34);
    sessionImportButton_ = new QPushButton(QStringLiteral("导入"), sessionTools);
    sessionImportButton_->setMinimumHeight(34);
    sessionExportButton_ = new QPushButton(QStringLiteral("导出"), sessionTools);
    sessionExportButton_->setMinimumHeight(34);
    for (QPushButton *button : {sessionRefreshButton_, sessionDeleteButton_,
                                sessionImportButton_, sessionExportButton_}) {
        button->setStyleSheet(QStringLiteral(
            "QPushButton { background: #ffffff; color: #475569; border: 1px solid #d7dee9;"
            " border-radius: 8px; padding: 0 11px; font-size: 11px; }"
            "QPushButton:hover { background: #f8fafc; border-color: #93b4f3; }"));
        sessionToolsLayout->addWidget(button);
    }
    sessionTools->setVisible(false);
    frameLayout->addWidget(sessionTools);

    auto *actionRow = new QHBoxLayout();
    detailAction_ = new QPushButton(detailFrame);
    detailAction_->setMinimumHeight(38);
    detailAction_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #2563eb; color: white; border: none; border-radius: 10px;"
        " padding: 8px 18px; font-weight: 700; }"
        "QPushButton:hover { background: #1d4ed8; }"
        "QPushButton:disabled { background: #cbd5e1; }"));
    actionRow->addStretch(1);
    actionRow->addWidget(detailAction_);
    frameLayout->addLayout(actionRow);
    detailLayout->addWidget(detailFrame, 1);
    stack_->addWidget(detailPage_);

    connect(backButton, &QPushButton::clicked, this, &MoreRecoveryToolsPage::showHome);
    connect(detailAction_, &QPushButton::clicked, this, [this]() {
        if (!currentTool_.isEmpty()) {
            if (currentTool_ == QStringLiteral("分区探测") &&
                partitionCandidateCombo_ && partitionCandidateCombo_->isVisible() &&
                partitionCandidateCombo_->currentIndex() >= 0) {
                const QVariant &start = partitionCandidateCombo_->currentData(Qt::UserRole);
                const QVariant &total = partitionCandidateCombo_->currentData(Qt::UserRole + 1);
                const QVariant &disk = partitionCandidateCombo_->currentData(Qt::UserRole + 2);
                const QString fsType = partitionCandidateCombo_->currentData(Qt::UserRole + 3).toString();
                emit requestPartitionScan(disk.toInt(),
                                           start.toULongLong(),
                                           total.toULongLong(),
                                           fsType);
                return;
            }
            if (currentTool_ == QStringLiteral("扫描会话") &&
                sessionCombo_ && sessionCombo_->isVisible() &&
                sessionCombo_->currentIndex() >= 0) {
                emit requestSessionLoad(sessionCombo_->currentData(Qt::UserRole).toString());
                return;
            }
            emit requestToolRecovery(currentTool_);
        }
    });
    connect(sessionSearch_, &QLineEdit::textChanged, this,
            [this](const QString &keyword) { refillSessionCombo(keyword); });
    connect(sessionRefreshButton_, &QPushButton::clicked, this, [this]() {
        emit requestToolRecovery(QStringLiteral("扫描会话"));
    });
    connect(sessionDeleteButton_, &QPushButton::clicked, this, [this]() {
        if (sessionCombo_ && sessionCombo_->currentIndex() >= 0) {
            emit requestSessionDelete(sessionCombo_->currentData(Qt::UserRole).toString());
        }
    });
    connect(sessionImportButton_, &QPushButton::clicked, this,
            &MoreRecoveryToolsPage::requestSessionImport);
    connect(sessionExportButton_, &QPushButton::clicked, this, [this]() {
        if (sessionCombo_ && sessionCombo_->currentIndex() >= 0) {
            emit requestSessionExport(sessionCombo_->currentData(Qt::UserRole).toString());
        }
    });

    showHome();
}

QToolButton *MoreRecoveryToolsPage::makeToolCard(const QString &title,
                                                 const QString &subtitle,
                                                 QStyle::StandardPixmap iconType)
{
    auto *button = new QToolButton(homePage_);
    button->setText(title + QStringLiteral("\n") + subtitle);
    button->setIcon(style()->standardIcon(iconType));
    button->setIconSize(QSize(34, 34));
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    button->setMinimumHeight(138);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    button->setStyleSheet(cardStyle());
    return button;
}

void MoreRecoveryToolsPage::showToolPage(const QString &toolName)
{
    currentTool_ = toolName;
    if (partitionCandidateCombo_) {
        partitionCandidateCombo_->clear();
        partitionCandidateCombo_->setVisible(false);
    }
    if (sessionCombo_) {
        sessionCombo_->clear();
        sessionCombo_->setVisible(false);
    }
    if (sessionSearch_) {
        sessionSearch_->clear();
        sessionSearch_->setVisible(false);
    }
    if (sessionDeleteButton_) sessionDeleteButton_->setVisible(false);
    if (sessionRefreshButton_) sessionRefreshButton_->setVisible(false);
    if (sessionImportButton_) sessionImportButton_->setVisible(false);
    if (sessionExportButton_) sessionExportButton_->setVisible(false);
    if (toolName == QStringLiteral("SMART 健康检测")) {
        updateToolDetail(toolName,
                         QStringLiteral("读取物理磁盘的 SMART 健康信息，判断是否应该先制作镜像。"),
                         QStringLiteral("点击“读取 SMART”开始只读检测。\n\n检测内容：\n• 重映射扇区\n• 当前待定扇区\n• 不可校正扇区\n• 温度和通电时间\n\n检测不会向源盘写入数据。"),
                         QStringLiteral("读取 SMART"));
    } else if (toolName == QStringLiteral("磁盘镜像")) {
        updateToolDetail(toolName,
                         QStringLiteral("制作只读镜像，后续扫描可以在镜像上进行。"),
                         QStringLiteral("操作步骤：\n1. 选择物理源盘\n2. 选择其他磁盘或网络位置作为输出\n3. 可选：坏区重试、坏区日志、SHA-256 校验\n4. 镜像完成后自动进入扫描\n\n安全约束：输出路径不能位于源物理盘。"),
                         QStringLiteral("选择源盘并制作镜像"));
    } else if (toolName == QStringLiteral("文件修复")) {
        updateToolDetail(toolName,
                         QStringLiteral("修复已经找到但无法正常打开的文件，并生成新的副本。"),
                         QStringLiteral("支持：JPG、PNG、GIF、MP4、ZIP、Office、SQLite。\n\n正式修复流程将在独立页面中选择文件、输出目录并显示修复报告；原始文件不会被覆盖。"),
                         QStringLiteral("打开文件修复"));
    } else if (toolName == QStringLiteral("分区探测")) {
        updateToolDetail(toolName,
                         QStringLiteral("查找丢失、损坏或未挂载的分区，不会自动写回分区表。"),
                         QStringLiteral("探测方式：\n• MBR / GPT 分区表\n• VBR 文件系统签名\n• 丢失分区深度扫描\n\n结果会按置信度排序，确认候选分区后再进入只读文件扫描。"),
                         QStringLiteral("开始分区探测"));
    } else if (toolName == QStringLiteral("扫描会话")) {
        updateToolDetail(toolName,
                         QStringLiteral("打开上次保存的扫描索引，继续筛选和恢复。"),
                         QStringLiteral("扫描会话只保存索引和扫描元数据，不保存源盘副本。\n\n点击后将检查最近会话；如果会话存在，可以加载到结果页继续操作。"),
                         QStringLiteral("加载最近会话"));
    } else if (toolName == QStringLiteral("恢复指南")) {
        updateToolDetail(toolName,
                         QStringLiteral("按风险从低到高了解 PhoenixRecover 的推荐恢复顺序。"),
                         QStringLiteral("01 保护源盘\n停止写入源盘，必要时先查看 SMART 并制作镜像。\n\n02 选择恢复来源\n优先使用实际设备或只读镜像，恢复目标必须是其他位置。\n\n03 自动扫描\n先读取文件系统，再扫描删除记录，最后补充 RAW/碎片分析。\n\n04 预览和恢复\n预览图片、视频第一帧、文本内容或前 100 字节摘要后再恢复。"),
                         QString(), false);
    } else {
        updateToolDetail(toolName,
                         QStringLiteral("该工具的页面正在准备中。"),
                         QStringLiteral("该入口已经纳入统一工具页路由，后续可以在此处增加专用控制器和结果面板。"),
                         QString(), false);
    }
    stack_->setCurrentWidget(detailPage_);
}

void MoreRecoveryToolsPage::showHome()
{
    currentTool_.clear();
    stack_->setCurrentWidget(homePage_);
}

void MoreRecoveryToolsPage::updateToolDetail(const QString &title,
                                             const QString &description,
                                             const QString &content,
                                             const QString &actionText,
                                             bool actionVisible)
{
    detailTitle_->setText(title);
    detailDescription_->setText(description);
    detailContent_->setPlainText(content);
    detailAction_->setText(actionText);
    detailAction_->setVisible(actionVisible);
}

void MoreRecoveryToolsPage::setSmartSummary(const QString &summary)
{
    showToolPage(QStringLiteral("SMART 健康检测"));
    detailContent_->setPlainText(summary);
}

void MoreRecoveryToolsPage::setPartitionResults(const QString &summary)
{
    showToolPage(QStringLiteral("分区探测"));
    detailContent_->setPlainText(summary);
}

void MoreRecoveryToolsPage::setPartitionCandidates(
    int physicalDiskIdx,
    const std::vector<PartitionCandidate> &candidates,
    const QString &summary)
{
    showToolPage(QStringLiteral("分区探测"));
    detailContent_->setPlainText(summary);
    if (!partitionCandidateCombo_ || candidates.empty()) {
        return;
    }
    for (size_t i = 0; i < candidates.size(); ++i) {
        const PartitionCandidate &candidate = candidates[i];
        const QString label = QStringLiteral("候选 %1 · %2 · 起始 LBA %3 · %4%")
                                  .arg(static_cast<int>(i + 1))
                                  .arg(QString::fromStdWString(candidate.fsType))
                                  .arg(QString::number(candidate.startSector))
                                  .arg(candidate.confidence);
        partitionCandidateCombo_->addItem(label);
        const int row = partitionCandidateCombo_->count() - 1;
        partitionCandidateCombo_->setItemData(row,
                                               QVariant::fromValue<qulonglong>(candidate.startSector),
                                               Qt::UserRole);
        partitionCandidateCombo_->setItemData(row,
                                               QVariant::fromValue<qulonglong>(candidate.totalSectors),
                                               Qt::UserRole + 1);
        partitionCandidateCombo_->setItemData(row, physicalDiskIdx, Qt::UserRole + 2);
        partitionCandidateCombo_->setItemData(row,
                                               QString::fromStdWString(candidate.fsType),
                                               Qt::UserRole + 3);
    }
    partitionCandidateCombo_->setVisible(true);
    detailAction_->setText(QStringLiteral("扫描选中的分区"));
}

void MoreRecoveryToolsPage::setSessionEntries(const QStringList &labels,
                                              const QStringList &paths,
                                              const QString &summary)
{
    showToolPage(QStringLiteral("扫描会话"));
    detailContent_->setPlainText(summary);
    sessionLabels_ = labels;
    sessionPaths_ = paths;
    if (!sessionCombo_ || labels.size() != paths.size()) {
        return;
    }
    refillSessionCombo(sessionSearch_ ? sessionSearch_->text() : QString());
    sessionCombo_->setVisible(true);
    if (sessionSearch_) sessionSearch_->setVisible(true);
    if (sessionDeleteButton_) sessionDeleteButton_->setVisible(true);
    if (sessionRefreshButton_) sessionRefreshButton_->setVisible(true);
    if (sessionImportButton_) sessionImportButton_->setVisible(true);
    if (sessionExportButton_) sessionExportButton_->setVisible(true);
    if (sessionExportButton_) sessionExportButton_->setEnabled(!labels.isEmpty());
    detailAction_->setText(QStringLiteral("加载选中会话"));
    detailAction_->setEnabled(!labels.isEmpty());
}

void MoreRecoveryToolsPage::refillSessionCombo(const QString &keyword)
{
    if (!sessionCombo_) {
        return;
    }
    const QString needle = keyword.trimmed();
    const QString selectedPath = sessionCombo_->currentData(Qt::UserRole).toString();
    sessionCombo_->clear();
    for (int i = 0; i < sessionLabels_.size() && i < sessionPaths_.size(); ++i) {
        if (!needle.isEmpty() &&
            !sessionLabels_.at(i).contains(needle, Qt::CaseInsensitive) &&
            !sessionPaths_.at(i).contains(needle, Qt::CaseInsensitive)) {
            continue;
        }
        sessionCombo_->addItem(sessionLabels_.at(i), sessionPaths_.at(i));
    }
    const int restored = sessionCombo_->findData(selectedPath);
    if (restored >= 0) {
        sessionCombo_->setCurrentIndex(restored);
    }
    if (sessionDeleteButton_) {
        sessionDeleteButton_->setEnabled(sessionCombo_->count() > 0);
    }
    if (sessionExportButton_) {
        sessionExportButton_->setEnabled(sessionCombo_->count() > 0);
    }
    if (detailAction_ && currentTool_ == QStringLiteral("扫描会话")) {
        detailAction_->setEnabled(sessionCombo_->count() > 0);
    }
}
