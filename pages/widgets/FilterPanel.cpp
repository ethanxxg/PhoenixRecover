#include "FilterPanel.h"

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>

FilterPanel::FilterPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    searchEdit_ = new QLineEdit(this);
    searchEdit_->setPlaceholderText(QStringLiteral("搜索文件或文件夹"));
    searchEdit_->setFixedHeight(36);
    searchEdit_->setMinimumWidth(220);
    searchEdit_->setStyleSheet(QStringLiteral(
        "QLineEdit { border: 1px solid #d1d5db; border-radius: 10px; padding: 0 12px; background: #ffffff; color: #111827; }"
        "QLineEdit:focus { border-color: #86efac; }"));
    layout->addWidget(searchEdit_);

    searchBtn_ = new QPushButton(QStringLiteral("搜索"), this);
    searchBtn_->setFixedHeight(36);
    searchBtn_->setMinimumWidth(56);
    searchBtn_->setStyleSheet(QStringLiteral(
        "QPushButton { border: 1px solid #d1d5db; border-radius: 10px; background: #ffffff; color: #374151; font-size: 14px; font-weight: 700; padding: 0; }"
        "QPushButton:hover { background: #f9fafb; }"));
    layout->addWidget(searchBtn_);

    connect(searchBtn_, &QPushButton::clicked, this, [this]() {
        emit filterChanged(searchEdit_->text().trimmed());
    });
    connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString &text) {
        emit filterChanged(text.trimmed());
    });
}

QString FilterPanel::searchKeyword() const
{
    return searchEdit_ ? searchEdit_->text().trimmed() : QString();
}

void FilterPanel::reset()
{
    if (searchEdit_) {
        searchEdit_->clear();
    }
}
