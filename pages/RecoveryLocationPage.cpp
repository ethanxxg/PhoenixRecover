#include "RecoveryLocationPage.h"

#include "../common/WindowsElevation.h"
#include "../log/Logger.h"

#include <algorithm>

#include <QtCore/QCoreApplication>
#include <QtCore/QLocale>
#include <QtCore/QStorageInfo>
#include <QtGui/QIcon>
#include <QtWidgets/QButtonGroup>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFrame>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QStyle>

namespace {

struct LocalDriveEntry {
    QString driveLetter;
    QString title;
    QString capacityLine;
    int usedPercent = 0;
};

QString formatBytesHuman(qint64 bytes)
{
    return QLocale::system().formattedDataSize(qMax<qint64>(bytes, 0), 2,
                                                QLocale::DataSizeIecFormat);
}

QList<LocalDriveEntry> enumerateLocalDrives()
{
    QList<LocalDriveEntry> out;
    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        if (!volume.isValid() || !volume.isReady()) {
            continue;
        }
        const QString root = volume.rootPath();
#ifdef Q_OS_WIN
        if (root.size() < 2 || !root[0].isLetter() || root[1] != QLatin1Char(':')) {
            continue;
        }
        const QString letter = QString(root[0].toUpper()) + QLatin1Char(':');
#else
        const QString letter = volume.displayName().isEmpty() ? root : volume.displayName();
#endif
        const qint64 total = volume.bytesTotal();
        if (total <= 0) {
            continue;
        }
        const qint64 used = qMax<qint64>(0, total - volume.bytesFree());
        LocalDriveEntry entry;
        entry.driveLetter = letter;
        entry.title = QStringLiteral("本地磁盘 %1").arg(letter);
        entry.capacityLine = QStringLiteral("%1 / %2")
                                  .arg(formatBytesHuman(used))
                                  .arg(formatBytesHuman(total));
        entry.usedPercent = qBound(0, int((100.0 * used) / total), 100);
        out.push_back(entry);
    }
    std::sort(out.begin(), out.end(), [](const LocalDriveEntry &a, const LocalDriveEntry &b) {
        return a.driveLetter < b.driveLetter;
    });
    return out;
}

QFrame *makeCard(QWidget *parent)
{
    auto *card = new QFrame(parent);
    card->setStyleSheet(QStringLiteral(
        "QFrame { background: #ffffff; border: 1px solid #e5eaf1; border-radius: 16px; }"));
    return card;
}

QToolButton *makeDeviceCard(QWidget *parent,
                            const LocalDriveEntry &entry,
                            bool selected)
{
    auto *button = new QToolButton(parent);
    button->setCheckable(true);
    button->setChecked(selected);
    button->setProperty("driveLetter", entry.driveLetter);
    button->setMinimumHeight(112);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setStyleSheet(QStringLiteral(
        "QToolButton { background: #ffffff; border: 1px solid #d7dee9; border-radius: 14px; }"
        "QToolButton:hover { border-color: #a8bce6; background: #fbfdff; }"
        "QToolButton:checked { border: 2px solid #2563eb; background: #eff6ff; }"));

    auto *layout = new QGridLayout(button);
    layout->setContentsMargins(15, 13, 15, 13);
    layout->setHorizontalSpacing(12);
    layout->setVerticalSpacing(5);
    auto *icon = new QLabel(QStringLiteral("▣"), button);
    icon->setAlignment(Qt::AlignCenter);
    icon->setFixedSize(34, 34);
    icon->setStyleSheet(QStringLiteral(
        "QLabel { color: #2563eb; background: #dbeafe; border-radius: 10px; font-size: 17px; }"));
    layout->addWidget(icon, 0, 0, 2, 1, Qt::AlignTop);

    auto *title = new QLabel(entry.title, button);
    title->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 750; color: #172033;"));
    layout->addWidget(title, 0, 1);
    auto *capacity = new QLabel(entry.capacityLine, button);
    capacity->setStyleSheet(QStringLiteral("font-size: 11px; color: #718096;"));
    layout->addWidget(capacity, 1, 1);

    auto *bar = new QFrame(button);
    bar->setFixedHeight(6);
    bar->setStyleSheet(QStringLiteral(
        "QFrame { background: #e8eef7; border-radius: 3px; }"));
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    auto *fill = new QFrame(bar);
    fill->setFixedHeight(6);
    fill->setFixedWidth(qMax(8, entry.usedPercent));
    fill->setStyleSheet(QStringLiteral(
        "QFrame { background: #2563eb; border-radius: 3px; }"));
    barLayout->addWidget(fill);
    barLayout->addStretch(1);
    layout->addWidget(bar, 2, 1);
    layout->setColumnStretch(1, 1);
    return button;
}

