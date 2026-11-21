#include "PhoenixRecover.h"
#include "common/AppVersion.h"
#include "pages/RecoveryLocationPage.h"
#include "pages/SDCardConnectionPage.h"
#include "pages/MoreRecoveryToolsPage.h"
#include "pages/SettingsPage.h"
#include "pages/FileRepairPage.h"
#include "pages/RecoveryScanPage.h"
#include "controllers/RecoveryFlowController.h"
#include "controllers/RecoveryScanController.h"
#include "disk/SmartInfo.h"
#include "log/Logger.h"

#include <QtCore/QList>
#include <QtCore/QMetaObject>
#include <QtCore/QPair>
#include <QtCore/QFileInfo>
#include <QtCore/QDateTime>
#include <QtCore/QFile>
#include <QtCore/QPointer>
#include <QtCore/QRunnable>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtCore/QThreadPool>
#include <QtGui/QCloseEvent>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QListWidgetItem>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStackedWidget>
#include <QtWidgets/QStyle>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <functional>

namespace {

class LambdaRunnable final : public QRunnable
{
public:
    explicit LambdaRunnable(std::function<void()> fn)
        : fn_(std::move(fn))
    {
    }

    void run() override
    {
        if (fn_) {
            fn_();
        }
    }

private:
    std::function<void()> fn_;
};

LogLevel logLevelFromSetting(int index)
{
    switch (index) {
    case 2: return LogLevel::Debug;
    case 1: return LogLevel::Warning;
    default: return LogLevel::Info;
    }
}

} // namespace

