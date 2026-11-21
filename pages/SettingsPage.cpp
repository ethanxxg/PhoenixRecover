#include "SettingsPage.h"

#include "../log/Logger.h"

#include <QtCore/QSettings>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QVBoxLayout>

namespace {

LogLevel logLevelFromSetting(int index)
{
    switch (index) {
    case 2: return LogLevel::Debug;
    case 1: return LogLevel::Warning;
    default: return LogLevel::Info;
    }
}

QFrame *makeGroup(QWidget *parent, const QString &title, const QString &description)
{
    auto *frame = new QFrame(parent);
    frame->setStyleSheet(QStringLiteral(
        "QFrame { background: #ffffff; border: 1px solid #e5eaf1; border-radius: 14px; }"));
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(20, 17, 20, 17);
    layout->setSpacing(5);
    auto *heading = new QLabel(title, frame);
    heading->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 750; color: #172033;"));
    layout->addWidget(heading);
    auto *hint = new QLabel(description, frame);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("font-size: 12px; color: #718096;"));
    layout->addWidget(hint);
    return frame;
}

void addSetting(QFrame *group,
                const QString &title,
                const QString &description,
                QCheckBox **outCheck,
                const QString &key,
                bool defaultValue)
{
    auto *layout = qobject_cast<QVBoxLayout *>(group->layout());
    if (!layout) {
        return;
    }
    auto *row = new QWidget(group);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 12, 0, 0);
    rowLayout->setSpacing(14);
    auto *copy = new QVBoxLayout();
    copy->setContentsMargins(0, 0, 0, 0);
    copy->setSpacing(3);
    auto *titleLabel = new QLabel(title, row);
    titleLabel->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 650; color: #334155;"));
    copy->addWidget(titleLabel);
    auto *descriptionLabel = new QLabel(description, row);
    descriptionLabel->setWordWrap(true);
    descriptionLabel->setStyleSheet(QStringLiteral("font-size: 11px; color: #718096;"));
    copy->addWidget(descriptionLabel);
    rowLayout->addLayout(copy, 1);

    auto *check = new QCheckBox(row);
    check->setChecked(QSettings().value(key, defaultValue).toBool());
    check->setStyleSheet(QStringLiteral(
        "QCheckBox::indicator { width: 42px; height: 24px; border-radius: 12px;"
        " background: #cbd5e1; }"
        "QCheckBox::indicator:checked { background: #2563eb; }"));
    QObject::connect(check, &QCheckBox::toggled, row, [key](bool checked) {
        QSettings settings;
        settings.setValue(key, checked);
    });
    rowLayout->addWidget(check, 0, Qt::AlignRight | Qt::AlignTop);
    layout->addWidget(row);
    *outCheck = check;
}

} // namespace

