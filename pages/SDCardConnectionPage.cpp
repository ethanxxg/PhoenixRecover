#include "SDCardConnectionPage.h"

#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>

SDCardConnectionPage::SDCardConnectionPage(QWidget *parent)
    : QWidget(parent)
{
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(18);

    auto *title = new QLabel(QStringLiteral("连接存储卡，恢复丢失的照片和视频"), this);
    title->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    title->setStyleSheet(QStringLiteral("font-size: 20px; font-weight: 600; color: #111827;"));
    pageLayout->addWidget(title);

    // 主提示卡片（近似截图：居中插图 + 提示文案）
    auto *infoCard = new QFrame(this);
    infoCard->setFrameShape(QFrame::StyledPanel);
    infoCard->setStyleSheet(QStringLiteral(
        "QFrame {"
        "  background: white;"
        "  border: 1px solid #e6eaf2;"
        "  border-radius: 16px;"
        "}"
    ));

    auto *cardLayout = new QVBoxLayout(infoCard);
    cardLayout->setContentsMargins(24, 22, 24, 22);
    cardLayout->setSpacing(14);

    auto *iconLabel = new QLabel(infoCard);
    iconLabel->setAlignment(Qt::AlignHCenter);
    iconLabel->setPixmap(style()->standardIcon(QStyle::SP_DriveNetIcon).pixmap(96, 96));
    cardLayout->addWidget(iconLabel);

    auto *desc = new QLabel(
        QStringLiteral("通过连接存储卡，将 SD 卡按说明连接到电脑或设备。随后开始扫描，找回丢失的图片和视频。"),
        infoCard);
    desc->setAlignment(Qt::AlignHCenter);
    desc->setWordWrap(true);
    desc->setStyleSheet(QStringLiteral("font-size: 14px; color: #6b7280;"));
    cardLayout->addWidget(desc);

    pageLayout->addWidget(infoCard, 1);

    // 底部按钮
    auto *bottomBar = new QHBoxLayout();
    bottomBar->setContentsMargins(0, 0, 0, 0);
    bottomBar->addStretch(1);

    startBtn_ = new QPushButton(QStringLiteral("开始检测并恢复"), this);
    startBtn_->setFixedHeight(40);
    startBtn_->setMinimumWidth(220);
    startBtn_->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: #1890ff;"
        "  color: white;"
        "  border: none;"
        "  border-radius: 8px;"
        "  font-size: 15px;"
        "  padding: 8px 14px;"
        "}"
        "QPushButton:hover { background: #1677ff; }"
    ));
    bottomBar->addWidget(startBtn_);

    pageLayout->addLayout(bottomBar);

    connect(startBtn_, &QPushButton::clicked, this, [this]() {
        emit requestSdCardRecovery();
    });
}

