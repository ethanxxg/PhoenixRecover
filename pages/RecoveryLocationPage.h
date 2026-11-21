#pragma once

#include <QString>
#include <QtWidgets/QWidget>

class QLabel;
class QButtonGroup;
class QFrame;
class QPushButton;
class QToolButton;

class RecoveryLocationPage : public QWidget
{
    Q_OBJECT

public:
    explicit RecoveryLocationPage(QWidget *parent = nullptr);
    void setSmartHealthStatus(bool available, bool risky, const QString &summary);
    void setLastSessionStatus(const QString &text, bool available);

signals:
    void requestStartScan(const QString &locationOption,
                          const QString &driveLetter);

private:
    QToolButton *recycleBtn_ = nullptr;
    QToolButton *desktopBtn_ = nullptr;
    QToolButton *folderBtn_ = nullptr;
    QPushButton *findBtn_ = nullptr;
    QButtonGroup *scanTargetGroup_ = nullptr;
    QLabel *selectedSourceLabel_ = nullptr;
    QLabel *lastScanLabel_ = nullptr;
    QFrame *smartHealthBanner_ = nullptr;
    QLabel *smartHealthStatusLabel_ = nullptr;
    QLabel *smartHealthDetailLabel_ = nullptr;
};