SettingsPage::SettingsPage(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral("background: #f5f7fb;"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 22, 18, 18);
    root->setSpacing(12);

    auto *top = new QHBoxLayout();
    auto *back = new QPushButton(QStringLiteral("← 返回恢复首页"), this);
    back->setStyleSheet(QStringLiteral(
        "QPushButton { border: 0; color: #475569; background: transparent; padding: 4px; }"
        "QPushButton:hover { color: #2563eb; }"));
    top->addWidget(back);
    top->addStretch(1);
    root->addLayout(top);

    auto *eyebrow = new QLabel(QStringLiteral("APPLICATION SETTINGS"), this);
    eyebrow->setStyleSheet(QStringLiteral("color: #2563eb; font-size: 11px; font-weight: 750; letter-spacing: 1px;"));
    root->addWidget(eyebrow);
    auto *title = new QLabel(QStringLiteral("设置"), this);
    title->setStyleSheet(QStringLiteral("font-size: 24px; font-weight: 750; color: #172033;"));
    root->addWidget(title);
    auto *lead = new QLabel(QStringLiteral("影响恢复安全的选项集中管理，普通用户保持默认即可。"), this);
    lead->setStyleSheet(QStringLiteral("font-size: 13px; color: #718096;"));
    root->addWidget(lead);

    auto *safeGroup = makeGroup(this, QStringLiteral("安全默认值"),
                                QStringLiteral("这些选项用于保护源盘，建议保持开启。"));
    addSetting(safeGroup, QStringLiteral("始终只读访问源盘"),
               QStringLiteral("禁止恢复流程向源设备写入数据。"),
               &readOnlyCheck_, QStringLiteral("recovery/readonly"), true);
    addSetting(safeGroup, QStringLiteral("检测到风险时自动建议制作镜像"),
               QStringLiteral("SMART 异常或坏区增加时，优先保护原始介质。"),
               &imageFirstCheck_, QStringLiteral("recovery/imageFirst"), true);
    auto *targetGroup = makeGroup(this, QStringLiteral("恢复目标"),
                                  QStringLiteral("恢复文件默认保存到与源盘不同的位置。"));
    addSetting(targetGroup, QStringLiteral("恢复前确认目标位置"),
               QStringLiteral("防止误把文件恢复回源盘。"),
               &confirmTargetCheck_, QStringLiteral("recovery/confirmTarget"), true);
    addSetting(targetGroup, QStringLiteral("保留原目录结构"),
               QStringLiteral("优先还原原始文件夹和文件名。"),
               &keepTreeCheck_, QStringLiteral("recovery/keepTree"), true);
    auto *tabs = new QTabWidget(this);
    tabs->setDocumentMode(true);
    tabs->setStyleSheet(QStringLiteral(
        "QTabWidget::pane { border: 1px solid #e5eaf1; border-radius: 14px; background: #f8fafc; top: -1px; }"
        "QTabBar::tab { background: transparent; color: #718096; padding: 10px 15px;"
        " margin-right: 8px; border-bottom: 2px solid transparent; font-size: 12px; }"
        "QTabBar::tab:selected { color: #2563eb; font-weight: 750; border-bottom-color: #2563eb; }"));

    auto *generalPage = new QWidget(tabs);
    auto *generalLayout = new QVBoxLayout(generalPage);
    generalLayout->setContentsMargins(14, 14, 14, 14);
    generalLayout->setSpacing(12);
    generalLayout->addWidget(safeGroup);
    generalLayout->addWidget(targetGroup);
    generalLayout->addStretch(1);
    tabs->addTab(generalPage, QStringLiteral("常规"));

    auto *scanPage = new QWidget(tabs);
    auto *scanLayout = new QVBoxLayout(scanPage);
    scanLayout->setContentsMargins(14, 14, 14, 14);
    scanLayout->setSpacing(12);
    auto *scanGroup = makeGroup(scanPage, QStringLiteral("扫描与读取"),
                                QStringLiteral("控制深度扫描中的碎片重组和源盘读取策略。"));
    // 扫描设置单独创建开关，以便明确展示并保存实际使用的设置键。
    if (QVBoxLayout *scanGroupLayout = qobject_cast<QVBoxLayout *>(scanGroup->layout())) {
        auto *row = new QWidget(scanGroup);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 12, 0, 0);
        rowLayout->setSpacing(14);
        auto *copy = new QVBoxLayout();
        copy->setContentsMargins(0, 0, 0, 0);
        auto *label = new QLabel(QStringLiteral("格式感知碎片重组"), row);
        label->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 650; color: #334155;"));
        copy->addWidget(label);
        auto *hint = new QLabel(QStringLiteral("对常见文件格式尝试重组断裂片段。"), row);
        hint->setStyleSheet(QStringLiteral("font-size: 11px; color: #718096;"));
        copy->addWidget(hint);
        rowLayout->addLayout(copy, 1);
        auto *check = new QCheckBox(row);
        check->setChecked(QSettings().value(QStringLiteral("recover/formatAwareFragments"), true).toBool());
        check->setStyleSheet(QStringLiteral(
            "QCheckBox::indicator { width: 42px; height: 24px; border-radius: 12px; background: #cbd5e1; }"
            "QCheckBox::indicator:checked { background: #2563eb; }"));
        connect(check, &QCheckBox::toggled, row, [](bool checked) {
            QSettings settings;
            settings.setValue(QStringLiteral("recover/formatAwareFragments"), checked);
        });
        rowLayout->addWidget(check, 0, Qt::AlignRight | Qt::AlignTop);
        scanGroupLayout->addWidget(row);
    }
    scanLayout->addWidget(scanGroup);
    auto *scanInfo = makeGroup(scanPage, QStringLiteral("只读保护"),
                               QStringLiteral("扫描器只使用只读读取路径；即使关闭界面确认，也不会向源盘写回数据。"));
    scanLayout->addWidget(scanInfo);
    scanLayout->addStretch(1);
    tabs->addTab(scanPage, QStringLiteral("扫描与读取"));

    auto *previewPage = new QWidget(tabs);
    auto *previewLayout = new QVBoxLayout(previewPage);
    previewLayout->setContentsMargins(14, 14, 14, 14);
    previewLayout->setSpacing(12);
    previewLayout->addWidget(makeGroup(previewPage, QStringLiteral("预览与结果"),
                                       QStringLiteral("结果页会优先显示图片缩略图、视频第一帧和文档预览；无法解析时显示前 100 字节摘要。")));
    previewLayout->addWidget(makeGroup(previewPage, QStringLiteral("临时文件策略"),
                                       QStringLiteral("视频、文档和图片解码产生的临时文件只在预览期间存在，完成后立即清理。")));
    previewLayout->addStretch(1);
    tabs->addTab(previewPage, QStringLiteral("预览与结果"));

    auto *advancedPage = new QWidget(tabs);
    auto *advancedLayout = new QVBoxLayout(advancedPage);
    advancedLayout->setContentsMargins(14, 14, 14, 14);
    advancedLayout->setSpacing(12);
    auto *loggingGroup = makeGroup(advancedPage, QStringLiteral("高级与日志"),
                                   QStringLiteral("日志设置用于排查设备兼容性和恢复失败原因，不影响源盘只读保护。"));
    auto *loggingRow = new QHBoxLayout();
    auto *loggingLabel = new QLabel(QStringLiteral("日志级别"), loggingGroup);
    loggingLabel->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 650; color: #334155;"));
    loggingRow->addWidget(loggingLabel);
    auto *loggingCombo = new QComboBox(loggingGroup);
    loggingCombo->addItems(QStringList() << QStringLiteral("信息") << QStringLiteral("警告") << QStringLiteral("详细调试"));
    const int configuredLogLevel = qBound(0, QSettings().value(QStringLiteral("logging/level"), 0).toInt(), 2);
    loggingCombo->setCurrentIndex(configuredLogLevel);
    Logger::Instance().SetLogLevel(logLevelFromSetting(configuredLogLevel));
    connect(loggingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), loggingGroup, [](int index) {
        QSettings settings;
        settings.setValue(QStringLiteral("logging/level"), index);
        Logger::Instance().SetLogLevel(logLevelFromSetting(index));
    });
    loggingRow->addWidget(loggingCombo, 0, Qt::AlignRight);
    if (QVBoxLayout *loggingLayout = qobject_cast<QVBoxLayout *>(loggingGroup->layout())) {
        loggingLayout->addLayout(loggingRow);
    }
    advancedLayout->addWidget(loggingGroup);
    advancedLayout->addWidget(makeGroup(advancedPage, QStringLiteral("当前版本约束"),
                                        QStringLiteral("高级设置不会改变扫描引擎的只读边界；新增功能应通过独立页面和控制器接入。")));
    advancedLayout->addStretch(1);
    tabs->addTab(advancedPage, QStringLiteral("高级与日志"));

    root->addWidget(tabs, 1);
    root->addStretch(1);

    connect(back, &QPushButton::clicked, this, &SettingsPage::requestBack);
}
