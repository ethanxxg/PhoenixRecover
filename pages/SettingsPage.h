#pragma once

#include <QtWidgets/QWidget>

class QCheckBox;

class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(QWidget *parent = nullptr);

signals:
    void requestBack();

private:
    QCheckBox *readOnlyCheck_ = nullptr;
    QCheckBox *imageFirstCheck_ = nullptr;
    QCheckBox *confirmTargetCheck_ = nullptr;
    QCheckBox *keepTreeCheck_ = nullptr;
};