QToolButton *makeInlineButton(QWidget *parent, const QString &text, QStyle::StandardPixmap icon)
{
    auto *button = new QToolButton(parent);
    button->setText(text);
    button->setIcon(button->style()->standardIcon(icon));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    button->setFixedHeight(38);
    button->setMinimumWidth(130);
    button->setStyleSheet(QStringLiteral(
        "QToolButton { background: #ffffff; border: 1px solid #d7dee9; border-radius: 10px;"
        " padding: 0 13px; color: #475569; font-size: 12px; font-weight: 650; }"
        "QToolButton:hover { border-color: #93b4f3; background: #eff6ff; }"
        "QToolButton:checked { border: 2px solid #2563eb; color: #1d4ed8; }"));
    button->setCheckable(true);
    return button;
}

} // namespace

RecoveryLocationPage::RecoveryLocationPage(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral(
        "QWidget { font-family: 'Segoe UI'; color: #172033; }"
        "QWidget#RecoveryLocationPage { background: #f5f7fb; }"));
    setObjectName(QStringLiteral("RecoveryLocationPage"));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(14);

    auto *headingRow = new QHBoxLayout();
    headingRow->setContentsMargins(0, 0, 0, 0);
    auto *headingCopy = new QVBoxLayout();
    headingCopy->setContentsMargins(0, 0, 0, 0);
    headingCopy->setSpacing(5);
    auto *eyebrow = new QLabel(QStringLiteral("DATA RECOVERY"), this);
    eyebrow->setStyleSheet(QStringLiteral("color: #2563eb; font-size: 11px; font-weight: 750; letter-spacing: 1px;"));
    headingCopy->addWidget(eyebrow);
    auto *title = new QLabel(QStringLiteral("恢复你的数据"), this);
    title->setStyleSheet(QStringLiteral("font-size: 26px; font-weight: 750; color: #172033;"));
    headingCopy->addWidget(title);
    auto *subtitle = new QLabel(
        QStringLiteral("选择数据所在的设备，我们会自动选择最合适的恢复方式。"), this);
    subtitle->setStyleSheet(QStringLiteral("font-size: 13px; color: #718096;"));
    headingCopy->addWidget(subtitle);
    headingRow->addLayout(headingCopy, 1);
    lastScanLabel_ = new QLabel(QStringLiteral("上次扫描：暂无可继续会话"), this);
    lastScanLabel_->setStyleSheet(QStringLiteral("font-size: 11px; color: #9aa7b8;"));
    headingRow->addWidget(lastScanLabel_, 0, Qt::AlignTop);
    root->addLayout(headingRow);

    auto *hero = new QGridLayout();
    hero->setHorizontalSpacing(16);
    hero->setVerticalSpacing(16);
    hero->setColumnStretch(0, 3);
    hero->setColumnStretch(1, 2);

    auto *sourceCard = makeCard(this);
    auto *sourceLayout = new QVBoxLayout(sourceCard);
    sourceLayout->setContentsMargins(20, 18, 20, 18);
    sourceLayout->setSpacing(12);
    auto *sourceTitleRow = new QHBoxLayout();
    auto *sourceTitle = new QLabel(QStringLiteral("选择恢复来源"), sourceCard);
    sourceTitle->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 750;"));
    sourceTitleRow->addWidget(sourceTitle);
    sourceTitleRow->addStretch(1);
    auto *sourceHint = new QLabel(QStringLiteral("一步完成选择"), sourceCard);
    sourceHint->setStyleSheet(QStringLiteral("font-size: 11px; color: #9aa7b8;"));
    sourceTitleRow->addWidget(sourceHint);
    sourceLayout->addLayout(sourceTitleRow);

    const QList<LocalDriveEntry> drives = enumerateLocalDrives();
    scanTargetGroup_ = new QButtonGroup(this);
    scanTargetGroup_->setExclusive(true);
    auto *driveGrid = new QGridLayout();
    driveGrid->setHorizontalSpacing(10);
    driveGrid->setVerticalSpacing(10);
    for (int i = 0; i < drives.size(); ++i) {
        auto *card = makeDeviceCard(sourceCard, drives.at(i), i == 0);
        scanTargetGroup_->addButton(card);
        driveGrid->addWidget(card, i / 2, i % 2);
    }
    if (drives.isEmpty()) {
        auto *empty = new QLabel(QStringLiteral("未检测到可用本地磁盘。"), sourceCard);
        empty->setStyleSheet(QStringLiteral("color: #718096; font-size: 12px;"));
        driveGrid->addWidget(empty, 0, 0, 1, 2);
    }
    sourceLayout->addLayout(driveGrid);

    auto *imageEntry = new QFrame(sourceCard);
    imageEntry->setStyleSheet(QStringLiteral(
        "QFrame { background: #f8fafc; border: 1px solid #e5eaf1; border-radius: 11px; }"));
    auto *imageLayout = new QHBoxLayout(imageEntry);
    imageLayout->setContentsMargins(11, 8, 11, 8);
    auto *imageCopy = new QVBoxLayout();
    imageCopy->setSpacing(2);
    auto *imageTitle = new QLabel(QStringLiteral("扫描磁盘镜像"), imageEntry);
    imageTitle->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 700;"));
    imageCopy->addWidget(imageTitle);
    auto *imageHint = new QLabel(
        QStringLiteral("支持 .img/.dd/.raw 及 VHD/VHDX/VMDK/QCOW2/VDI 虚拟磁盘（只读）"),
        imageEntry);
    imageHint->setStyleSheet(QStringLiteral("font-size: 10px; color: #718096;"));
    imageCopy->addWidget(imageHint);
    imageLayout->addLayout(imageCopy, 1);
    auto *imageButton = new QPushButton(QStringLiteral("选择文件"), imageEntry);
    imageButton->setStyleSheet(QStringLiteral(
        "QPushButton { border: 0; color: #2563eb; background: transparent; font-size: 11px; }"
        "QPushButton:hover { color: #1d4ed8; }"));
    imageLayout->addWidget(imageButton);
    sourceLayout->addWidget(imageEntry);

    auto *cta = new QHBoxLayout();
    findBtn_ = new QPushButton(QStringLiteral("开始恢复"), sourceCard);
    findBtn_->setMinimumHeight(42);
    findBtn_->setMinimumWidth(150);
    findBtn_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #2563eb; color: white; border: 0; border-radius: 10px;"
        " font-size: 14px; font-weight: 750; padding: 8px 20px; }"
        "QPushButton:hover { background: #1d4ed8; }"));
    cta->addWidget(findBtn_);
    selectedSourceLabel_ = new QLabel(this);
    selectedSourceLabel_->setStyleSheet(QStringLiteral("font-size: 11px; color: #718096;"));
    cta->addWidget(selectedSourceLabel_, 1, Qt::AlignVCenter);
    sourceLayout->addLayout(cta);
    hero->addWidget(sourceCard, 0, 0);

    auto *autoCard = makeCard(this);
    autoCard->setStyleSheet(QStringLiteral(
        "QFrame { background: #ffffff; border: 1px solid #e5eaf1; border-radius: 16px; }"));
    auto *autoLayout = new QVBoxLayout(autoCard);
    autoLayout->setContentsMargins(20, 18, 20, 18);
    autoLayout->setSpacing(10);
    autoLayout->addWidget(new QLabel(QStringLiteral("SMART RECOVERY"), autoCard));
    autoCard->layout()->itemAt(0)->widget()->setStyleSheet(
        QStringLiteral("color: #2563eb; font-size: 11px; font-weight: 750; letter-spacing: 1px;"));
    autoLayout->addWidget(new QLabel(QStringLiteral("自动恢复模式"), autoCard));
    autoCard->layout()->itemAt(1)->widget()->setStyleSheet(
        QStringLiteral("font-size: 19px; font-weight: 750; color: #172033;"));
    autoLayout->addWidget(new QLabel(
        QStringLiteral("无需理解扫描模式，PhoenixRecover 会自动完成判断。"), autoCard));
    autoCard->layout()->itemAt(2)->widget()->setStyleSheet(
        QStringLiteral("font-size: 12px; color: #718096;"));
    const QStringList steps = {
        QStringLiteral("1  快速读取文件系统\n   优先保留文件名、目录和时间信息"),
        QStringLiteral("2  深度扫描删除记录\n   查找已删除、格式化后的文件"),
        QStringLiteral("3  按文件结构补充恢复\n   必要时启用 RAW 和碎片分析")};
    for (const QString &step : steps) {
        auto *stepLabel = new QLabel(step, autoCard);
        stepLabel->setWordWrap(true);
        stepLabel->setStyleSheet(QStringLiteral(
            "font-size: 11px; color: #475569; padding: 8px;"
            " background: #f8fafc; border-radius: 8px;"));
        autoLayout->addWidget(stepLabel);
    }

    smartHealthBanner_ = new QFrame(autoCard);
    smartHealthBanner_->setObjectName(QStringLiteral("smartHealthBanner"));
    smartHealthBanner_->setStyleSheet(QStringLiteral(
        "#smartHealthBanner { background: #eff6ff; border: 1px solid #bfdbfe; border-radius: 10px; }"
        "#smartHealthBanner QLabel[role='status'] { color: #1d4ed8; font-size: 12px; font-weight: 750; }"
        "#smartHealthBanner QLabel[role='detail'] { color: #64748b; font-size: 10px; }"));
    auto *smartHealthLayout = new QVBoxLayout(smartHealthBanner_);
    smartHealthLayout->setContentsMargins(11, 9, 11, 9);
    smartHealthLayout->setSpacing(3);
    smartHealthStatusLabel_ = new QLabel(QStringLiteral("设备健康：正在检测"), smartHealthBanner_);
    smartHealthStatusLabel_->setProperty("role", QStringLiteral("status"));
    smartHealthDetailLabel_ = new QLabel(QStringLiteral("正在读取 SMART，只读检测不会影响源盘。"), smartHealthBanner_);
    smartHealthDetailLabel_->setProperty("role", QStringLiteral("detail"));
    smartHealthDetailLabel_->setWordWrap(true);
    smartHealthLayout->addWidget(smartHealthStatusLabel_);
    smartHealthLayout->addWidget(smartHealthDetailLabel_);
    autoLayout->addWidget(smartHealthBanner_);
    autoLayout->addStretch(1);
    autoLayout->addWidget(new QLabel(
        QStringLiteral("高级选项不会影响普通用户流程，可在扫描前展开调整。"), autoCard));
    autoCard->layout()->itemAt(autoLayout->count() - 1)->widget()->setStyleSheet(
        QStringLiteral("font-size: 10px; color: #9aa7b8;"));
    hero->addWidget(autoCard, 0, 1);
    root->addLayout(hero);

    auto *advanced = new QFrame(this);
    advanced->setStyleSheet(QStringLiteral(
        "QFrame { background: #ffffff; border: 1px solid #e5eaf1; border-radius: 12px; }"));
    auto *advancedLayout = new QVBoxLayout(advanced);
    advancedLayout->setContentsMargins(15, 10, 15, 10);
    advancedLayout->setSpacing(8);
    auto *advancedButton = new QToolButton(advanced);
    advancedButton->setText(QStringLiteral("高级设置    扫描模式、文件类型、镜像策略"));
    advancedButton->setCheckable(true);
    advancedButton->setStyleSheet(QStringLiteral(
        "QToolButton { border: 0; color: #475569; background: transparent; text-align: left; font-size: 12px; }"));
    advancedLayout->addWidget(advancedButton);
    auto *advancedBody = new QWidget(advanced);
    auto *advancedBodyLayout = new QHBoxLayout(advancedBody);
    advancedBodyLayout->setContentsMargins(0, 0, 0, 0);
    for (const QString &value : {QStringLiteral("扫描策略：自动选择"),
                                 QStringLiteral("文件类型：全部类型"),
                                 QStringLiteral("磁盘读取：只读 · 自动重试")}) {
        auto *label = new QLabel(value, advancedBody);
        label->setStyleSheet(QStringLiteral("font-size: 11px; color: #718096;"));
        advancedBodyLayout->addWidget(label);
    }
    advancedBodyLayout->addStretch(1);
    advancedBody->setVisible(false);
    advancedLayout->addWidget(advancedBody);
    connect(advancedButton, &QToolButton::toggled, advancedBody, &QWidget::setVisible);
    root->addWidget(advanced);

    auto *status = new QLabel(
        QStringLiteral("● 不会写入源盘      预计步骤：1 次选择 + 1 次点击      恢复文件将保存到其他位置"),
        this);
    status->setStyleSheet(QStringLiteral("font-size: 11px; color: #718096; padding: 3px 0;"));
    root->addWidget(status);
    root->addStretch(1);

    auto updateSelected = [this]() {
        if (!selectedSourceLabel_ || !scanTargetGroup_) {
            return;
        }
        auto *button = scanTargetGroup_->checkedButton();
        selectedSourceLabel_->setText(button
                                          ? QStringLiteral("已选择：%1").arg(button->property("driveLetter").toString())
                                          : QStringLiteral("请选择恢复来源"));
    };
    connect(scanTargetGroup_, QOverload<QAbstractButton *>::of(&QButtonGroup::buttonClicked), this,
            [updateSelected](QAbstractButton *) { updateSelected(); });
    updateSelected();

    connect(imageButton, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择磁盘镜像或虚拟磁盘"), QString(),
            QStringLiteral("磁盘镜像 (*.img *.dd *.raw);;"
                           "虚拟磁盘 (*.vhd *.vhdx *.vmdk *.qcow2 *.vdi);;"
                           "所有文件 (*)"));
        if (!path.isEmpty()) {
            emit requestStartScan(QStringLiteral("磁盘镜像"), path);
        }
    });

    connect(findBtn_, &QPushButton::clicked, this, [this]() {
        if (!scanTargetGroup_) {
            return;
        }
        auto *button = scanTargetGroup_->checkedButton();
        if (!button) {
            QMessageBox::information(this, QStringLiteral("开始恢复"),
                                     QStringLiteral("请先选择一个恢复来源。"));
            return;
        }
        const QString drive = button->property("driveLetter").toString();
        if (!drive.isEmpty() && !WindowsElevation::IsRunningAsAdmin()) {
            const auto reply = QMessageBox::question(
                this, QStringLiteral("需要管理员权限"),
                QStringLiteral("扫描本地磁盘需要管理员权限。是否以管理员身份重启？"),
                QMessageBox::Yes | QMessageBox::No);
            if (reply == QMessageBox::Yes && WindowsElevation::RelaunchAsAdmin()) {
                QCoreApplication::quit();
            }
            return;
        }
        emit requestStartScan(QStringLiteral("本地磁盘"), drive);
    });
}

