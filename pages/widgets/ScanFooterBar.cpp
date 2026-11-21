#include "ScanFooterBar.h"

#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>

ScanFooterBar::ScanFooterBar(QWidget *parent)
    : QWidget(parent)
{
    setStyleSheet(QStringLiteral(
        "QFrame { border-top: 1px solid #e5e7eb; background: #ffffff; }"));

    auto *footerLayout = new QHBoxLayout(this);
    footerLayout->setContentsMargins(26, 10, 26, 10);
    footerLayout->setSpacing(10);

    footerSubLabel_ = new QLabel(QStringLiteral("恢复前可继续筛选并勾选需要的文件"), this);
    footerSubLabel_->setStyleSheet(QStringLiteral("font-size: 12px; color: #6b7280;"));
    footerSubLabel_->hide();
    footerLayout->addWidget(footerSubLabel_);

    footerLayout->addStretch(1);

    keepDirStructureCheck_ = new QCheckBox(QStringLiteral("保留原目录结构"), this);
    keepDirStructureCheck_->setChecked(true);
    keepDirStructureCheck_->setStyleSheet(QStringLiteral(
        "QCheckBox { color: #374151; font-size: 12px; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #e5e7eb; border-radius: 4px; background: #ffffff; }"
        "QCheckBox::indicator:hover { border-color: #16a34a; background: #ffffff; }"
        "QCheckBox::indicator:checked { border: 1px solid #16a34a; border-radius: 4px; background: #16a34a;"
        " image: url(:/qt-project.org/styles/commonstyle/images/checkbox_checked.png); }"));
    keepDirStructureCheck_->hide();
    footerLayout->addWidget(keepDirStructureCheck_);

    recoverAllBtn_ = new QPushButton(QStringLiteral("恢复全部"), this);
    recoverAllBtn_->setFixedHeight(40);
    recoverAllBtn_->setMinimumWidth(118);
    recoverAllBtn_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #16a34a; color: #ffffff; border: none; border-radius: 10px; font-size: 14px; font-weight: 700; padding: 0 20px; }"
        "QPushButton:hover { background: #15803d; }"
        "QPushButton:disabled { background: #bbf7d0; color: #ffffff; }"));
    recoverAllBtn_->setEnabled(false);
    recoverAllBtn_->hide();
    footerLayout->addWidget(recoverAllBtn_);

    connect(recoverAllBtn_, &QPushButton::clicked, this, &ScanFooterBar::recoverClicked);
}

void ScanFooterBar::setFileCount(int total, int checked)
{
    Q_UNUSED(total);
    if (recoverAllBtn_) {
        recoverAllBtn_->setEnabled(checked > 0);
    }
    if (footerSubLabel_ && footerSubLabel_->isVisible()) {
        footerSubLabel_->setText(
            total > 0 ? QStringLiteral("扫描完成，找到文件：%1").arg(total)
                      : QStringLiteral("恢复前可继续筛选并勾选需要的文件"));
    }
}

void ScanFooterBar::setEnabled(bool enabled)
{
    if (recoverAllBtn_) {
        recoverAllBtn_->setEnabled(enabled);
    }
}

void ScanFooterBar::setResultsMode(bool enabled)
{
    if (footerSubLabel_) footerSubLabel_->setVisible(enabled);
    if (keepDirStructureCheck_) keepDirStructureCheck_->setVisible(enabled);
    if (recoverAllBtn_) recoverAllBtn_->setVisible(enabled);
}

bool ScanFooterBar::keepDirStructure() const
{
    return keepDirStructureCheck_ ? keepDirStructureCheck_->isChecked() : true;
}
