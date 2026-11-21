#pragma once

#include <QtWidgets/QWidget>

class QPushButton;

class SDCardConnectionPage : public QWidget
{
    Q_OBJECT

public:
    explicit SDCardConnectionPage(QWidget *parent = nullptr);

signals:
    // UI 只表达用户意图：当用户点击“开始检测”后由控制层执行后续流程
    void requestSdCardRecovery();

private:
    QPushButton *startBtn_ = nullptr;
};