PhoenixRecover::PhoenixRecover(QWidget *parent)
    : QMainWindow(parent)
{
    ui.setupUi(this);
    {
        QSettings settings;
        Logger::Instance().SetLogLevel(
            logLevelFromSetting(qBound(0, settings.value(QStringLiteral("logging/level"), 0).toInt(), 2)));
    }
    setWindowTitle(QStringLiteral("PhoenixRecover ") + QStringLiteral(PHOENIX_VERSION_STRING));
    if (ui.menuBar) {
        ui.menuBar->hide();
    }
    if (ui.mainToolBar) {
        ui.mainToolBar->hide();
    }
    if (ui.statusBar) {
        ui.statusBar->hide();
    }

    controller_ = new RecoveryFlowController(this, this);
    scanController_ = new RecoveryScanController(this);

    resize(1200, 720);
    setMinimumSize(1200, 720);

    // 外层堆叠：0=向导（侧栏+白框），1=扫描页（铺满中央区域，盖住向导）
    ui.centralWidget->setStyleSheet(QStringLiteral("background: #ffffff;"));
    auto *outerStack = new QStackedWidget(ui.centralWidget);
    outerStack_ = outerStack;
    auto *cwLayout = new QVBoxLayout(ui.centralWidget);
    cwLayout->setContentsMargins(0, 0, 0, 0);
    cwLayout->setSpacing(0);
    cwLayout->addWidget(outerStack);

    auto *wizardShell = new QWidget();
    wizardShell_ = wizardShell;
    wizardShell->setStyleSheet(QStringLiteral("background: #ffffff;"));
    auto *rootLayout = new QHBoxLayout(wizardShell);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // 左侧导航
    auto *sidebar = new QListWidget(wizardShell);
    sidebar->setFixedWidth(220);
    sidebar->setFrameShape(QFrame::NoFrame);
    sidebar->setStyleSheet(QStringLiteral(
        "QListWidget {"
        "  background: #f7f9fc;"
        "  border: none;"
        "  border-right: 1px solid #e5e7eb;"
        "  border-radius: 0px;"
        "  outline: none;"
        "}"
        "QListWidget::item {"
        "  color: #1f2937;"
        "  padding: 12px 16px;"
        "  margin: 2px 10px;"
        "  border-radius: 10px;"
        "  border-left: 3px solid transparent;"
        "  font-size: 14px;"
        "}"
        "QListWidget::item:disabled {"
        "  background: transparent;"
        "  color: #6b7280;"
        "  font-weight: 600;"
        "  border-left: 3px solid transparent;"
        "  padding: 10px 16px 6px 16px;"
        "  margin: 12px 10px 4px 10px;"
        "}"
        "QListWidget::item:hover {"
        "  background: #ffffff;"
        "}"
        "QListWidget::item:selected,"
        "QListWidget::item:selected:active,"
        "QListWidget::item:selected:focus {"
        "  background: #eefbf3;"
        "  border-left: 3px solid #22c55e;"
        "  color: #15803d;"
        "  font-weight: 700;"
        "}"
    ));

    auto addHeader = [&](const QString &text) {
        auto *li = new QListWidgetItem(text, sidebar);
        li->setFlags(Qt::NoItemFlags);
        li->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        sidebar->addItem(li);
    };

    auto addNavItem = [&](const QString &text, QStyle::StandardPixmap iconType, int pageIndex) {
        auto *li = new QListWidgetItem(sidebar);
        li->setText(text);
        li->setIcon(style()->standardIcon(iconType));
        li->setData(Qt::UserRole, pageIndex);
        sidebar->addItem(li);
    };

    // 数据恢复
    addHeader(QStringLiteral("数据恢复"));
    addNavItem(QStringLiteral("磁盘"), QStyle::SP_DriveHDIcon, 0);
    addNavItem(QStringLiteral("SD 卡"), QStyle::SP_DriveNetIcon, 1);
    addNavItem(QStringLiteral("更多恢复工具"), QStyle::SP_ComputerIcon, 2);
    addNavItem(QStringLiteral("文件修复"), QStyle::SP_FileDialogContentsView, 3);

    // 高级功能
    addHeader(QStringLiteral("高级功能"));
    addNavItem(QStringLiteral("文件恢复"), QStyle::SP_FileIcon, 0);
    addNavItem(QStringLiteral("丢失分区恢复"), QStyle::SP_DirIcon, 0);

    sidebar->setCurrentRow(1);
    // 新版一页式入口不再展示旧向导侧栏；保留对象和路由兼容性，页面由顶部导航切换。
    sidebar->setVisible(false);
    rootLayout->addWidget(sidebar);

    // 右侧内容
    auto *main = new QFrame(wizardShell);
    main->setStyleSheet(QStringLiteral(
        "QFrame {"
        "  background: #ffffff;"
        "  border: none;"
        "  border-radius: 0px;"
        "}"
    ));
    auto *mainLayout = new QVBoxLayout(main);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // 新版顶部品牌栏：保留最少的全局入口，普通用户仍从首页一键开始恢复。
    auto *topbar = new QFrame(main);
    topbar->setFixedHeight(64);
    topbar->setStyleSheet(QStringLiteral(
        "QFrame { background: #ffffff; border-bottom: 1px solid #e5eaf1; }"));
    auto *topbarLayout = new QHBoxLayout(topbar);
    topbarLayout->setContentsMargins(28, 0, 28, 0);
    topbarLayout->setSpacing(12);

    auto *brandMark = new QLabel(QStringLiteral("P"), topbar);
    brandMark->setAlignment(Qt::AlignCenter);
    brandMark->setFixedSize(34, 34);
    brandMark->setStyleSheet(QStringLiteral(
        "QLabel { background: #2563eb; color: white; border-radius: 10px;"
        " font-size: 17px; font-weight: 800; }"));
    topbarLayout->addWidget(brandMark);

    auto *brandName = new QLabel(QStringLiteral("PhoenixRecover"), topbar);
    brandName->setStyleSheet(QStringLiteral(
        "font-size: 16px; font-weight: 750; color: #172033;"));
    topbarLayout->addWidget(brandName);
    topbarLayout->addStretch(1);

    auto *healthButton = new QPushButton(QStringLiteral("● 设备健康：查看"), topbar);
    auto *toolsButton = new QPushButton(QStringLiteral("工具"), topbar);
    auto *settingsButton = new QPushButton(QStringLiteral("设置"), topbar);
    auto *helpButton = new QPushButton(QStringLiteral("帮助"), topbar);
    const QString topButtonStyle = QStringLiteral(
        "QPushButton { border: 0; padding: 8px 10px; color: #718096;"
        " background: transparent; font-size: 12px; }"
        "QPushButton:hover { color: #2563eb; background: #eff6ff; border-radius: 8px; }");
    healthButton->setStyleSheet(QStringLiteral(
        "QPushButton { border: 1px solid #bbf7d0; border-radius: 999px;"
        " padding: 7px 11px; color: #15803d; background: #ecfdf3; font-size: 12px; }"
        "QPushButton:hover { background: #dcfce7; }") );
    toolsButton->setStyleSheet(topButtonStyle);
    settingsButton->setStyleSheet(topButtonStyle);
    helpButton->setStyleSheet(topButtonStyle);
    topbarLayout->addWidget(healthButton);
    topbarLayout->addWidget(toolsButton);
    topbarLayout->addWidget(settingsButton);
    topbarLayout->addWidget(helpButton);
    mainLayout->addWidget(topbar);

    auto *stack = new QStackedWidget(main);
    wizardPages_ = stack;
    stack->setFrameShape(QFrame::NoFrame);
    stack->setStyleSheet(QStringLiteral("QStackedWidget { background: transparent; }"));
    auto *content = new QWidget(main);
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(28, 20, 28, 24);
    contentLayout->setSpacing(0);
    contentLayout->addWidget(stack, 1);
    mainLayout->addWidget(content, 1);
    rootLayout->addWidget(main, 1);
    outerStack->addWidget(wizardShell);

    // 页面创建
    auto *page1 = new RecoveryLocationPage(stack);
    locationPage_ = page1;
    stack->addWidget(page1);
    {
        QSettings settings;
        const QString lastSessionPath = settings.value(QStringLiteral("session/lastPath")).toString();
        const QFileInfo sessionInfo(lastSessionPath);
        if (!lastSessionPath.isEmpty() && sessionInfo.exists() && sessionInfo.isFile()) {
            page1->setLastSessionStatus(
                QStringLiteral("上次扫描：%1 · 可继续")
                    .arg(sessionInfo.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm"))),
                true);
        } else {
            page1->setLastSessionStatus(QStringLiteral("上次扫描：暂无可继续会话"), false);
        }
    }

    auto *sdPage = new SDCardConnectionPage(stack);
    stack->addWidget(sdPage);

    auto *toolsPage = new MoreRecoveryToolsPage(stack);
    stack->addWidget(toolsPage);

    auto *repairPage = new FileRepairPage(stack);
    stack->addWidget(repairPage);

    auto *settingsPage = new SettingsPage(stack);
    settingsPage_ = settingsPage;
    stack->addWidget(settingsPage);

    auto *scanPage = new RecoveryScanPage();
    scanPage_ = scanPage;
    scanPage->setResultStore(scanController_->resultStore());
    outerStack->addWidget(scanPage);

    stack->setCurrentWidget(page1);
    outerStack->setCurrentWidget(wizardShell);

    // 将所有信号连接委托给 FlowController
    controller_->init(outerStack, stack, wizardShell,
                      scanController_, scanPage,
                      page1, sdPage, toolsPage, repairPage);

    connect(toolsButton, &QPushButton::clicked, controller_,
            &RecoveryFlowController::navigateToMoreTools);
    connect(healthButton, &QPushButton::clicked, this, [this, toolsPage]() {
        controller_->navigateToMoreTools();
        toolsPage->showToolPage(QStringLiteral("SMART 健康检测"));
    });
    connect(settingsButton, &QPushButton::clicked, this, [stack, settingsPage]() {
        stack->setCurrentWidget(settingsPage);
    });
    connect(settingsPage, &SettingsPage::requestBack, controller_,
            &RecoveryFlowController::navigateToLocation);
    connect(helpButton, &QPushButton::clicked, this, [this]() {
        QMessageBox::information(this, QStringLiteral("PhoenixRecover"),
                                 QStringLiteral("选择设备后点击“开始恢复”。系统会自动选择扫描策略，源盘保持只读。"));
    });

    // 首页设备健康状态采用后台只读查询，避免 SMART 读取阻塞首页首次打开。
    {
        QPointer<RecoveryLocationPage> locationPageGuard(page1);
        QPointer<QPushButton> healthButtonGuard(healthButton);
        auto *probe = new LambdaRunnable([locationPageGuard, healthButtonGuard]() {
            const bool testMode = qEnvironmentVariableIsSet("PHOENIX_TEST_MODE") ||
                                  !qgetenv("PHOENIX_MOCK_SCAN_COUNT").isEmpty();
            if (testMode) {
                QMetaObject::invokeMethod(qApp, [locationPageGuard, healthButtonGuard]() {
                    if (locationPageGuard) {
                        locationPageGuard->setSmartHealthStatus(
                            false, false,
                            QStringLiteral("测试模式：不访问物理设备，当前验证只使用测试数据。"));
                    }
                    if (healthButtonGuard) {
                        healthButtonGuard->setText(QStringLiteral("● 测试模式：未读取设备"));
                        healthButtonGuard->setStyleSheet(QStringLiteral(
                            "QPushButton { border: 1px solid #e2e8f0; border-radius: 999px;"
                            " padding: 7px 11px; color: #475569; background: #f8fafc; font-size: 12px; }"
                            "QPushButton:hover { background: #f1f5f9; }"));
                    }
                }, Qt::QueuedConnection);
                return;
            }
            const std::vector<SmartInfo> infos = SmartReader::ReadAllDisks();
            bool available = false;
            bool risky = false;
            QString detail;
            for (const SmartInfo &info : infos) {
                if (info.smartSupported || info.health != DiskHealthStatus::Unknown) {
                    available = true;
                }
                const bool diskRisk = info.health == DiskHealthStatus::Warning ||
                                      info.health == DiskHealthStatus::Critical ||
                                      info.health == DiskHealthStatus::Failing ||
                                      info.pendingSectors > 0 ||
                                      info.uncorrectableSectors > 0 ||
                                      info.reallocatedSectors > 0;
                if (diskRisk && detail.isEmpty()) {
                    risky = true;
                    detail = QStringLiteral("%1 · 重映射 %2 · 待定 %3 · 不可校正 %4")
                                 .arg(QString::fromStdWString(SmartReader::HealthStatusText(info.health)))
                                 .arg(info.reallocatedSectors)
                                 .arg(info.pendingSectors)
                                 .arg(info.uncorrectableSectors);
                }
            }
            if (!available && !infos.empty()) {
                detail = QStringLiteral("设备未提供可用 SMART 属性，程序仍会保持只读扫描。让扫描继续即可。 ");
            } else if (available && !risky) {
                detail = QStringLiteral("已完成只读检测，未发现明显介质风险。可以直接开始恢复。 ");
            } else if (infos.empty()) {
                detail = QStringLiteral("未检测到可读取 SMART 的物理磁盘，程序仍会保持只读扫描。 ");
            }

            QMetaObject::invokeMethod(qApp, [locationPageGuard, healthButtonGuard,
                                               available, risky, detail]() {
                if (locationPageGuard) {
                    locationPageGuard->setSmartHealthStatus(available, risky, detail);
                }
                if (healthButtonGuard) {
                    healthButtonGuard->setText(risky ? QStringLiteral("● 设备健康：建议镜像")
                                                     : (available ? QStringLiteral("● 设备健康：正常")
                                                                  : QStringLiteral("● 设备健康：暂不可用")));
                    healthButtonGuard->setStyleSheet(risky
                        ? QStringLiteral(
                              "QPushButton { border: 1px solid #fed7aa; border-radius: 999px;"
                              " padding: 7px 11px; color: #c2410c; background: #fff7ed; font-size: 12px; }"
                              "QPushButton:hover { background: #ffedd5; }")
                        : (available
                            ? QStringLiteral(
                                  "QPushButton { border: 1px solid #bbf7d0; border-radius: 999px;"
                                  " padding: 7px 11px; color: #15803d; background: #ecfdf3; font-size: 12px; }"
                                  "QPushButton:hover { background: #dcfce7; }")
                            : QStringLiteral(
                                  "QPushButton { border: 1px solid #e2e8f0; border-radius: 999px;"
                                  " padding: 7px 11px; color: #475569; background: #f8fafc; font-size: 12px; }"
                                  "QPushButton:hover { background: #f1f5f9; }")));
                }
            }, Qt::QueuedConnection);
        });
        probe->setAutoDelete(true);
        QThreadPool::globalInstance()->start(probe);
    }

    // 侧栏路由：提取 pageIndex 后转发给 FlowController
    connect(sidebar,
            &QListWidget::currentRowChanged,
            this,
            [this, sidebar](int /*row*/) {
                const auto *item = sidebar->currentItem();
                if (!item) {
                    return;
                }
                const QVariant v = item->data(Qt::UserRole);
                if (!v.isValid()) {
                    return;
                }
                controller_->onSidebarChanged(v.toInt());
            });

    // 尝试加载上次扫描会话
    controller_->tryLoadLastSession();

    // 仅用于显式 UI 烟测：PHOENIX_TEST_MODE + PHOENIX_UI_SMOKE 才会进入，
    // 自动验证模拟扫描页和结果页，不访问真实设备，也不执行恢复写出。
    if (qEnvironmentVariableIsSet("PHOENIX_UI_SMOKE") &&
        (qEnvironmentVariableIsSet("PHOENIX_TEST_MODE") ||
         !qgetenv("PHOENIX_MOCK_SCAN_COUNT").isEmpty())) {
        const QString reportPath = QString::fromLocal8Bit(qgetenv("PHOENIX_UI_SMOKE_REPORT"));
        auto writeSmokeReport = [reportPath](const QStringList &lines, int exitCode) {
            if (!reportPath.isEmpty()) {
                QFile report(reportPath);
                if (report.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
                    report.write(lines.join(QLatin1Char('\n')).toUtf8());
                    report.write("\n");
                    report.close();
                }
            }
            QCoreApplication::exit(exitCode);
        };

        const bool partitionSmoke = qEnvironmentVariableIsSet("PHOENIX_PARTITION_UI_SMOKE");
        QTimer::singleShot(0, this, [this, partitionSmoke]() {
            if (partitionSmoke) {
                controller_->onToolRecoveryRequested(QStringLiteral("分区探测"));
            } else {
                controller_->startScan(QStringLiteral("本地磁盘"), QStringLiteral("TEST_DATA"));
            }
        });
        if (partitionSmoke) {
            QTimer::singleShot(450, this, [this, writeSmokeReport]() {
                auto combos = findChildren<QComboBox *>();
                QComboBox *candidateCombo = nullptr;
                for (QComboBox *combo : combos) {
                    if (combo->isVisible() && combo->count() >= 2) {
                        candidateCombo = combo;
                        break;
                    }
                }
                auto buttons = findChildren<QPushButton *>();
                QPushButton *scanCandidateButton = nullptr;
                for (QPushButton *button : buttons) {
                    if (button->text() == QStringLiteral("扫描选中的分区")) {
                        scanCandidateButton = button;
                        break;
                    }
                }
                if (!candidateCombo || !scanCandidateButton) {
                    QStringList lines;
                    lines << QStringLiteral("UI_SMOKE_START")
                          << QStringLiteral("PARTITION_CANDIDATES=0")
                          << QStringLiteral("UI_SMOKE_RESULT=FAIL");
                    writeSmokeReport(lines, 4);
                    return;
                }
                candidateCombo->setCurrentIndex(0);
                scanCandidateButton->click();
            });
        }
        QTimer::singleShot(900, this, [this, writeSmokeReport]() {
            QStringList lines;
            lines << QStringLiteral("UI_SMOKE_START");
            auto *scanCard = scanPage_->findChild<QFrame *>(QStringLiteral("scanStatusCard"));
            const bool scanCardVisible = scanCard && scanCard->isVisible();
            lines << QStringLiteral("SCAN_CARD_VISIBLE=%1").arg(scanCardVisible ? 1 : 0);
            auto buttons = scanPage_->findChildren<QPushButton *>();
            QPushButton *previewButton = nullptr;
            for (QPushButton *button : buttons) {
                if (button->text() == QStringLiteral("查看已发现文件")) {
                    previewButton = button;
                    break;
                }
            }
            lines << QStringLiteral("PREVIEW_BUTTON_FOUND=%1").arg(previewButton ? 1 : 0);
            if (!scanCardVisible || !previewButton) {
                lines << QStringLiteral("UI_SMOKE_FAIL=SCAN_STAGE");
                writeSmokeReport(lines, 2);
                return;
            }
            previewButton->click();
            QTimer::singleShot(250, this, [this, writeSmokeReport, lines]() mutable {
                const auto *summary = scanPage_->findChild<QFrame *>(QStringLiteral("resultsSummaryWrap"));
                lines << QStringLiteral("RESULTS_SUMMARY_VISIBLE=%1")
                             .arg(summary && summary->isVisible() ? 1 : 0);
                lines << QStringLiteral("UI_SMOKE_RESULT=%1")
                             .arg(summary && summary->isVisible() ? QStringLiteral("PASS")
                                                                  : QStringLiteral("FAIL"));
                writeSmokeReport(lines, summary && summary->isVisible() ? 0 : 3);
            });
        });
    }
}

PhoenixRecover::~PhoenixRecover()
{}

void PhoenixRecover::closeEvent(QCloseEvent *event)
{
    if (controller_->hasUnsavedSession()) {
        const auto reply = QMessageBox::question(
            this,
            QStringLiteral("保存扫描结果"),
            QStringLiteral("当前有未保存的扫描结果，是否保存？"),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
        if (reply == QMessageBox::Save) {
            controller_->saveCurrentSession();
        } else if (reply == QMessageBox::Cancel) {
            event->ignore();
            return;
        }
    }
    event->accept();
}