void RecoveryLocationPage::setSmartHealthStatus(bool available, bool risky, const QString &summary)
{
    if (!smartHealthBanner_ || !smartHealthStatusLabel_ || !smartHealthDetailLabel_) {
        return;
    }

    if (!available) {
        smartHealthBanner_->setStyleSheet(QStringLiteral(
            "#smartHealthBanner { background: #f8fafc; border: 1px solid #e2e8f0; border-radius: 10px; }"
            "#smartHealthBanner QLabel[role='status'] { color: #475569; font-size: 12px; font-weight: 750; }"
            "#smartHealthBanner QLabel[role='detail'] { color: #64748b; font-size: 10px; }"));
        smartHealthStatusLabel_->setText(QStringLiteral("设备健康：暂不可用"));
        smartHealthDetailLabel_->setText(summary.isEmpty()
                                             ? QStringLiteral("未能读取 SMART；程序仍会保持只读扫描。")
                                             : summary);
        return;
    }

    if (risky) {
        smartHealthBanner_->setStyleSheet(QStringLiteral(
            "#smartHealthBanner { background: #fff7ed; border: 1px solid #fed7aa; border-radius: 10px; }"
            "#smartHealthBanner QLabel[role='status'] { color: #c2410c; font-size: 12px; font-weight: 750; }"
            "#smartHealthBanner QLabel[role='detail'] { color: #9a3412; font-size: 10px; }"));
        smartHealthStatusLabel_->setText(QStringLiteral("设备健康：建议先制作镜像"));
        smartHealthDetailLabel_->setText(summary);
        return;
    }

    smartHealthBanner_->setStyleSheet(QStringLiteral(
        "#smartHealthBanner { background: #ecfdf5; border: 1px solid #bbf7d0; border-radius: 10px; }"
        "#smartHealthBanner QLabel[role='status'] { color: #15803d; font-size: 12px; font-weight: 750; }"
        "#smartHealthBanner QLabel[role='detail'] { color: #166534; font-size: 10px; }"));
    smartHealthStatusLabel_->setText(QStringLiteral("设备健康：状态良好"));
    smartHealthDetailLabel_->setText(summary.isEmpty()
                                         ? QStringLiteral("未发现明显介质风险，可以直接开始恢复。")
                                         : summary);
}

void RecoveryLocationPage::setLastSessionStatus(const QString &text, bool available)
{
    if (!lastScanLabel_) {
        return;
    }
    lastScanLabel_->setText(text);
    lastScanLabel_->setStyleSheet(available
        ? QStringLiteral("font-size: 11px; color: #15803d; font-weight: 650;")
        : QStringLiteral("font-size: 11px; color: #9aa7b8;"));
}
