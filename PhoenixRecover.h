#pragma once

#include <QtWidgets/QMainWindow>
#include "ui_PhoenixRecover.h"

class QCloseEvent;
class RecoveryFlowController;
class RecoveryScanController;
class SettingsPage;

class PhoenixRecover : public QMainWindow
{
    Q_OBJECT

public:
    PhoenixRecover(QWidget *parent = nullptr);
    ~PhoenixRecover();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    Ui::PhoenixRecoverClass ui;
    RecoveryFlowController *controller_ = nullptr;
    RecoveryScanController *scanController_ = nullptr;
    class QStackedWidget *outerStack_ = nullptr;
    class QWidget *wizardShell_ = nullptr;
    class QStackedWidget *wizardPages_ = nullptr;
    class RecoveryLocationPage *locationPage_ = nullptr;
    class RecoveryScanPage *scanPage_ = nullptr;
    SettingsPage *settingsPage_ = nullptr;
};
