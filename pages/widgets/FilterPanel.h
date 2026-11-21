#pragma once

#include <QtWidgets/QWidget>

class QLineEdit;
class QPushButton;

class FilterPanel : public QWidget
{
    Q_OBJECT

public:
    explicit FilterPanel(QWidget *parent = nullptr);

    QString searchKeyword() const;
    void reset();

    QLineEdit *searchEdit() const { return searchEdit_; }
    QPushButton *searchBtn() const { return searchBtn_; }

signals:
    void filterChanged(const QString &keyword);

private:
    QLineEdit *searchEdit_ = nullptr;
    QPushButton *searchBtn_ = nullptr;
};
