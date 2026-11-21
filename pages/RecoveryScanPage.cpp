#include "RecoveryScanPage.h"

#include "widgets/ScanProgressBar.h"
#include "widgets/PreviewPanel.h"
#include "widgets/ScanFooterBar.h"
#include "widgets/FilterPanel.h"

#include "../controllers/ScanResultStore.h"
#include "../models/ScanResultModel.h"
#include "../models/ScanResultFilterModel.h"
#include "../disk/SectorReader.h"
#include "../log/Logger.h"
#include "../recover/FileNode.h"
#include "../recover/RecoverEngine.h"
#include "../media/MediaThumbnails.h"
#include "../media/GridPreviewExtensions.h"
#include "../common/UiFileTypeCatalog.h"
#include "../repair/RepairFormatUtil.h"
#include "../repair/ZipEntryList.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <thread>
#include <vector>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QStandardPaths>
#include <QtCore/QStorageInfo>
#include <QtCore/QMetaObject>
#include <QtCore/QSignalBlocker>
#include <QtCore/QSettings>
#include <QtCore/QTimer>
#include <QtCore/QThreadPool>
#include <QtCore/QThread>
#include <QtCore/QRunnable>
#include <QtCore/QTemporaryFile>
#include <QtCore/QItemSelectionModel>
#include <QtCore/QLocale>
#include <QtCore/QDateTime>
#include <QtCore/QDate>
#include <QtCore/QTime>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QFrame>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QMenu>
#include <QtWidgets/QAction>
#include <QtWidgets/QProgressBar>
#include <QtWidgets/QProgressDialog>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QListWidgetItem>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableView>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QTableWidgetItem>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QTreeWidgetItem>
#include <QtCore/QSettings>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QStyle>
#include <QtWidgets/QStackedWidget>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPixmap>
#include <QtGui/QDesktopServices>
#include <QtGui/QResizeEvent>
#include <QtCore/QUrl>

namespace {

class LambdaRunnable : public QRunnable {
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

static std::wstring QStringToWString(const QString &s)
{
    return std::wstring(reinterpret_cast<const wchar_t *>(s.utf16()), static_cast<size_t>(s.size()));
}

static QString recoveryStatusLabel(RecoverStatus st)
{
    switch (st) {
    case RecoverStatus::Success:
        return QStringLiteral("成功");
    case RecoverStatus::Partial:
        return QStringLiteral("部分恢复");
    case RecoverStatus::Failed:
        return QStringLiteral("失败");
    case RecoverStatus::Skipped:
        return QStringLiteral("跳过");
    }
    return QStringLiteral("未知");
}

static QString buildRecoverBatchDetailText(const RecoverBatchResult &batch)
{
    QStringList lines;
    lines.reserve(static_cast<int>(batch.details.size()));
    for (size_t i = 0; i < batch.details.size(); ++i) {
        const FileRecoverResult &item = batch.details[i];
        const QString name = (item.node && !item.node->name.empty())
                                 ? QString::fromStdWString(item.node->name)
                                 : QStringLiteral("(unknown)");
        const QString target = item.targetPath.empty()
                                   ? QStringLiteral("-")
                                   : QDir::toNativeSeparators(QString::fromStdWString(item.targetPath));
        const QString reason = item.reason.empty()
                                   ? QStringLiteral("-")
                                   : QString::fromStdWString(item.reason);
        lines.append(QStringLiteral("[%1] %2 | 目标: %3 | 原因: %4")
                         .arg(recoveryStatusLabel(item.status), name, target, reason));
    }
    return lines.join(QStringLiteral("\n"));
}

static bool exportRecoverDetailsToFile(const QString &path, const QString &details, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (error) {
            *error = f.errorString();
        }
        return false;
    }
    const QByteArray bom("\xEF\xBB\xBF", 3);
    f.write(bom);
    f.write(details.toUtf8());
    f.close();
    return true;
}

static bool openScanSourceReader(const QString &sourceId,
                                 ScanResultStore *store,
                                 SectorReader &reader)
{
    Partition part;
    if (store && store->tryGetScanVolumePartition(&part) && part.physicalDiskIdx >= 0) {
        return reader.Open(part.physicalDiskIdx);
    }
    return !sourceId.isEmpty() && reader.OpenVolume(QStringToWString(sourceId));
}

static QByteArray loadPreviewThumbBytes(const QString &driveLetter, ScanResultStore *store, quint64 id)
{
    if (!store || driveLetter.isEmpty()) {
        return {};
    }
    ScanFileDto dto;
    if (!store->tryGetFile(id, &dto)) {
        return {};
    }
    const QString extLower = dto.extension.trimmed().toLower();
    const uint32_t byteCap = GridPreview::effectivePreviewReadCap(extLower, dto.sizeBytes);
    switch (dto.previewKind) {
    case ScanPreviewKind::None:
        return {};
    case ScanPreviewKind::ResidentBlob: {
        QByteArray b;
        if (!store->tryGetPreviewResidentBlob(id, &b)) {
            return {};
        }
        return b;
    }
    case ScanPreviewKind::VolumeSlice: {
        uint32_t len = dto.previewReadLength;
        if (len == 0) {
            return {};
        }
        if (len > byteCap) {
            len = byteCap;
        }
        SectorReader reader;
        if (!openScanSourceReader(driveLetter, store, reader)) {
            return {};
        }
        QByteArray out(static_cast<int>(len), '\0');
        if (!reader.ReadBytes(dto.previewVolumeOffset, reinterpret_cast<uint8_t *>(out.data()), len)) {
            return {};
        }
        return out;
    }
    case ScanPreviewKind::FullNode: {
        FileNode node;
        if (!store->tryGetPreviewFileNode(id, &node)) {
            return {};
        }
        Partition part;
        if (!store->tryGetScanVolumePartition(&part)) {
            return {};
        }
        SectorReader reader;
        if (!openScanSourceReader(driveLetter, store, reader)) {
            return {};
        }
        std::vector<uint8_t> buf;
        if (!RecoverEngine::ReadFileDataPrefix(reader, part, node, byteCap, buf) || buf.empty()) {
            return {};
        }
        return QByteArray(reinterpret_cast<const char *>(buf.data()), static_cast<int>(buf.size()));
    }
    }
    return {};
}

enum class TreeItemKind {
    Root,
    Directory,
    File,
    Category
};

QString baseTreeLabel(QTreeWidgetItem *item)
{
    const QString base = item->data(0, Qt::UserRole + 2).toString();
    return base.isEmpty() ? item->text(0) : base;
}

TreeItemKind treeItemKind(QTreeWidgetItem *item)
{
    return static_cast<TreeItemKind>(item->data(0, Qt::UserRole + 4).toInt());
}



void applyTreeItemTypography(QTreeWidgetItem *item)
{
    const TreeItemKind kind = treeItemKind(item);

    QFont labelFont;
    labelFont.setFamily(QStringLiteral("Microsoft YaHei"));
    QFont countFont = labelFont;

    switch (kind) {
    case TreeItemKind::Root:
        labelFont.setPointSize(10);
        labelFont.setWeight(QFont::Bold);
        countFont.setPointSize(9);
        countFont.setWeight(QFont::DemiBold);
        break;
    case TreeItemKind::Directory:
    case TreeItemKind::Category:
        labelFont.setPointSize(9);
        labelFont.setWeight(QFont::DemiBold);
        countFont.setPointSize(9);
        countFont.setWeight(QFont::Normal);
        break;
    case TreeItemKind::File:
    default:
        labelFont.setPointSize(9);
        labelFont.setWeight(QFont::Normal);
        countFont.setPointSize(9);
        countFont.setWeight(QFont::Normal);
        break;
    }

    item->setFont(0, labelFont);
    item->setFont(1, countFont);
}

void enableTreeCheckbox(QTreeWidgetItem *item, bool checked = false)
{
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    item->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
}

QString itemRelativeFolderPath(QTreeWidgetItem *item)
{
    return item->data(0, Qt::UserRole + 5).toString();
}

void setMetricLabelText(QLabel *label, const QString &value, const QString &caption)
{
    if (!label) {
        return;
    }
    label->setText(QStringLiteral("<b style='color:#111827;'>%1</b> <span style='color:#6b7280;'>%2</span>")
                       .arg(value, caption));
}

QString formatTreeMetricSize(qint64 bytes)
{
    return bytes > 0 ? QLocale::system().formattedDataSize(bytes, 2, QLocale::DataSizeIecFormat)
                     : QString();
}

QString formatElapsedCompact(qint64 elapsedMs)
{
    if (elapsedMs < 0) {
        elapsedMs = 0;
    }
    const qint64 totalSeconds = elapsedMs / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

const QString &documentExtensionsCsv()
{
    static const QString s = QStringLiteral(
        "doc,docx,pdf,xls,xlsx,ppt,pptx,3ds,accdb,addin,ascii,bak,cfg,chk,chm,config,cpp,crwl,cs,css,csv,db,dot,"
        "dotm,dotx,dox,dwg,enc,eot,epub,et,fcpevent,fdf,fft,fil,fs,gpx,gthr,htm,html,ics,inf,ipynb,java,js,json,"
        "key,lit,lnk,log,lst,lzh,m,manifest,md,mdb,me,mhtml,mm,mmap,mobi,mof,msg,numbers,odg,odp,ods,odt,old,ort,"
        "ott,p7s,pem,pfx,php,plist,pot,potx,pub,py,rad,readme,rpt,rst,rtf,sam,scpt,sitx,slk,sin,srt,swift,sxc,tab,"
        "text,thmx,tlb,tm,ttf,txt,url,utf8,vbs,vcf,vcproj,woff,woff2,wpl,wps,wpt,xib,xlf,xmind,xml,xsi,yml");
    return s;
}

const QString &audioExtensionsCsv()
{
    static const QString s = QStringLiteral(
        "mp3,wav,wma,m4a,ape,flac,aac,acc,acm,aifc,aiff,al,amr,au,dls,dsp,emx,flp,ins,jam,lib,mid,midi,mpc,mpga,"
        "mui,pga,ogg,qcp,rbs,sb,sct,smp,sng,tsp,val,voc");
    return s;
}

const QString &mailExtensionsCsv()
{
    static const QString s = QStringLiteral("edb,eml,idx,pst");
    return s;
}

const QString &archiveExtensionsCsv()
{
    static const QString s = QStringLiteral(
        "zip,rar,7z,tar,apk,bat,bz2,cab,cat,cbt,deb,dist,gz,img,iso,jar,lzma,msi,otf,pack,pak,pf,pfb,pup,ro,rev,sdi,sfx,"
        "sh,tgz,ttc,wim,xz,n,zap,zfsendtotarget,z");
    return s;
}

QStringList fixedTypeChildrenForCategory(const QString &category)
{
    for (const auto& cat : UiFileTypeCatalog::Categories()) {
        if (category == QString::fromWCharArray(cat.name)) {
            QStringList out;
            for (const wchar_t* ext : cat.extensions)
                out << QString::fromWCharArray(ext);
            return out;
        }
    }

    return QStringList();
}

QString otherFixedSubtypeForUi(const QString &ext)
{
    const QString e = ext.trimmed().toLower();
    const auto allowed = UiFileTypeCatalog::ExtensionsForCategory(L"其他");
    for (const auto& a : allowed) {
        if (QString::fromStdWString(a).compare(e, Qt::CaseInsensitive) == 0)
            return e;
    }
    return QStringLiteral("其他");
}

void refreshTreeCaption(QTreeWidgetItem *item)
{
    const TreeItemKind kind = treeItemKind(item);
    const int count = item->data(0, Qt::UserRole + 3).toInt();
    const qint64 directFileBytes = item->data(0, Qt::UserRole + 10).toLongLong();
    QString label = baseTreeLabel(item);
    // 类型树固定类需要严格显示固定顺序；如果出现 store 写入脏数据导致 subLabel
    // 不在固定集合里，则降级为「其他」，避免乱码字符显示。
    if (kind == TreeItemKind::File) {
        const QString cat = item->data(0, Qt::UserRole + 6).toString();
        const QString sub = item->data(0, Qt::UserRole + 7).toString();
        // 仅对「其他」做严格校验：它的固定集合很小，且你截图里的乱码主要出现在这里。
        if (cat == QStringLiteral("其他")) {
            const QStringList fixed = fixedTypeChildrenForCategory(cat);
            if (!fixed.isEmpty() && !fixed.contains(sub, Qt::CaseInsensitive)) {
                label = QStringLiteral("其他");
            }
        }
    }
    item->setText(0, label);
    if (kind == TreeItemKind::Directory || kind == TreeItemKind::Root) {
        item->setText(1, formatTreeMetricSize(directFileBytes));
    } else {
        item->setText(1, count > 0 ? QString::number(count) : QString());
    }
    item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
    item->setSizeHint(1, QSize(72, 28));
    item->setForeground(0, QColor(QStringLiteral("#374151")));
    item->setForeground(1, QColor(QStringLiteral("#9ca3af")));
    applyTreeItemTypography(item);
}

QTreeWidgetItem *findPathChildInsensitive(QTreeWidgetItem *parent, const QString &name)
{
    for (int i = 0; i < parent->childCount(); ++i) {
        QTreeWidgetItem *ch = parent->child(i);
        if (baseTreeLabel(ch).compare(name, Qt::CaseInsensitive) == 0) {
            return ch;
        }
    }
    return nullptr;
}

void collectFileIdsRecursive(QTreeWidgetItem *item, QList<quint64> *out)
{
    const quint64 id = item->data(0, Qt::UserRole).toULongLong();
    if (id != 0) {
        out->append(id);
    }
    for (int i = 0; i < item->childCount(); ++i) {
        collectFileIdsRecursive(item->child(i), out);
    }
}

class TreeMetricDelegate : public QStyledItemDelegate
{
public:
    explicit TreeMetricDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter *painter,
               const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (index.column() != 0) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        painter->save();
        if (option.state & QStyle::State_Selected) {
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#e6f7ff")));
            painter->drawRoundedRect(option.rect.adjusted(0, 1, -2, -1), 10, 10);
        } else if (option.state & QStyle::State_MouseOver) {
            painter->setRenderHint(QPainter::Antialiasing, true);
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(QStringLiteral("#f3f4f6")));
            painter->drawRoundedRect(option.rect.adjusted(0, 1, -2, -1), 10, 10);
        }
        painter->restore();

        QStyleOptionViewItem textOption(option);
        initStyleOption(&textOption, index);
        textOption.backgroundBrush = Qt::NoBrush;
        const QString metric = index.sibling(index.row(), 1).data(Qt::DisplayRole).toString();
        const int metricPadding = metric.isEmpty() ? 0 : 10;
        const int metricWidth = metric.isEmpty() ? 0 : qMin(84, option.fontMetrics.horizontalAdvance(metric) + 8);
        textOption.rect.adjust(0, 0, -(metricWidth + metricPadding), 0);
        QStyledItemDelegate::paint(painter, textOption, index);

        if (metric.isEmpty()) {
            return;
        }

        painter->save();
        QRect metricRect = option.rect.adjusted(option.rect.width() - metricWidth - 8, 0, -8, 0);
        QColor metricColor = QColor(QStringLiteral("#9ca3af"));
        if (option.state & QStyle::State_Selected) {
            metricColor = QColor(QStringLiteral("#22c55e"));
        }
        painter->setPen(metricColor);
        painter->drawText(metricRect, Qt::AlignRight | Qt::AlignVCenter, metric);
        painter->restore();
    }
};

} // namespace

void RecoveryScanPage::setResultStore(ScanResultStore *resultStore)
{
    resultStore_ = resultStore;
    if (scanModel_) {
        scanModel_->setResultStore(resultStore);
    }
}

RecoveryScanPage::~RecoveryScanPage()
{
    if (recoveryCancel_) {
        recoveryCancel_->store(true);
    }
    if (recoveryThread_ && recoveryThread_->isRunning()) {
        recoveryThread_->wait();
    }
}

RecoveryScanPage::RecoveryScanPage(QWidget *parent)
    : QWidget(parent)
{
    qRegisterMetaType<QPixmap>("QPixmap");
    progressUiThrottleTimer_.start();
    scanBatchLogTimer_.start();
    uiPumpTimer_ = new QTimer(this);
    uiPumpTimer_->setInterval(80);
    uiPumpTimer_->setSingleShot(false);
    connect(uiPumpTimer_, &QTimer::timeout, this, &RecoveryScanPage::processUiPump);
    setObjectName(QStringLiteral("recoveryScanRoot"));
    setAutoFillBackground(true);
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(QStringLiteral(
        "#recoveryScanRoot { background-color: #ffffff; }"
        "QTreeWidget, QTableWidget, QTableView {"
        "  border: none;"
        "  background: #ffffff;"
        "  alternate-background-color: #f8fafc;"
        "  gridline-color: #eef2f7;"
        "  outline: none;"
        "}"
        "QTreeWidget::item { height: 28px; padding: 4px 2px 4px 0px; color: #374151; border-radius: 10px; }"
        "QTreeWidget::item:hover { background: #f3f4f6; }"
        "QTreeWidget::item:selected { background: #e6f7ff; color: #22c55e; border-radius: 10px; font-weight: 800; }"
        "QTreeWidget::item:selected:active { background: #e6f7ff; color: #22c55e; }"
        "QTreeWidget::item:selected:!active { background: #e6f7ff; color: #22c55e; }"
        "QTreeWidget::indicator { width: 16px; height: 16px; border: 1px solid #e5e7eb; border-radius: 4px; background: #ffffff; margin-top: 0px; margin-bottom: 0px; margin-left: 2px; margin-right: 6px; }"
        "QTreeWidget::indicator:unchecked:hover { border-color: #16a34a; background: #ffffff; }"
        "QTreeWidget::item:selected QTreeWidget::indicator:unchecked { border: 1px solid #22c55e; background: #f0fdf4; }"
        "QTreeWidget::item:selected:active QTreeWidget::indicator:unchecked { border: 1px solid #22c55e; background: #f0fdf4; }"
        "QTreeWidget::item:selected:!active QTreeWidget::indicator:unchecked { border: 1px solid #22c55e; background: #f0fdf4; }"
        "QTreeWidget::indicator:checked { border: 1px solid #16a34a; border-radius: 4px; background: #16a34a;"
        " image: url(:/qt-project.org/styles/commonstyle/images/checkbox_checked.png); }"
        "QTreeWidget::indicator:indeterminate { border: 1px solid #16a34a; border-radius: 4px; background: #eefbf3;"
        " image: url(:/qt-project.org/styles/commonstyle/images/checkbox_indeterminate.png); }"
        "QTreeView::branch { margin-left: 2px; margin-right: 2px; }"
        "QTableWidget::item:selected { background: #eefbf3; color: #111827; }"
        "QTableWidget::indicator { width: 16px; height: 16px; border: 1px solid #e5e7eb; border-radius: 4px; background: #ffffff; margin-left: 0px; margin-right: 0px; }"
        "QTableWidget::indicator:unchecked:hover { border-color: #16a34a; background: #ffffff; }"
        "QTableWidget::indicator:checked { border: 1px solid #16a34a; border-radius: 4px; background: #16a34a;"
        " image: url(:/qt-project.org/styles/commonstyle/images/checkbox_checked.png); }"
        "QTableWidget::indicator:indeterminate { border: 1px solid #16a34a; border-radius: 4px; background: #eefbf3;"
        " image: url(:/qt-project.org/styles/commonstyle/images/checkbox_indeterminate.png); }"
        "QHeaderView::section {"
        "  background: #f8fafc;"
        "  color: #6b7280;"
        "  border: none;"
        "  border-bottom: 1px solid #e5e7eb;"
        "  padding: 11px 14px;"
        "  font-size: 12px;"
        "  font-weight: 700;"
        "}"
        "QScrollBar:vertical { width: 10px; background: transparent; margin: 4px 0 4px 0; }"
        "QScrollBar::handle:vertical { background: #d1d5db; border-radius: 5px; min-height: 28px; }"
        "QScrollBar::handle:vertical:hover { background: #9ca3af; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical,"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { height: 0; background: transparent; }"
        "QTabBar::tab {"
        "  padding: 12px 2px 11px 2px;"
        "  margin-right: 22px;"
        "  color: #9ca3af;"
        "  font-size: 13px;"
        "  border-bottom: 2px solid transparent;"
        "}"
        "QTabBar::tab:selected {"
        "  color: #16a34a;"
        "  font-weight: 800;"
        "  border-bottom-color: #16a34a;"
        "}"
        "QTabWidget::pane { border: none; top: -1px; }"
    ));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *headerWrap = new QWidget(this);
    headerWrap->setStyleSheet(QStringLiteral("background: #ffffff; border-bottom: 1px solid #e5e7eb;"));
    auto *headerLayout = new QVBoxLayout(headerWrap);
    headerLayout->setContentsMargins(26, 14, 26, 0);
    headerLayout->setSpacing(0);

    auto *topBar = new QHBoxLayout();
    topBar->setContentsMargins(0, 0, 0, 0);
    topBar->setSpacing(12);
    backBtn_ = new QPushButton(QStringLiteral("<"), headerWrap);
    backBtn_->setFixedSize(36, 36);
    backBtn_->setStyleSheet(QStringLiteral(
        "QPushButton { border: 1px solid #d1d5db; border-radius: 10px; background: #ffffff; color: #374151; font-size: 16px; font-weight: 700; padding: 0; }"
        "QPushButton:hover { background: #f9fafb; }"));
    backBtn_->hide();
    topBar->addWidget(backBtn_);

    pathLabel_ = new QLabel(QStringLiteral("扫描 本地磁盘(C:)"), headerWrap);
    pathLabel_->setStyleSheet(QStringLiteral("font-size: 18px; font-weight: 800; color: #111827;"));
    topBar->addWidget(pathLabel_, 1);

    viewToggleBtn_ = new QPushButton(QStringLiteral("预览"), headerWrap);
    viewToggleBtn_->setFixedHeight(36);
    viewToggleBtn_->setMinimumWidth(62);
    viewToggleBtn_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #ffffff; color: #374151; border: 1px solid #d1d5db; border-radius: 10px; font-size: 13px; font-weight: 600; padding: 0 14px; }"
        "QPushButton:hover { background: #f9fafb; }"));
    viewToggleBtn_->hide();
    topBar->addWidget(viewToggleBtn_);

    searchEdit_ = new QLineEdit(headerWrap);
    searchEdit_->setPlaceholderText(QStringLiteral("搜索文件或文件夹"));
    searchEdit_->setFixedHeight(36);
    searchEdit_->setMinimumWidth(220);
    searchEdit_->setStyleSheet(QStringLiteral(
        "QLineEdit { border: 1px solid #d1d5db; border-radius: 10px; padding: 0 12px; background: #ffffff; color: #111827; }"
        "QLineEdit:focus { border-color: #86efac; }"));
    searchEdit_->hide();
    topBar->addWidget(searchEdit_);

    searchBtn_ = new QPushButton(QStringLiteral("搜索"), headerWrap);
    searchBtn_->setFixedHeight(36);
    searchBtn_->setMinimumWidth(56);
    searchBtn_->setStyleSheet(QStringLiteral(
        "QPushButton { border: 1px solid #d1d5db; border-radius: 10px; background: #ffffff; color: #374151; font-size: 14px; font-weight: 700; padding: 0; }"
        "QPushButton:hover { background: #f9fafb; }"));
    searchBtn_->hide();
    topBar->addWidget(searchBtn_);

    sortCombo_ = new QComboBox(headerWrap);
    sortCombo_->addItems(QStringList()
                         << QStringLiteral("默认顺序")
                         << QStringLiteral("按文件名")
                         << QStringLiteral("按大小")
                         << QStringLiteral("按修改时间")
                         << QStringLiteral("按质量"));
    sortCombo_->setFixedHeight(36);
    sortCombo_->setMinimumWidth(112);
    sortCombo_->setStyleSheet(QStringLiteral(
        "QComboBox { border: 1px solid #d1d5db; border-radius: 10px; padding: 0 10px;"
        " background: #ffffff; color: #374151; font-size: 12px; }"
        "QComboBox:hover { border-color: #9ca3af; }"
        "QComboBox::drop-down { border: none; width: 22px; }"));
    sortCombo_->hide();
    topBar->addWidget(sortCombo_);

    stopScanBtn_ = new QPushButton(QStringLiteral("停止"), headerWrap);
    stopScanBtn_->setFixedHeight(36);
    stopScanBtn_->setMinimumWidth(88);
    stopScanBtn_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #ffffff; color: #b91c1c; border: 1px solid rgba(239,68,68,0.55);"
        " border-radius: 10px; font-size: 14px; font-weight: 600; padding: 0 15px; }"
        "QPushButton:hover { background: rgba(239,68,68,0.06); }"
        "QPushButton:disabled { color: #9ca3af; border-color: #e5e7eb; background: #f9fafb; }"));
    stopScanBtn_->setEnabled(false);
    topBar->addWidget(stopScanBtn_);
    headerLayout->addLayout(topBar);

    statusLabel_ = new QLabel(QStringLiteral("扫描进行中"), headerWrap);
    statusLabel_->setStyleSheet(QStringLiteral(
        "font-size: 12px; color: #6b7280; padding: 7px 0 10px 0;"));
    headerLayout->addWidget(statusLabel_);

    resultsMetaLabel_ = new QLabel(QStringLiteral("已选：0"), headerWrap);
    resultsMetaLabel_->setStyleSheet(QStringLiteral(
        "font-size: 13px; color: #6b7280; padding: 8px 0 10px 0;"));
    resultsMetaLabel_->hide();
    headerLayout->addWidget(resultsMetaLabel_);

    progressBar_ = new QProgressBar(headerWrap);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    progressBar_->setTextVisible(false);
    progressBar_->setFixedHeight(10);
    progressBar_->setStyleSheet(QStringLiteral(
        "QProgressBar { border: none; background: #e7edf3; border-radius: 5px; padding: 0px; margin: 2px 0 0 0; }"
        "QProgressBar::chunk { background: #22c55e; border: none; border-radius: 5px; margin: 0px; }"));
    headerLayout->addWidget(progressBar_);

    scanMetricWrap_ = new QWidget(headerWrap);
    scanMetricWrap_->setStyleSheet(QStringLiteral(
        "background: #ffffff; border-bottom: 1px solid #e5e7eb;"));
    auto *metricRow = new QHBoxLayout(scanMetricWrap_);
    metricRow->setContentsMargins(0, 11, 0, 11);
    metricRow->setSpacing(10);
    auto makeMetric = [headerWrap](const QString &value, const QString &caption, QLabel **outLabel) {
        auto *lb = new QLabel(headerWrap);
        lb->setTextFormat(Qt::RichText);
        lb->setStyleSheet(QStringLiteral("font-size: 12px;"));
        setMetricLabelText(lb, value, caption);
        if (outLabel) {
            *outLabel = lb;
        }
        return lb;
    };
    metricRow->addWidget(makeMetric(QStringLiteral("0"), QStringLiteral("已找到"), &metricFoundLabel_));
    metricRow->addWidget(new QLabel(QStringLiteral("|"), scanMetricWrap_));
    metricRow->addWidget(makeMetric(QStringLiteral("0 GB"), QStringLiteral("已扫描"), &metricScannedLabel_));
    metricRow->addWidget(new QLabel(QStringLiteral("|"), scanMetricWrap_));
    metricRow->addWidget(makeMetric(QStringLiteral("500 GB"), QStringLiteral("总计"), &metricTotalLabel_));
    metricRow->addWidget(new QLabel(QStringLiteral("|"), scanMetricWrap_));
    metricRow->addWidget(makeMetric(QStringLiteral("00:00"), QStringLiteral("用时"), &metricElapsedLabel_));
    metricRow->addWidget(new QLabel(QStringLiteral("|"), scanMetricWrap_));
    metricRow->addWidget(makeMetric(QStringLiteral("0 MB/s"), QStringLiteral("速度"), &metricSpeedLabel_));
    metricRow->addStretch(1);
    headerLayout->addWidget(scanMetricWrap_);
    root->addWidget(headerWrap);

    // 扫描状态页与结果页使用两套明确的工作区。扫描时只保留一个核心状态卡，
    // 避免把结果表格、筛选树和进度信息同时挤在首屏，结构与 HTML 原型一致。
    scanStatusCard_ = new QFrame(this);
    scanStatusCard_->setObjectName(QStringLiteral("scanStatusCard"));
    scanStatusCard_->setStyleSheet(QStringLiteral(
        "#scanStatusCard { background: #ffffff; border: 1px solid #e5e7eb; border-radius: 18px; }"
        "#scanStatusCard QLabel[role='eyebrow'] { color: #2563eb; font-size: 12px; font-weight: 800; }"
        "#scanStatusCard QLabel[role='title'] { color: #111827; font-size: 20px; font-weight: 800; }"
        "#scanStatusCard QLabel[role='lead'] { color: #6b7280; font-size: 13px; }"
        "#scanStatusCard QLabel[role='meta'] { color: #6b7280; font-size: 12px; }"
        "#scanStatusCard QLabel[role='eta'] { color: #2563eb; font-size: 12px; font-weight: 700; }"
        "#scanStatusCard QFrame[role='metric'] { background: #f8fafc; border: 1px solid #e5e7eb; border-radius: 10px; }"
        "#scanStatusCard QLabel[role='metricValue'] { color: #111827; font-size: 20px; font-weight: 800; }"
        "#scanStatusCard QLabel[role='metricCaption'] { color: #6b7280; font-size: 11px; }"));
    scanStatusCard_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    auto *scanCardLayout = new QVBoxLayout(scanStatusCard_);
    scanCardLayout->setContentsMargins(48, 38, 48, 34);
    scanCardLayout->setSpacing(0);

    auto *scanEyebrow = new QLabel(QStringLiteral("RECOVERY IN PROGRESS"), scanStatusCard_);
    scanEyebrow->setProperty("role", QStringLiteral("eyebrow"));
    scanEyebrow->setAlignment(Qt::AlignCenter);
    scanCardLayout->addWidget(scanEyebrow);

    scanPercentLabel_ = new QLabel(QStringLiteral("0%"), scanStatusCard_);
    scanPercentLabel_->setFixedSize(92, 92);
    scanPercentLabel_->setAlignment(Qt::AlignCenter);
    scanPercentLabel_->setStyleSheet(QStringLiteral(
        "QLabel { border: 7px solid #dbeafe; border-top-color: #2563eb; border-radius: 46px;"
        " color: #2563eb; font-size: 19px; font-weight: 800; background: #ffffff; }"));
    auto *scanCircleRow = new QHBoxLayout();
    scanCircleRow->setContentsMargins(0, 20, 0, 18);
    scanCircleRow->addStretch(1);
    scanCircleRow->addWidget(scanPercentLabel_);
    scanCircleRow->addStretch(1);
    scanCardLayout->addLayout(scanCircleRow);

    scanPhaseLabel_ = new QLabel(QStringLiteral("正在进行深度扫描"), scanStatusCard_);
    scanPhaseLabel_->setProperty("role", QStringLiteral("title"));
    scanPhaseLabel_->setAlignment(Qt::AlignCenter);
    scanCardLayout->addWidget(scanPhaseLabel_);

    scanDescriptionLabel_ = new QLabel(QStringLiteral(
        "自动模式正在按顺序检查文件系统、删除记录和文件结构。"), scanStatusCard_);
    scanDescriptionLabel_->setProperty("role", QStringLiteral("lead"));
    scanDescriptionLabel_->setAlignment(Qt::AlignCenter);
    scanDescriptionLabel_->setWordWrap(true);
    scanCardLayout->addWidget(scanDescriptionLabel_);

    scanCardProgressBar_ = new QProgressBar(scanStatusCard_);
    scanCardProgressBar_->setRange(0, 100);
    scanCardProgressBar_->setValue(0);
    scanCardProgressBar_->setTextVisible(false);
    scanCardProgressBar_->setFixedHeight(10);
    scanCardProgressBar_->setStyleSheet(QStringLiteral(
        "QProgressBar { border: none; background: #e8eef8; border-radius: 5px; padding: 0; }"
        "QProgressBar::chunk { background: #2563eb; border-radius: 5px; }"));
    scanCardLayout->addSpacing(28);
    scanCardLayout->addWidget(scanCardProgressBar_);

    auto *scanMetaRow = new QHBoxLayout();
    scanMetaRow->setContentsMargins(0, 10, 0, 0);
    scanProgressTextLabel_ = new QLabel(QStringLiteral("已扫描 0 B / —"), scanStatusCard_);
    scanProgressTextLabel_->setProperty("role", QStringLiteral("meta"));
    scanEtaLabel_ = new QLabel(QStringLiteral("预计还需 —"), scanStatusCard_);
    scanEtaLabel_->setProperty("role", QStringLiteral("eta"));
    scanMetaRow->addWidget(scanProgressTextLabel_);
    scanMetaRow->addStretch(1);
    scanMetaRow->addWidget(scanEtaLabel_);
    scanCardLayout->addLayout(scanMetaRow);

    auto makeScanMetric = [this](const QString &value, const QString &caption, QLabel **valueOut) {
        auto *frame = new QFrame(scanStatusCard_);
        frame->setProperty("role", QStringLiteral("metric"));
        auto *layout = new QVBoxLayout(frame);
        layout->setContentsMargins(14, 12, 14, 11);
        layout->setSpacing(3);
        auto *valueLabel = new QLabel(value, frame);
        valueLabel->setProperty("role", QStringLiteral("metricValue"));
        auto *captionLabel = new QLabel(caption, frame);
        captionLabel->setProperty("role", QStringLiteral("metricCaption"));
        layout->addWidget(valueLabel);
        layout->addWidget(captionLabel);
        if (valueOut) {
            *valueOut = valueLabel;
        }
        return frame;
    };
    auto *scanMetrics = new QHBoxLayout();
    scanMetrics->setContentsMargins(0, 26, 0, 0);
    scanMetrics->setSpacing(12);
    scanMetrics->addWidget(makeScanMetric(QStringLiteral("0"), QStringLiteral("已发现文件"), &scanFoundLabel_));
    scanMetrics->addWidget(makeScanMetric(QStringLiteral("0 B"), QStringLiteral("已扫描空间"), &scanScannedLabel_));
    scanMetrics->addWidget(makeScanMetric(QStringLiteral("0 B/s"), QStringLiteral("当前速度"), &scanSpeedLabel_));
    scanCardLayout->addLayout(scanMetrics);

    auto *scanActions = new QHBoxLayout();
    scanActions->setContentsMargins(0, 24, 0, 0);
    scanActions->setSpacing(12);
    scanActions->addStretch(1);
    scanPreviewResultsBtn_ = new QPushButton(QStringLiteral("查看已发现文件"), scanStatusCard_);
    scanPreviewResultsBtn_->setFixedHeight(38);
    scanPreviewResultsBtn_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #ffffff; color: #374151; border: 1px solid #d1d5db; border-radius: 9px;"
        " padding: 0 16px; font-size: 13px; font-weight: 600; }"
        "QPushButton:hover { background: #f9fafb; border-color: #9ca3af; }"));
    scanActions->addWidget(scanPreviewResultsBtn_);
    auto *scanHelper = new QLabel(QStringLiteral("扫描仍会在后台继续"), scanStatusCard_);
    scanHelper->setProperty("role", QStringLiteral("meta"));
    scanActions->addWidget(scanHelper);
    scanActions->addStretch(1);
    scanCardLayout->addLayout(scanActions);
    scanStatusCard_->hide();
    root->addWidget(scanStatusCard_, 1);

    resultsSummaryWrap_ = new QFrame(this);
    resultsSummaryWrap_->setObjectName(QStringLiteral("resultsSummaryWrap"));
    resultsSummaryWrap_->setStyleSheet(QStringLiteral(
        "#resultsSummaryWrap { background: #ffffff; border-bottom: 1px solid #e5e7eb; }"
        "#resultsSummaryWrap QFrame[role='summary'] { background: #f8fafc; border: 1px solid #e5e7eb; border-radius: 10px; }"
        "#resultsSummaryWrap QLabel[role='value'] { color: #111827; font-size: 17px; font-weight: 800; }"
        "#resultsSummaryWrap QLabel[role='caption'] { color: #6b7280; font-size: 11px; }"
        "#resultsSummaryWrap QLabel[role='hint'] { color: #6b7280; font-size: 12px; }"));
    auto *summaryLayout = new QHBoxLayout(resultsSummaryWrap_);
    summaryLayout->setContentsMargins(26, 12, 26, 12);
    summaryLayout->setSpacing(10);
    auto makeSummary = [this](const QString &value, const QString &caption, QLabel **valueOut) {
        auto *frame = new QFrame(resultsSummaryWrap_);
        frame->setProperty("role", QStringLiteral("summary"));
        auto *layout = new QVBoxLayout(frame);
        layout->setContentsMargins(13, 8, 18, 8);
        layout->setSpacing(1);
        auto *valueLabel = new QLabel(value, frame);
        valueLabel->setProperty("role", QStringLiteral("value"));
        auto *captionLabel = new QLabel(caption, frame);
        captionLabel->setProperty("role", QStringLiteral("caption"));
        layout->addWidget(valueLabel);
        layout->addWidget(captionLabel);
        if (valueOut) *valueOut = valueLabel;
        return frame;
    };
    summaryLayout->addWidget(makeSummary(QStringLiteral("0"), QStringLiteral("文件"), &resultsFilesSummaryLabel_));
    summaryLayout->addWidget(makeSummary(QStringLiteral("0 B"), QStringLiteral("可恢复数据"), &resultsBytesSummaryLabel_));
    summaryLayout->addWidget(makeSummary(QStringLiteral("0"), QStringLiteral("已选择"), &resultsSelectedSummaryLabel_));
    auto *summaryHint = new QLabel(QStringLiteral("先预览，再选择需要恢复的文件"), resultsSummaryWrap_);
    summaryHint->setProperty("role", QStringLiteral("hint"));
    summaryLayout->addSpacing(4);
    summaryLayout->addWidget(summaryHint);
    summaryLayout->addStretch(1);
    resultsSummaryWrap_->hide();
    root->addWidget(resultsSummaryWrap_);

    auto *contentWrap = new QWidget(this);
    resultsContentWrap_ = contentWrap;
    auto *contentRoot = new QVBoxLayout(contentWrap);
    contentRoot->setContentsMargins(26, 0, 26, 0);
    contentRoot->setSpacing(0);

    leftTabs_ = new QTabWidget(contentWrap);
    leftTabs_->setDocumentMode(true);
    leftTabs_->setStyleSheet(QStringLiteral(
        "QTabWidget::pane { border: none; margin-top: 0px; }"
        "QTabBar::tab { min-width: 0; padding-bottom: 8px; }"));
    auto *pathTab = new QWidget(contentWrap);
    auto *typeTab = new QWidget(contentWrap);
    leftTabs_->addTab(pathTab, QStringLiteral("文件位置"));
    leftTabs_->addTab(typeTab, QStringLiteral("文件类型"));
    contentRoot->addWidget(leftTabs_);

    auto *contentBody = new QWidget(contentWrap);
    auto *contentLayout = new QHBoxLayout(contentBody);
    contentLayout->setContentsMargins(0, 2, 0, 5);
    contentLayout->setSpacing(5);

    auto *leftPanel = new QFrame(contentBody);
    leftPanel->setObjectName(QStringLiteral("scanLeftPanel"));
    leftPanel->setStyleSheet(QStringLiteral(
        "#scanLeftPanel { background: #ffffff; border: 1px solid #e5e7eb; border-radius: 12px; }"));
    auto *leftPanelLayout = new QVBoxLayout(leftPanel);
    leftPanelLayout->setContentsMargins(8, 4, 8, 8);
    leftPanelLayout->setSpacing(0);
    auto *filterTitle = new QLabel(QStringLiteral("筛选结果"), leftPanel);
    filterTitle->setStyleSheet(QStringLiteral(
        "font-size: 14px; font-weight: 800; color: #111827; padding: 6px 8px 9px 8px;"));
    leftPanelLayout->addWidget(filterTitle);

    pathTree_ = new QTreeWidget();
    pathTree_->setHeaderHidden(true);
    pathTree_->setColumnCount(2);
    pathTree_->setIndentation(15);
    pathTree_->setRootIsDecorated(false);
    pathTree_->setUniformRowHeights(true);
    pathTree_->setAnimated(true);
    pathTree_->setItemDelegate(new TreeMetricDelegate(pathTree_));
    pathTree_->header()->setStretchLastSection(false);
    pathTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    pathTree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    pathTree_->setColumnWidth(1, 0);
    pathTree_->setColumnHidden(1, true);
    leftPanelLayout->addWidget(pathTree_);

    typeTree_ = new QTreeWidget();
    typeTree_->setHeaderHidden(true);
    typeTree_->setColumnCount(2);
    typeTree_->setIndentation(15);
    typeTree_->setRootIsDecorated(true);
    typeTree_->setUniformRowHeights(true);
    typeTree_->setAnimated(true);
    typeTree_->setItemDelegate(new TreeMetricDelegate(typeTree_));
    typeTree_->header()->setStretchLastSection(false);
    typeTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    typeTree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    typeTree_->setColumnWidth(1, 0);
    typeTree_->setColumnHidden(1, true);
    leftPanelLayout->addWidget(typeTree_);
    typeTree_->hide();
    contentLayout->addWidget(leftPanel, 0);

    scanModel_ = new ScanResultModel(this);
    filterModel_ = new ScanResultFilterModel(this);
    filterModel_->setSourceModel(scanModel_);

    tableView_ = new QTableView();
    tableView_->setModel(filterModel_);
    tableView_->horizontalHeader()->setStretchLastSection(false);
    tableView_->horizontalHeader()->setSectionResizeMode(ScanResultModel::ColCheckBox, QHeaderView::Fixed);
    tableView_->horizontalHeader()->resizeSection(ScanResultModel::ColCheckBox, 52);
    tableView_->horizontalHeader()->setSectionResizeMode(ScanResultModel::ColName, QHeaderView::Stretch);
    tableView_->horizontalHeader()->setSectionResizeMode(ScanResultModel::ColSize, QHeaderView::Fixed);
    tableView_->horizontalHeader()->resizeSection(ScanResultModel::ColSize, 90);
    tableView_->horizontalHeader()->setSectionResizeMode(ScanResultModel::ColType, QHeaderView::Fixed);
    tableView_->horizontalHeader()->resizeSection(ScanResultModel::ColType, 78);
    tableView_->horizontalHeader()->setSectionResizeMode(ScanResultModel::ColModified, QHeaderView::Fixed);
    tableView_->horizontalHeader()->resizeSection(ScanResultModel::ColModified, 108);
    tableView_->horizontalHeader()->setSectionResizeMode(ScanResultModel::ColRecoverability, QHeaderView::Fixed);
    tableView_->horizontalHeader()->resizeSection(ScanResultModel::ColRecoverability, 72);
    tableView_->horizontalHeader()->setSectionResizeMode(ScanResultModel::ColPath, QHeaderView::Fixed);
    tableView_->horizontalHeader()->resizeSection(ScanResultModel::ColPath, 160);
    tableView_->horizontalHeader()->setSectionResizeMode(ScanResultModel::ColPhase, QHeaderView::Fixed);
    tableView_->horizontalHeader()->resizeSection(ScanResultModel::ColPhase, 80);
    tableView_->setSelectionBehavior(QAbstractItemView::SelectRows);
    tableView_->setSelectionMode(QAbstractItemView::SingleSelection);
    tableView_->setAlternatingRowColors(false);
    tableView_->verticalHeader()->setVisible(false);
    tableView_->setShowGrid(false);
    tableView_->setStyleSheet(QStringLiteral(
        "QTableView { background: transparent; }"
        "QTableView::item { padding: 6px 10px; border-bottom: 1px solid #f1f5f9; }"
        "QTableView::item:selected { background: #eefbf3; color: #111827; }"
        "QTableView::indicator { width: 16px; height: 16px; border: 1px solid #e5e7eb; border-radius: 4px; background: #ffffff; margin-left: 0px; margin-right: 0px; }"
        "QTableView::indicator:unchecked:hover { border-color: #16a34a; background: #ffffff; }"
        "QTableView::indicator:checked { border: 1px solid #16a34a; border-radius: 4px; background: #16a34a;"
        " image: url(:/qt-project.org/styles/commonstyle/images/checkbox_checked.png); }"
        "QTableView::indicator:indeterminate { border: 1px solid #16a34a; border-radius: 4px; background: #eefbf3;"
        " image: url(:/qt-project.org/styles/commonstyle/images/checkbox_indeterminate.png); }"));
    tableView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tableView_->setWordWrap(false);
    tableView_->setSortingEnabled(true);
    tableView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    tableView_->verticalHeader()->setDefaultSectionSize(36);
    connect(tableView_->selectionModel(), &QItemSelectionModel::currentChanged,
            this, [this](const QModelIndex &current, const QModelIndex &) {
                if (!current.isValid() || !filterModel_ || !scanModel_) {
                    return;
                }
                const QModelIndex sourceIndex = filterModel_->mapToSource(current);
                updateDetailFromTableRow(sourceIndex.row());
            });

    table_ = nullptr;

    headerSelectAllCheckBox_ = new QCheckBox(tableView_->horizontalHeader());
    headerSelectAllCheckBox_->setTristate(false);
    headerSelectAllCheckBox_->setStyleSheet(QStringLiteral(
        "QCheckBox { background: transparent; spacing: 0px; padding: 0px; margin: 0px; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #e5e7eb; border-radius: 4px; background: #ffffff; }"
        "QCheckBox::indicator:hover { border-color: #16a34a; background: #ffffff; }"
        "QCheckBox::indicator:checked { border: 1px solid #16a34a; border-radius: 4px; background: #16a34a;"
        " image: url(:/qt-project.org/styles/commonstyle/images/checkbox_checked.png); }"
        "QCheckBox::indicator:indeterminate { border: 1px solid #16a34a; border-radius: 4px; background: #eefbf3;"
        " image: url(:/qt-project.org/styles/commonstyle/images/checkbox_indeterminate.png); }"));
    headerSelectAllCheckBox_->setGeometry(10, 10, 20, 20);
    connect(tableView_->horizontalHeader(), &QHeaderView::sectionResized, this, [this](int logicalIndex, int, int) {
        if (!headerSelectAllCheckBox_ || logicalIndex != 0) {
            return;
        }
        const int x = tableView_->horizontalHeader()->sectionViewportPosition(0);
        const int w = tableView_->horizontalHeader()->sectionSize(0);
        const int h = tableView_->horizontalHeader()->height();
        headerSelectAllCheckBox_->move(x + (w - 20) / 2, (h - 20) / 2);
    });
    connect(tableView_->horizontalHeader(), &QHeaderView::geometriesChanged, this, [this]() {
        if (!headerSelectAllCheckBox_) {
            return;
        }
        const int x = tableView_->horizontalHeader()->sectionViewportPosition(0);
        const int w = tableView_->horizontalHeader()->sectionSize(0);
        const int h = tableView_->horizontalHeader()->height();
        headerSelectAllCheckBox_->move(x + (w - 20) / 2, (h - 20) / 2);
    });
    connect(headerSelectAllCheckBox_, &QCheckBox::stateChanged, this, [this](int state) {
        if (!scanModel_ || syncingHeaderCheck_) {
            return;
        }
        syncingTableChecks_ = true;
        scanModel_->setAllChecked(state == Qt::Checked);
        syncingTableChecks_ = false;
    });
    auto *tableFrame = new QFrame(contentBody);
    tableFrame->setObjectName(QStringLiteral("scanTablePanel"));
    tableFrame->setStyleSheet(QStringLiteral(
        "#scanTablePanel { background: #ffffff; border: 1px solid #e5e7eb; border-radius: 12px; }"));
    auto *tableLayout = new QVBoxLayout(tableFrame);
    tableLayout->setContentsMargins(1, 1, 1, 1);
    tableLayout->addWidget(tableView_);

    tableView_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tableView_, &QTableView::customContextMenuRequested, this, [this](const QPoint& pos) {
        if (!resultsMode_ || !filterModel_) {
            return;
        }
        const QModelIndex idx = tableView_->indexAt(pos);
        QMenu menu(this);
        QAction* repairAct = menu.addAction(QStringLiteral("尝试修复…"));
        repairAct->setEnabled(resultsMode_ && !diskLetter_.isEmpty());
        QAction* chosen = menu.exec(tableView_->viewport()->mapToGlobal(pos));
        if (chosen == repairAct) {
            if (idx.isValid()) {
                const int srcRow = filterModel_->mapToSource(idx).row();
                if (scanModel_) {
                    tableView_->selectRow(idx.row());
                    if (const ScanFileDto* dto = scanModel_->dtoAtRow(srcRow)) {
                        if (!dto->isDirectory && isRepairableDto(*dto)) {
                            scanModel_->setChecked(srcRow, true);
                        }
                    }
                }
            }
            onRepairRequested();
        }
    });
    contentLayout->addWidget(tableFrame, 1);

    previewFrame_ = new QFrame(contentBody);
    previewFrame_->setObjectName(QStringLiteral("scanPreviewPanel"));
    previewFrame_->setStyleSheet(QStringLiteral(
        "#scanPreviewPanel { background: #ffffff; border: 1px solid #e5e7eb; border-radius: 12px; }"
        "#scanPreviewPanel QListWidget { background: transparent; border: none; padding: 10px; }"
        "#scanPreviewPanel QListWidget::item { border: none; background: transparent; }"));
    auto *previewLayout = new QVBoxLayout(previewFrame_);
    previewLayout->setContentsMargins(14, 14, 14, 14);
    previewLayout->setSpacing(10);
    previewDetailFrame_ = new QFrame(previewFrame_);
    previewDetailFrame_->setObjectName(QStringLiteral("previewDetailFrame"));
    previewDetailFrame_->setStyleSheet(QStringLiteral(
        "#previewDetailFrame { background: #f8fafc; border: 1px solid #eef2f7; border-radius: 12px; }"
        "#previewDetailFrame QLabel[role='title'] { color: #111827; font-size: 14px; font-weight: 800; }"
        "#previewDetailFrame QLabel[role='meta'] { color: #6b7280; font-size: 11px; }"
        "#previewDetailFrame QLabel[role='content'] { color: #6b7280; font-size: 13px; background: transparent; }"));
    auto *detailLayout = new QVBoxLayout(previewDetailFrame_);
    detailLayout->setContentsMargins(14, 12, 14, 14);
    detailLayout->setSpacing(6);
    previewTitleLabel_ = new QLabel(QStringLiteral("文件预览"), previewDetailFrame_);
    previewTitleLabel_->setProperty("role", QStringLiteral("title"));
    previewMetaLabel_ = new QLabel(QStringLiteral("选择文件后，这里会自动判断预览方式"), previewDetailFrame_);
    previewMetaLabel_->setProperty("role", QStringLiteral("meta"));
    previewMetaLabel_->setWordWrap(true);
    previewContentLabel_ = new QLabel(QStringLiteral("选择文件后，这里会自动判断预览方式。\n图片显示缩略图，视频显示第一帧；无法解析的文件显示前 100 字节摘要。"), previewDetailFrame_);
    previewContentLabel_->setObjectName(QStringLiteral("previewContent"));
    previewContentLabel_->setProperty("role", QStringLiteral("content"));
    previewContentLabel_->setAlignment(Qt::AlignCenter);
    previewContentLabel_->setWordWrap(true);
    previewContentLabel_->setMinimumHeight(220);
    detailLayout->addWidget(previewTitleLabel_);
    detailLayout->addWidget(previewMetaLabel_);
    detailLayout->addWidget(previewContentLabel_, 1);
    previewLayout->addWidget(previewDetailFrame_, 1);
    gridView_ = new QListWidget(previewFrame_);
    gridView_->setViewMode(QListView::IconMode);
    gridView_->setResizeMode(QListView::Adjust);
    gridView_->setMovement(QListView::Static);
    gridView_->setSpacing(12);
    gridView_->setWrapping(true);
    gridView_->setUniformItemSizes(false);
    gridView_->setWordWrap(true);
    gridView_->setSelectionMode(QAbstractItemView::NoSelection);
    gridView_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    gridView_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    gridView_->setIconSize(QSize(168, 126));
    gridView_->hide();
    previewLayout->addWidget(gridView_);
    previewFrame_->hide();
    contentLayout->addWidget(previewFrame_);

    contentLayout->setStretch(0, 24);
    contentLayout->setStretch(1, 52);
    contentLayout->setStretch(2, 24);
    contentRoot->addWidget(contentBody, 1);
    root->addWidget(contentWrap, 1);

    scanFooter_ = new QFrame(this);
    scanFooter_->setStyleSheet(QStringLiteral(
        "QFrame { border-top: 1px solid #e5e7eb; background: #ffffff; }"));
    auto *footerLayout = new QHBoxLayout(scanFooter_);
    footerLayout->setContentsMargins(26, 10, 26, 10);
    footerLayout->setSpacing(10);

    footerSubLabel_ = new QLabel(QStringLiteral("恢复前可继续筛选并勾选需要的文件"), scanFooter_);
    footerSubLabel_->setStyleSheet(QStringLiteral("font-size: 12px; color: #6b7280;"));
    footerSubLabel_->hide();
    footerLayout->addWidget(footerSubLabel_);

    footerLayout->addStretch(1);

    keepDirStructureCheck_ = new QCheckBox(QStringLiteral("保留原目录结构"), scanFooter_);
    {
        QSettings prefs;
        keepDirStructureCheck_->setChecked(
            prefs.value(QStringLiteral("recovery/keepTree"), true).toBool());
    }
    keepDirStructureCheck_->setStyleSheet(QStringLiteral(
        "QCheckBox { color: #374151; font-size: 12px; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #e5e7eb; border-radius: 4px; background: #ffffff; }"
        "QCheckBox::indicator:hover { border-color: #16a34a; background: #ffffff; }"
        "QCheckBox::indicator:checked { border: 1px solid #16a34a; border-radius: 4px; background: #16a34a;"
        " image: url(:/qt-project.org/styles/commonstyle/images/checkbox_checked.png); }"));
    keepDirStructureCheck_->hide();
    connect(keepDirStructureCheck_, &QCheckBox::toggled, this, [](bool checked) {
        QSettings prefs;
        prefs.setValue(QStringLiteral("recovery/keepTree"), checked);
    });
    footerLayout->addWidget(keepDirStructureCheck_);

    formatAwareFragmentCheck_ = new QCheckBox(QStringLiteral("格式感知碎片重组"), scanFooter_);
    formatAwareFragmentCheck_->setToolTip(
        QStringLiteral("FAT 删除且链断裂时，对 JPEG/PNG/PDF/ZIP/MP4 等按文件结构尝试重组簇链。"
                       "无结构文件（如 .dat）无法保证正确；关闭可加快恢复。"));
    formatAwareFragmentCheck_->setStyleSheet(keepDirStructureCheck_->styleSheet());
    {
        QSettings prefs;
        formatAwareFragmentCheck_->setChecked(
            prefs.value(QStringLiteral("recover/formatAwareFragments"), true).toBool());
        connect(formatAwareFragmentCheck_, &QCheckBox::toggled, this, [](bool checked) {
            QSettings p;
            p.setValue(QStringLiteral("recover/formatAwareFragments"), checked);
        });
    }
    formatAwareFragmentCheck_->hide();
    footerLayout->addWidget(formatAwareFragmentCheck_);

    recoverAllBtn_ = new QPushButton(QStringLiteral("恢复全部"), scanFooter_);
    recoverAllBtn_->setFixedHeight(40);
    recoverAllBtn_->setMinimumWidth(118);
    recoverAllBtn_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #16a34a; color: #ffffff; border: none; border-radius: 10px; font-size: 14px; font-weight: 700; padding: 0 20px; }"
        "QPushButton:hover { background: #15803d; }"
        "QPushButton:disabled { background: #bbf7d0; color: #ffffff; }"));
    recoverAllBtn_->setEnabled(false);
    recoverAllBtn_->hide();
    // 放到 footerLayout 里（配合上面的 addStretch），确保显示在右侧而不是默认左上角。
    footerLayout->addWidget(recoverAllBtn_);

    root->addWidget(scanFooter_);

    recoveryCompletePanel_ = new QFrame(this);
    recoveryCompletePanel_->setObjectName(QStringLiteral("recoveryCompletePanel"));
    recoveryCompletePanel_->setStyleSheet(QStringLiteral(
        "#recoveryCompletePanel { background: rgba(255,255,255,0.98); border: 1px solid #e5e7eb; border-radius: 14px; }"
        "#recoveryCompletePanel QLabel[title='1'] { color: #111827; font-size: 20px; font-weight: 800; }"
        "#recoveryCompletePanel QLabel[meta='1'] { color: #6b7280; font-size: 12px; }"
        "#recoveryCompletePanel QTableWidget { background: #ffffff; border: 1px solid #e5e7eb; border-radius: 10px; }"
        "#recoveryCompletePanel QHeaderView::section { background: #f8fafc; color: #6b7280; border: none; border-bottom: 1px solid #e5e7eb; padding: 8px 10px; }"));
    auto *completeLayout = new QVBoxLayout(recoveryCompletePanel_);
    completeLayout->setContentsMargins(20, 18, 20, 18);
    completeLayout->setSpacing(10);
    auto *completeTitle = new QLabel(QStringLiteral("恢复完成"), recoveryCompletePanel_);
    completeTitle->setProperty("title", QStringLiteral("1"));
    completeLayout->addWidget(completeTitle);
    recoveryCompleteSummaryLabel_ = new QLabel(QStringLiteral("成功: 0  失败: 0  跳过: 0"), recoveryCompletePanel_);
    recoveryCompleteSummaryLabel_->setProperty("meta", QStringLiteral("1"));
    completeLayout->addWidget(recoveryCompleteSummaryLabel_);
    recoveryCompletePathLabel_ = new QLabel(QStringLiteral("目标目录：-"), recoveryCompletePanel_);
    recoveryCompletePathLabel_->setWordWrap(true);
    recoveryCompletePathLabel_->setProperty("meta", QStringLiteral("1"));
    completeLayout->addWidget(recoveryCompletePathLabel_);
    recoveryResultFilterCombo_ = new QComboBox(recoveryCompletePanel_);
    recoveryResultFilterCombo_->addItems(QStringList()
                                         << QStringLiteral("全部结果")
                                         << QStringLiteral("成功")
                                         << QStringLiteral("部分恢复")
                                         << QStringLiteral("失败")
                                         << QStringLiteral("跳过"));
    recoveryResultFilterCombo_->setMinimumHeight(32);
    recoveryResultFilterCombo_->setMaximumWidth(180);
    recoveryResultFilterCombo_->setToolTip(QStringLiteral("按恢复结果筛选明细"));
    completeLayout->addWidget(recoveryResultFilterCombo_);
    recoveryCompleteTable_ = new QTableWidget(recoveryCompletePanel_);
    recoveryCompleteTable_->setColumnCount(4);
    recoveryCompleteTable_->setHorizontalHeaderLabels(QStringList()
                                                      << QStringLiteral("状态")
                                                      << QStringLiteral("文件")
                                                      << QStringLiteral("目标路径")
                                                      << QStringLiteral("原因"));
    recoveryCompleteTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    recoveryCompleteTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    recoveryCompleteTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    recoveryCompleteTable_->verticalHeader()->setVisible(false);
    recoveryCompleteTable_->horizontalHeader()->setStretchLastSection(true);
    recoveryCompleteTable_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    recoveryCompleteTable_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    recoveryCompleteTable_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    recoveryCompleteTable_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    completeLayout->addWidget(recoveryCompleteTable_, 1);
    auto *completeActions = new QHBoxLayout();
    completeActions->setContentsMargins(0, 0, 0, 0);
    completeActions->addStretch(1);
    recoveryRetryFailedBtn_ = new QPushButton(QStringLiteral("重试失败项"), recoveryCompletePanel_);
    recoveryCompleteExportBtn_ = new QPushButton(QStringLiteral("导出明细..."), recoveryCompletePanel_);
    recoveryCompleteOpenDirBtn_ = new QPushButton(QStringLiteral("打开目录"), recoveryCompletePanel_);
    recoveryCompleteBackBtn_ = new QPushButton(QStringLiteral("返回扫描结果"), recoveryCompletePanel_);
    for (QPushButton *btn : {recoveryRetryFailedBtn_, recoveryCompleteExportBtn_, recoveryCompleteOpenDirBtn_, recoveryCompleteBackBtn_}) {
        btn->setFixedHeight(34);
        btn->setStyleSheet(QStringLiteral(
            "QPushButton { background: #ffffff; color: #374151; border: 1px solid #d1d5db; border-radius: 8px; padding: 0 14px; font-size: 13px; font-weight: 600; }"
            "QPushButton:hover { background: #f9fafb; }"));
        completeActions->addWidget(btn);
    }
    recoveryCompleteOpenDirBtn_->setStyleSheet(QStringLiteral(
        "QPushButton { background: #2563eb; color: #ffffff; border: none; border-radius: 8px;"
        " padding: 0 16px; font-size: 13px; font-weight: 700; }"
        "QPushButton:hover { background: #1d4ed8; }"
        "QPushButton:disabled { background: #bfdbfe; color: #ffffff; }"));
    recoveryRetryFailedBtn_->setEnabled(false);
    completeLayout->addLayout(completeActions);
    recoveryCompletePanel_->hide();
    recoveryCompletePanel_->raise();

    connect(stopScanBtn_,
            &QPushButton::clicked,
            this,
            [this]() {
                if (useMockData_) {
                    setScanFinished(true, QStringLiteral("扫描完成"));
                    return;
                }
                emit requestStopScan();
            });
    connect(backBtn_, &QPushButton::clicked, this, [this]() { emit requestBack(); });
    connect(recoverAllBtn_, &QPushButton::clicked, this, &RecoveryScanPage::onRecoverAllClicked);
    connect(scanPreviewResultsBtn_, &QPushButton::clicked, this, [this]() {
        // 允许在扫描尚未结束时查看当前结果；停止按钮仍保持可用。
        setResultsMode(true);
        if (resultsMetaLabel_) {
            resultsMetaLabel_->setText(QStringLiteral("扫描仍在后台继续 · 已发现：%1")
                                           .arg(static_cast<qulonglong>(foundTotal_)));
        }
    });
    connect(recoveryCompleteBackBtn_, &QPushButton::clicked, this, &RecoveryScanPage::hideRecoveryCompletePanel);
    connect(recoveryRetryFailedBtn_, &QPushButton::clicked, this, &RecoveryScanPage::onRetryFailedClicked);
    connect(recoveryResultFilterCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &RecoveryScanPage::applyRecoveryResultFilter);
    connect(recoveryCompleteExportBtn_, &QPushButton::clicked, this, &RecoveryScanPage::exportRecoveryDetailReport);
    connect(recoveryCompleteOpenDirBtn_, &QPushButton::clicked, this, [this]() {
        if (!recoveryLastTargetDir_.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(recoveryLastTargetDir_));
        }
    });

    connect(pathTree_,
            &QTreeWidget::currentItemChanged,
            this,
            [this](QTreeWidgetItem *cur, QTreeWidgetItem *) {
                if (leftTabs_->currentIndex() == 0) {
                    onPathTreeCurrentChanged(cur);
                }
            });
    connect(pathTree_, &QTreeWidget::itemChanged, this, &RecoveryScanPage::onPathTreeItemChanged);
    connect(pathTree_, &QTreeWidget::itemExpanded, this, &RecoveryScanPage::onTreeItemExpanded);
    connect(pathTree_, &QTreeWidget::itemCollapsed, this, &RecoveryScanPage::onTreeItemCollapsed);
    connect(typeTree_,
            &QTreeWidget::currentItemChanged,
            this,
            [this](QTreeWidgetItem *cur, QTreeWidgetItem *) {
                if (leftTabs_->currentIndex() == 1) {
                    onTypeTreeCurrentChanged(cur);
                }
            });
    connect(typeTree_, &QTreeWidget::itemChanged, this, &RecoveryScanPage::onTypeTreeItemChanged);
    connect(typeTree_, &QTreeWidget::itemExpanded, this, &RecoveryScanPage::onTreeItemExpanded);
    connect(typeTree_, &QTreeWidget::itemCollapsed, this, &RecoveryScanPage::onTreeItemCollapsed);
    connect(leftTabs_, &QTabWidget::currentChanged, this, &RecoveryScanPage::onLeftTabChanged);
    connect(viewToggleBtn_, &QPushButton::clicked, this, [this]() { setPreviewMode(!previewMode_); });
    if (tableView_ && tableView_->verticalScrollBar()) {
        connect(tableView_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
            onResultViewportChanged();
        });
    }
    if (gridView_ && gridView_->verticalScrollBar()) {
        connect(gridView_->verticalScrollBar(), &QScrollBar::valueChanged, this, [this](int) {
            onResultViewportChanged();
        });
    }

    searchTimer_ = new QTimer(this);
    searchTimer_->setSingleShot(true);
    searchTimer_->setInterval(300);
    connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString &text) {
        pendingSearchText_ = text.trimmed();
        searchTimer_->start();
    });
    connect(searchTimer_, &QTimer::timeout, this, [this]() {
        searchKeyword_ = pendingSearchText_;
        if (filterModel_) {
            filterModel_->setNameFilter(pendingSearchText_);
        }
        refreshSearchHighlight();
        if (tableView_ && filterModel_ && filterModel_->rowCount() > 0) {
            tableView_->selectRow(0);
        }
    });
    connect(searchBtn_, &QPushButton::clicked, this, [this]() {
        searchKeyword_ = searchEdit_ ? searchEdit_->text().trimmed() : QString();
        if (filterModel_) {
            filterModel_->setNameFilter(searchKeyword_);
        }
        refreshSearchHighlight();
        if (tableView_ && filterModel_ && filterModel_->rowCount() > 0) {
            tableView_->selectRow(0);
        }
    });
    connect(sortCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (!tableView_ || !filterModel_ || !resultsMode_) {
            return;
        }
        if (index == 0) {
            filterModel_->sort(-1, Qt::AscendingOrder);
            tableView_->setSortingEnabled(false);
            return;
        }
        const int column = index == 1 ? ScanResultModel::ColName
                           : index == 2 ? ScanResultModel::ColSize
                           : index == 3 ? ScanResultModel::ColModified
                                         : ScanResultModel::ColRecoverability;
        tableView_->setSortingEnabled(true);
        tableView_->sortByColumn(column, index == 4 ? Qt::DescendingOrder : Qt::AscendingOrder);
    });

    connect(scanModel_, &ScanResultModel::checkedCountChanged, this, [this](int checked, int) {
        if (!syncingTableChecks_) {
            updateHeaderSelectAllCheckState();
        }
        if (resultsSelectedSummaryLabel_) {
            resultsSelectedSummaryLabel_->setText(QLocale::system().toString(checked));
        }
        refreshResultsHeader();
    });
}

void RecoveryScanPage::setResultsMode(bool enabled)
{
    resultsMode_ = enabled;
    if (resultsContentWrap_) {
        resultsContentWrap_->setVisible(enabled);
    }
    if (scanFooter_) {
        scanFooter_->setVisible(enabled);
    }
    if (scanStatusCard_) {
        scanStatusCard_->setVisible(!enabled && scanRunning_);
    }
    if (resultsSummaryWrap_) {
        resultsSummaryWrap_->setVisible(enabled);
    }
    if (backBtn_) backBtn_->setVisible(enabled);
    if (viewToggleBtn_) viewToggleBtn_->setVisible(enabled);
    if (searchEdit_) searchEdit_->setVisible(enabled);
    if (searchBtn_) searchBtn_->setVisible(enabled);
    if (sortCombo_) sortCombo_->setVisible(enabled);
    if (stopScanBtn_) stopScanBtn_->setVisible(!enabled || scanRunning_);
    if (statusLabel_) statusLabel_->setVisible(!enabled);
    if (resultsMetaLabel_) resultsMetaLabel_->setVisible(enabled);
    // 旧版顶部进度条和横向指标仍由原逻辑更新，但视觉上由新版状态卡统一承载。
    if (progressBar_) progressBar_->setVisible(false);
    if (scanMetricWrap_) scanMetricWrap_->setVisible(false);
    if (footerSubLabel_) footerSubLabel_->setVisible(enabled);
    if (keepDirStructureCheck_) keepDirStructureCheck_->setVisible(enabled);
    if (formatAwareFragmentCheck_) formatAwareFragmentCheck_->setVisible(enabled);
    if (recoverAllBtn_) recoverAllBtn_->setVisible(enabled);
    if (!enabled) {
        setPreviewMode(false);
    }

    if (!enabled) {
        if (sortCombo_) sortCombo_->setCurrentIndex(0);
        if (resultsFilesSummaryLabel_) resultsFilesSummaryLabel_->setText(QStringLiteral("0"));
        if (resultsBytesSummaryLabel_) resultsBytesSummaryLabel_->setText(QStringLiteral("0 B"));
        if (resultsSelectedSummaryLabel_) resultsSelectedSummaryLabel_->setText(QStringLiteral("0"));
        if (resultsMetaLabel_) {
            resultsMetaLabel_->setText(QStringLiteral("已选：0"));
        }
        refreshSearchHighlight();
        return;
    }

    int checkedCount = scanModel_ ? scanModel_->checkedCount() : 0;
    if (resultsFilesSummaryLabel_) {
        resultsFilesSummaryLabel_->setText(QLocale::system().toString(foundTotal_));
    }
    if (resultsBytesSummaryLabel_) {
        resultsBytesSummaryLabel_->setText(totalBytes_ > 0 ? formatSize(static_cast<qint64>(totalBytes_))
                                                           : QStringLiteral("—"));
    }
    if (resultsSelectedSummaryLabel_) {
        resultsSelectedSummaryLabel_->setText(QLocale::system().toString(checkedCount));
    }
    if (resultsMetaLabel_) {
        resultsMetaLabel_->setText(QStringLiteral("已选：%1").arg(checkedCount));
    }
    if (footerFoundLabel_) {
        footerFoundLabel_->setText(QStringLiteral("扫描完成，找到文件：%1").arg(static_cast<qulonglong>(foundTotal_)));
    }
    if (footerSubLabel_) {
        footerSubLabel_->setText(totalBytes_ > 0
                                     ? QStringLiteral("预计数据量：%1").arg(formatSize(totalBytes_))
                                     : QStringLiteral("恢复前可继续筛选并勾选需要的文件"));
    }
    setPreviewMode(previewMode_);
    refreshSearchHighlight();
}

void RecoveryScanPage::setPreviewMode(bool enabled)
{
    previewMode_ = enabled && resultsMode_;
    if (previewFrame_) {
        previewFrame_->setVisible(resultsMode_);
    }
    if (previewDetailFrame_) {
        previewDetailFrame_->setVisible(!previewMode_ && resultsMode_);
    }
    if (gridView_) {
        gridView_->setVisible(previewMode_ && resultsMode_);
    }
    if (viewToggleBtn_) {
        viewToggleBtn_->setText(previewMode_ ? QStringLiteral("列表") : QStringLiteral("网格预览"));
    }
    if (previewFrame_ && previewFrame_->parentWidget()) {
        if (QHBoxLayout *layout = qobject_cast<QHBoxLayout *>(previewFrame_->parentWidget()->layout())) {
            layout->setStretch(0, 24);
            layout->setStretch(1, previewMode_ ? 0 : 52);
            layout->setStretch(2, previewMode_ ? 76 : 24);
        }
    }
    if (tableView_ && tableView_->parentWidget()) {
        tableView_->parentWidget()->setVisible(!previewMode_);
    }
    if (resultsMode_) {
        if (previewMode_) {
            if (gridView_) {
                gridView_->clear();
            }
        }
        visibleResultsLoaded_ = 0;
        ensureVisibleResultRowsLoaded(true);
    }
    refreshResultsHeader();
}

bool RecoveryScanPage::matchesSearch(const ScanFileDto &dto) const
{
    return matchesSearch(dto.name,
                         fileTypeDisplay(dto),
                         dto.fullPath);
}

bool RecoveryScanPage::matchesSearch(const QString &name, const QString &type, const QString &path) const
{
    const QString keyword = searchKeyword_.trimmed();
    if (keyword.isEmpty()) {
        return true;
    }

    return name.contains(keyword, Qt::CaseInsensitive) ||
           type.contains(keyword, Qt::CaseInsensitive) ||
           path.contains(keyword, Qt::CaseInsensitive);
}

void RecoveryScanPage::fillListView(const QList<ScanFileDto> &visibleFiles, Qt::CheckState defaultState)
{
    if (!tableView_ && !table_) {
        return;
    }

    resetVisibleResults(visibleFiles, defaultState);
}

void RecoveryScanPage::appendTableRow(const ScanFileDto &dto, Qt::CheckState state)
{
    Q_UNUSED(dto);
    Q_UNUSED(state);
}

QWidget *RecoveryScanPage::createGridCard(const ScanFileDto &dto, Qt::CheckState state)
{
    auto *card = new QFrame(gridView_);
    card->setObjectName(QStringLiteral("scanThumbCard"));
    card->setAttribute(Qt::WA_Hover, true);
    const auto applyCardState = [card](bool checked) {
        card->setStyleSheet(checked
            ? QStringLiteral(
                  "#scanThumbCard { background: #f7fbf8; border: 2px solid #22c55e; border-radius: 14px; }"
                  "#scanThumbCard:hover { background: #f1fbf4; border: 2px solid #16a34a; }"
                  "#scanThumbCard QLabel[role='name'] { color: #111827; font-size: 13px; font-weight: 700; }"
                  "#scanThumbCard QLabel[role='meta'] { color: #6b7280; font-size: 11px; }")
            : QStringLiteral(
                  "#scanThumbCard { background: #ffffff; border: 1px solid #e5e7eb; border-radius: 14px; }"
                  "#scanThumbCard:hover { background: #ffffff; border: 1px solid #c9d7ff; }"
                  "#scanThumbCard QLabel[role='name'] { color: #111827; font-size: 13px; font-weight: 700; }"
                  "#scanThumbCard QLabel[role='meta'] { color: #6b7280; font-size: 11px; }"));
    };
    applyCardState(state == Qt::Checked);

    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    auto *topRow = new QHBoxLayout();
    topRow->setContentsMargins(0, 0, 0, 0);
    topRow->setSpacing(6);
    auto *check = new QCheckBox(card);
    check->setCheckState(state);
    check->setProperty("rowId", QVariant::fromValue(dto.id));
    check->setProperty("isDirectory", dto.isDirectory);
    check->setProperty("relPath", relativeVolumePath(dto.fullPath, diskLetter_));
    check->setStyleSheet(QStringLiteral(
        "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #e5e7eb; border-radius: 4px; background: #ffffff; }"
        "QCheckBox::indicator:hover { border-color: #16a34a; }"
        "QCheckBox::indicator:checked { border: 1px solid #16a34a; background: #16a34a;"
        " image: url(:/qt-project.org/styles/commonstyle/images/checkbox_checked.png); }"));
    topRow->addWidget(check, 0, Qt::AlignLeft | Qt::AlignTop);
    topRow->addStretch(1);

    auto *sizeBadge = new QLabel(dto.isDirectory ? QStringLiteral("文件夹") : formatSize(dto.sizeBytes), card);
    sizeBadge->setStyleSheet(QStringLiteral("background: #f3f4f6; color: #4b5563; border-radius: 10px; padding: 2px 8px; font-size: 10px; font-weight: 700;"));
    topRow->addWidget(sizeBadge, 0, Qt::AlignRight | Qt::AlignTop);
    layout->addLayout(topRow);

    const QString lowerExt = dto.extension.trimmed().toLower();
    const bool isImage = GridPreview::isImageExt(lowerExt);
    const bool isVideo = GridPreview::isVideoExt(lowerExt);
    const bool isDocument = GridPreview::isDocumentExt(lowerExt);
    const bool isPreviewMedia = isImage || isVideo || isDocument;
    auto *thumbFrame = new QFrame(card);
    thumbFrame->setFixedHeight(126);
    QString thumbFrameCss;
    if (isVideo) {
        thumbFrameCss = QStringLiteral(
            "background: qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 #eef2ff, stop:0.5 #e0e7ff, stop:1 #dbeafe); "
            "border: 1px solid #c7d2fe; border-radius: 12px;");
    } else if (isImage) {
        thumbFrameCss = QStringLiteral(
            "background: qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 #e6fff2, stop:0.5 #eef6ff, stop:1 #dcfce7); "
            "border: 1px solid #bbf7d0; border-radius: 12px;");
    } else if (isDocument) {
        thumbFrameCss = QStringLiteral(
            "background: qlineargradient(x1:0,y1:0,x2:1,y2:1, stop:0 #fffbeb, stop:0.5 #fef3c7, stop:1 #fde68a); "
            "border: 1px solid #fcd34d; border-radius: 12px;");
    } else {
        thumbFrameCss =
            QStringLiteral("background: #f8fafc; border: 1px dashed #d1d5db; border-radius: 12px;");
    }
    thumbFrame->setStyleSheet(thumbFrameCss);
    auto *thumbLayout = new QVBoxLayout(thumbFrame);
    thumbLayout->setContentsMargins(10, 10, 10, 10);
    thumbLayout->setSpacing(0);

    auto *thumbPixLabel = new QLabel(thumbFrame);
    thumbPixLabel->setObjectName(QStringLiteral("gridThumbPix"));
    thumbPixLabel->setMinimumHeight(110);
    thumbPixLabel->setAlignment(Qt::AlignCenter);
    thumbPixLabel->setWordWrap(true);
    if (isPreviewMedia && dto.previewKind != ScanPreviewKind::None) {
        thumbPixLabel->setText(QStringLiteral("加载中…"));
        thumbPixLabel->setStyleSheet(QStringLiteral(
            "color: #9ca3af; font-size: 13px; font-weight: 600; background: transparent;"));
    } else if (isVideo) {
        thumbPixLabel->setText(QStringLiteral("视频预览"));
        thumbPixLabel->setStyleSheet(QStringLiteral(
            "color: #3730a3; font-size: 16px; font-weight: 800; background: transparent;"));
    } else if (isImage) {
        thumbPixLabel->setText(QStringLiteral("缩略图预览"));
        thumbPixLabel->setStyleSheet(QStringLiteral(
            "color: #166534; font-size: 18px; font-weight: 800; background: transparent;"));
    } else if (isDocument) {
        thumbPixLabel->setText(QStringLiteral("文档预览"));
        thumbPixLabel->setStyleSheet(QStringLiteral(
            "color: #92400e; font-size: 16px; font-weight: 800; background: transparent;"));
    } else {
        thumbPixLabel->setText(dto.extension.isEmpty() ? QStringLiteral("FILE")
                                                       : dto.extension.left(5).toUpper());
        thumbPixLabel->setStyleSheet(QStringLiteral(
            "color: #16a34a; font-size: 14px; font-weight: 800; background: transparent;"));
    }
    thumbLayout->addStretch(1);
    thumbLayout->addWidget(thumbPixLabel, 0, Qt::AlignHCenter);
    thumbLayout->addStretch(1);

    layout->addWidget(thumbFrame);

    if (isPreviewMedia && dto.previewKind != ScanPreviewKind::None) {
        scheduleGridThumbnail(dto.id);
    }

    auto *nameLabel = new QLabel(dto.name, card);
    nameLabel->setProperty("role", "name");
    nameLabel->setWordWrap(true);
    nameLabel->setMaximumHeight(38);
    layout->addWidget(nameLabel);

    auto *bottomRow = new QHBoxLayout();
    bottomRow->setContentsMargins(0, 0, 0, 0);
    bottomRow->setSpacing(8);
    auto *typeLabel = new QLabel(dto.isDirectory ? QStringLiteral("文件夹") : fileTypeDisplay(dto), card);
    typeLabel->setProperty("role", "meta");
    bottomRow->addWidget(typeLabel, 1, Qt::AlignLeft | Qt::AlignVCenter);
    auto *dateLabel = new QLabel(dto.isDirectory ? QStringLiteral("—") : formatFileTime(dto.modifyTimeFt), card);
    dateLabel->setProperty("role", "meta");
    dateLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    bottomRow->addWidget(dateLabel, 0, Qt::AlignRight | Qt::AlignVCenter);
    layout->addLayout(bottomRow);

    connect(check, &QCheckBox::stateChanged, this, [this, check, applyCardState](int) {
        if (syncingTableChecks_) {
            return;
        }
        applyCardState(check->checkState() == Qt::Checked);
        syncingTreeChecks_ = true;
        if (leftTabs_ && leftTabs_->currentIndex() == 1) {
            const quint64 id = check->property("rowId").toULongLong();
            ScanFileDto dto;
            if (resultStore_ && resultStore_->tryGetFile(id, &dto)) {
                const QString category = categoryForExtension(dto.extension);
                const QString sub = dto.extension.trimmed().toLower().isEmpty()
                                        ? QStringLiteral("无扩展名")
                                        : dto.extension.trimmed().toLower();
                QTreeWidgetItem *treeItem = ensureTypeTreeItemByKey(category, sub);
                if (treeItem) {
                    treeItem->setCheckState(0, check->checkState());
                    updateParentCheckState(treeItem);
                }
            }
        } else {
            const QString relPath = check->property("relPath").toString();
            const bool isDir = check->property("isDirectory").toBool();
            QString folderRel = relPath;
            if (!isDir) {
                const int slash = folderRel.lastIndexOf(QLatin1Char('\\'));
                folderRel = (slash >= 0) ? folderRel.left(slash) : QString();
            }
            QTreeWidgetItem *treeItem = ensurePathTreeItemByRelativePath(folderRel);
            if (treeItem) {
                treeItem->setCheckState(0, check->checkState());
                if (isDir) {
                    setTreeSubtreeCheckState(treeItem, check->checkState());
                }
                updateParentCheckState(treeItem);
            }
        }
        syncingTreeChecks_ = false;
        syncTableChecksFromTreeSelection();
        updateHeaderSelectAllCheckState();
    });

    return card;
}

void RecoveryScanPage::scheduleGridThumbnail(quint64 fileId)
{
    if (!resultStore_ || diskLetter_.isEmpty()) {
        return;
    }
    const quint64 listGen = visibleResultsGeneration_;
    const QString letter = diskLetter_;
    ScanResultStore *store = resultStore_;
    auto *job = new LambdaRunnable([this, fileId, listGen, letter, store]() {
        const QByteArray payload = loadPreviewThumbBytes(letter, store, fileId);
        ScanFileDto dto;
        if (!store->tryGetFile(fileId, &dto)) {
            QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                      Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                      Q_ARG(QPixmap, QPixmap()));
            return;
        }
        const QString ext = dto.extension.trimmed().toLower();

        if (GridPreview::isVideoExt(ext)) {
            if (payload.isEmpty()) {
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap()));
                return;
            }
            QString safeExt = ext;
            if (safeExt.isEmpty() || safeExt.size() > 8) {
                safeExt = QStringLiteral("bin");
            }
            QTemporaryFile tmp(QDir::tempPath() + QStringLiteral("/phxrv_thumb_XXXXXX.") + safeExt);
            tmp.setAutoRemove(false);
            if (!tmp.open()) {
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap()));
                return;
            }
            if (tmp.write(payload) != payload.size()) {
                const QString badPath = tmp.fileName();
                tmp.close();
                QFile::remove(badPath);
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap()));
                return;
            }
            tmp.flush();
            const QString path = tmp.fileName();
            tmp.close();
            QMetaObject::invokeMethod(this, "decodeVideoThumbnailOnMainThread", Qt::QueuedConnection,
                                      Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                      Q_ARG(QString, path));
            return;
        }

        if (GridPreview::isDocumentExt(ext)) {
            if (payload.isEmpty()) {
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap()));
                return;
            }
            QString safeExt = ext;
            if (safeExt.isEmpty() || safeExt.size() > 8) {
                safeExt = QStringLiteral("bin");
            }
            QTemporaryFile tmp(QDir::tempPath() + QStringLiteral("/phxrv_doc_XXXXXX.") + safeExt);
            tmp.setAutoRemove(false);
            if (!tmp.open()) {
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap()));
                return;
            }
            if (tmp.write(payload) != payload.size()) {
                const QString badPath = tmp.fileName();
                tmp.close();
                QFile::remove(badPath);
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap()));
                return;
            }
            tmp.flush();
            const QString path = tmp.fileName();
            tmp.close();
            QMetaObject::invokeMethod(this, "decodeDocumentThumbnailOnMainThread", Qt::QueuedConnection,
                                      Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                      Q_ARG(QString, path));
            return;
        }

        if (GridPreview::isImageExt(ext)) {
            if (payload.isEmpty()) {
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap()));
                return;
            }
            QImage img;
            if (img.loadFromData(payload)) {
                img = img.scaled(180, 110, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                const QPixmap pm = QPixmap::fromImage(img);
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, pm));
                return;
            }
            QString safeExt = ext;
            if (safeExt.isEmpty() || safeExt.size() > 8) {
                safeExt = QStringLiteral("bin");
            }
            QTemporaryFile tmp(QDir::tempPath() + QStringLiteral("/phxrv_img_XXXXXX.") + safeExt);
            tmp.setAutoRemove(false);
            if (!tmp.open()) {
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap()));
                return;
            }
            if (tmp.write(payload) != payload.size()) {
                const QString badPath = tmp.fileName();
                tmp.close();
                QFile::remove(badPath);
                QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                          Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                          Q_ARG(QPixmap, QPixmap()));
                return;
            }
            tmp.flush();
            const QString path = tmp.fileName();
            tmp.close();
            QMetaObject::invokeMethod(this, "decodeImageThumbnailOnMainThread", Qt::QueuedConnection,
                                      Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                      Q_ARG(QString, path));
            return;
        }

        QPixmap pm;
        if (!payload.isEmpty()) {
            QImage img;
            if (img.loadFromData(payload)) {
                img = img.scaled(180, 110, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                pm = QPixmap::fromImage(img);
            }
        }
        QMetaObject::invokeMethod(this, "applyGridThumbnail", Qt::QueuedConnection,
                                  Q_ARG(quint64, fileId), Q_ARG(quint64, listGen),
                                  Q_ARG(QPixmap, pm));
    });
    job->setAutoDelete(true);
    QThreadPool::globalInstance()->start(job);
}

void RecoveryScanPage::decodeVideoThumbnailOnMainThread(quint64 fileId, quint64 listGeneration,
                                                        QString tempFilePath)
{
    // 主线程：跨平台缩略图逻辑见 media/MediaThumbnails.cpp（Windows 含 Shell；其它平台为 FFmpeg 等）。
    QTimer::singleShot(0, this, [this, fileId, listGeneration, tempFilePath]() {
        const QPixmap pm = MediaThumbnails::videoPreviewPixmapForLocalFile(tempFilePath);
        QFile::remove(tempFilePath);
        applyGridThumbnail(fileId, listGeneration, pm);
    });
}

void RecoveryScanPage::decodeDocumentThumbnailOnMainThread(quint64 fileId, quint64 listGeneration,
                                                          QString tempFilePath)
{
    QTimer::singleShot(0, this, [this, fileId, listGeneration, tempFilePath]() {
        const QPixmap pm = MediaThumbnails::documentPreviewPixmapForLocalFile(tempFilePath);
        QFile::remove(tempFilePath);
        applyGridThumbnail(fileId, listGeneration, pm);
    });
}

void RecoveryScanPage::decodeImageThumbnailOnMainThread(quint64 fileId, quint64 listGeneration,
                                                        QString tempFilePath)
{
    QTimer::singleShot(0, this, [this, fileId, listGeneration, tempFilePath]() {
        const QPixmap pm = MediaThumbnails::imagePreviewPixmapForLocalFile(tempFilePath);
        QFile::remove(tempFilePath);
        applyGridThumbnail(fileId, listGeneration, pm);
    });
}

void RecoveryScanPage::applyGridThumbnail(quint64 fileId, quint64 listGeneration, QPixmap pixmap)
{
    if (listGeneration != visibleResultsGeneration_) {
        return;
    }
    if (!gridView_) {
        return;
    }
    for (int i = 0; i < gridView_->count(); ++i) {
        QListWidgetItem *item = gridView_->item(i);
        if (!item || item->data(Qt::UserRole).toULongLong() != fileId) {
            continue;
        }
        QWidget *card = gridView_->itemWidget(item);
        if (!card) {
            continue;
        }
        QLabel *lab = card->findChild<QLabel *>(QStringLiteral("gridThumbPix"));
        if (!lab) {
            continue;
        }
        if (!pixmap.isNull()) {
            lab->setPixmap(pixmap);
            lab->setText(QString());
            lab->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
        } else {
            lab->setText(QStringLiteral("无法预览"));
            lab->setStyleSheet(QStringLiteral(
                "color: #9ca3af; font-size: 12px; font-weight: 600; background: transparent;"));
        }
        break;
    }
}

void RecoveryScanPage::fillGridView(const QList<ScanFileDto> &visibleFiles, Qt::CheckState defaultState)
{
    if (!gridView_) {
        return;
    }

    resetVisibleResults(visibleFiles, defaultState);
}

void RecoveryScanPage::appendGridRow(const ScanFileDto &dto, Qt::CheckState state)
{
    if (!gridView_ || gridContainsDto(dto.id)) {
        return;
    }

    auto *item = new QListWidgetItem(gridView_);
    item->setData(Qt::UserRole, QVariant::fromValue(dto.id));
    item->setSizeHint(QSize(214, 232));
    gridView_->addItem(item);
    gridView_->setItemWidget(item, createGridCard(dto, state));
}

bool RecoveryScanPage::tableContainsDto(quint64 id) const
{
    Q_UNUSED(id);
    return false;
}

bool RecoveryScanPage::gridContainsDto(quint64 id) const
{
    if (!gridView_) {
        return false;
    }
    for (int i = 0; i < gridView_->count(); ++i) {
        QListWidgetItem *item = gridView_->item(i);
        if (item && item->data(Qt::UserRole).toULongLong() == id) {
            return true;
        }
    }
    return false;
}

bool RecoveryScanPage::currentSelectionAcceptsDto(const ScanFileDto &dto, Qt::CheckState *state) const
{
    if (!leftTabs_) {
        return false;
    }

    Qt::CheckState resolvedState = Qt::Unchecked;
    if (leftTabs_->currentIndex() == 0) {
        QTreeWidgetItem *current = pathTree_ ? pathTree_->currentItem() : nullptr;
        if (!current) {
            current = pathRoot_;
        }
        if (!current) {
            return false;
        }

        const QString folderRel = itemRelativeFolderPath(current);
        QString rel = relativeVolumePath(dto.fullPath, diskLetter_);
        rel.replace(QLatin1Char('/'), QLatin1Char('\\'));
        QString parentRel = rel;
        const int slash = parentRel.lastIndexOf(QLatin1Char('\\'));
        parentRel = (slash >= 0) ? parentRel.left(slash) : QString();

        const bool accepted = (parentRel == folderRel) && matchesSearch(dto);
        if (!accepted) {
            return false;
        }
        resolvedState = isPathUnderCheckedFolder(dto.isDirectory ? rel : parentRel, dto.isDirectory)
                            ? Qt::Checked
                            : Qt::Unchecked;
    } else {
        QTreeWidgetItem *current = typeTree_ ? typeTree_->currentItem() : nullptr;
        if (!current || dto.isDirectory || !matchesSearch(dto)) {
            return false;
        }

        const QString currentCat = current->data(0, Qt::UserRole + 6).toString();
        const QString currentSub = current->data(0, Qt::UserRole + 7).toString();
        const QString dtoCat = categoryForExtension(dto.extension);
        const QString dtoSub = dto.extension.trimmed().toLower().isEmpty()
                                   ? QStringLiteral("无扩展名")
                                   : dto.extension.trimmed().toLower();

        bool accepted = false;
        if (!currentCat.isEmpty() && !currentSub.isEmpty()) {
            accepted = (dtoCat == currentCat && dtoSub == currentSub);
        } else {
            accepted = (dtoCat == baseTreeLabel(current));
        }
        if (!accepted) {
            return false;
        }
        resolvedState = current->checkState(0) == Qt::Checked ? Qt::Checked : Qt::Unchecked;
    }

    if (state) {
        *state = resolvedState;
    }
    return true;
}

void RecoveryScanPage::markTreeItemCaptionDirty(QTreeWidgetItem *item)
{
    if (!item) {
        return;
    }
    if (batchingUiUpdate_) {
        dirtyTreeCaptionItems_.insert(item);
    } else {
        refreshTreeCaption(item);
    }
}

void RecoveryScanPage::flushDirtyTreeCaptions()
{
    if (dirtyTreeCaptionItems_.isEmpty()) {
        return;
    }
    const auto items = dirtyTreeCaptionItems_.values();
    dirtyTreeCaptionItems_.clear();
    for (QTreeWidgetItem *item : items) {
        refreshTreeCaption(item);
    }
}

void RecoveryScanPage::updateTreeItemCount(QTreeWidgetItem *item)
{
    if (!item) {
        return;
    }

    int newCount = 0;
    qint64 newDirectFileBytes = item->data(0, Qt::UserRole + 10).toLongLong();
    if (item->treeWidget() == pathTree_) {
        const QString relPath = itemRelativeFolderPath(item);
        newCount = resultStore_ ? resultStore_->pathNodeCount(relPath) : 0;
        newDirectFileBytes = resultStore_ ? resultStore_->pathDirectFileBytes(relPath) : 0;
    } else if (item->treeWidget() == typeTree_) {
        const QString cat = item->data(0, Qt::UserRole + 6).toString();
        const QString sub = item->data(0, Qt::UserRole + 7).toString();
        // store 的 key：类别节点使用 cat；子类节点使用 cat::sub。
        const QString key = cat.isEmpty()
                                ? baseTreeLabel(item)
                                : (sub.isEmpty() ? cat : (cat + QStringLiteral("::") + sub));
        newCount = resultStore_ ? resultStore_->typeNodeCount(key) : 0;
    } else {
        return;
    }

    const int oldCount = item->data(0, Qt::UserRole + 3).toInt();
    const qint64 oldDirectFileBytes = item->data(0, Qt::UserRole + 10).toLongLong();
    if (oldCount == newCount && oldDirectFileBytes == newDirectFileBytes) {
        return;
    }

    item->setData(0, Qt::UserRole + 3, newCount);
    item->setData(0, Qt::UserRole + 10, newDirectFileBytes);
    markTreeItemCaptionDirty(item);
}

QTreeWidgetItem *RecoveryScanPage::createPathTreeItem(const QString &relPath, const QString &name) const
{
    auto *item = new QTreeWidgetItem(QStringList() << name << QString());
    item->setData(0, Qt::UserRole + 2, name);
    item->setData(0, Qt::UserRole + 4, static_cast<int>(TreeItemKind::Directory));
    item->setData(0, Qt::UserRole + 5, relPath);
    item->setData(0, Qt::UserRole + 9, false);
    if (resultStore_ && !resultStore_->pathChildDirectories(relPath).isEmpty()) {
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
    enableTreeCheckbox(item, false);
    return item;
}

static int desiredTypeCategoryInsertIndex(QTreeWidget *tree, const QString &category);

QTreeWidgetItem *RecoveryScanPage::createTypeCategoryItem(const QString &category) const
{
    auto *item = new QTreeWidgetItem(QStringList() << category << QString());
    item->setData(0, Qt::UserRole + 2, category);
    item->setData(0, Qt::UserRole + 4, static_cast<int>(TreeItemKind::Category));
    item->setData(0, Qt::UserRole + 6, category);
    item->setData(0, Qt::UserRole + 7, QString());
    const QStringList fixedChildren = fixedTypeChildrenForCategory(category);
    // 子节点固定集合存在时，这些子节点在创建时已经被 add 进树中；因此标记为已加载，
    // 避免 onTreeItemExpanded 时重复 delete/recreate 引发崩溃。
    item->setData(0, Qt::UserRole + 9, !fixedChildren.isEmpty());
    // 固定类别：只由 fixed 列表决定是否显示展开指示器；不依赖扫描结果动态子项。
    if (!fixedChildren.isEmpty()) {
        item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
    enableTreeCheckbox(item, false);
    for (const QString &subLabel : fixedChildren) {
        QTreeWidgetItem *child = createTypeSubItem(category, subLabel);
        item->addChild(child);
    }
    return item;
}

QTreeWidgetItem *RecoveryScanPage::createTypeSubItem(const QString &category, const QString &subLabel) const
{
    auto *item = new QTreeWidgetItem(QStringList() << subLabel << QString());
    item->setData(0, Qt::UserRole + 2, subLabel);
    item->setData(0, Qt::UserRole + 4, static_cast<int>(TreeItemKind::File));
    item->setData(0, Qt::UserRole + 6, category);
    item->setData(0, Qt::UserRole + 7, subLabel);
    item->setData(0, Qt::UserRole + 9, true);
    enableTreeCheckbox(item, false);
    return item;
}

int RecoveryScanPage::ensurePathChildrenLoaded(QTreeWidgetItem *item, int maxAppend, const char *source)
{
    if (!item || item->treeWidget() != pathTree_) {
        return 0;
    }

    const QString parentRel = itemRelativeFolderPath(item);
    const QStringList childDirs = resultStore_ ? resultStore_->pathChildDirectories(parentRel) : QStringList();
    const int beforeCount = item->childCount();
    const QString sourceText = QString::fromLatin1(source ? source : "unknown");
    if (item == pathRoot_ && beforeCount >= childDirs.size()) {
        LOG_INFO(L"RecoveryScanPage::ensurePathChildrenLoaded skip root no-op"
                 L" source=[" + std::wstring(reinterpret_cast<const wchar_t *>(sourceText.utf16()),
                                             static_cast<size_t>(sourceText.size())) + L"]" +
                 L" beforeChildren=" + std::to_wstring(static_cast<uint64_t>(beforeCount)) +
                 L" sourceDirs=" + std::to_wstring(static_cast<uint64_t>(childDirs.size())));
        return 0;
    }
    int appendedCount = 0;
    for (const QString &childName : childDirs) {
        if (maxAppend >= 0 && appendedCount >= maxAppend) {
            break;
        }
        const QString childRel = parentRel.isEmpty() ? childName
                                                     : (parentRel + QStringLiteral("\\") + childName);
        if (findPathChildInsensitive(item, childName)) {
            continue;
        }
        QTreeWidgetItem *child = createPathTreeItem(childRel, childName);
        if (item->checkState(0) == Qt::Checked || isPathUnderCheckedFolder(childRel, true)) {
            child->setCheckState(0, Qt::Checked);
        }
        item->addChild(child);
        updateTreeItemCount(child);
        ++appendedCount;
    }
    item->setData(0, Qt::UserRole + 9, true);
    item->setChildIndicatorPolicy(item->childCount() > 0
                                      ? QTreeWidgetItem::ShowIndicator
                                      : QTreeWidgetItem::DontShowIndicatorWhenChildless);
    LOG_INFO(L"RecoveryScanPage::ensurePathChildrenLoaded"
             L" source=[" + std::wstring(reinterpret_cast<const wchar_t *>(sourceText.utf16()),
                                         static_cast<size_t>(sourceText.size())) + L"]" +
             L" parent=[" + std::wstring(reinterpret_cast<const wchar_t *>(parentRel.utf16()),
                                        static_cast<size_t>(parentRel.size())) + L"]" +
             L" sourceDirs=" + std::to_wstring(static_cast<uint64_t>(childDirs.size())) +
             L" beforeChildren=" + std::to_wstring(static_cast<uint64_t>(beforeCount)) +
             L" appended=" + std::to_wstring(static_cast<uint64_t>(appendedCount)) +
             L" afterChildren=" + std::to_wstring(static_cast<uint64_t>(item->childCount())));
    return appendedCount;
}

void RecoveryScanPage::ensureTypeChildrenLoaded(QTreeWidgetItem *item)
{
    if (!item || item->treeWidget() != typeTree_) {
        return;
    }

    const QString category = item->data(0, Qt::UserRole + 6).toString();
    if (category.isEmpty()) {
        return;
    }
    const QStringList fixedChildren = fixedTypeChildrenForCategory(category);
    const QString prefix = category + QStringLiteral("::");

    // 对固定类别：如果当前子节点已经是 fixedChildren 的正确集合与顺序，则不删除/不重建，只同步映射。
    if (!fixedChildren.isEmpty() && item->data(0, Qt::UserRole + 9).toBool()) {
        bool orderOk = (item->childCount() == fixedChildren.size());
        if (orderOk) {
            for (int i = 0; i < fixedChildren.size(); ++i) {
                QTreeWidgetItem *ch = item->child(i);
                if (!ch) {
                    orderOk = false;
                    break;
                }
                const QString sub = ch->data(0, Qt::UserRole + 7).toString();
                if (sub.compare(fixedChildren.at(i), Qt::CaseInsensitive) != 0) {
                    orderOk = false;
                    break;
                }
            }
        }

        if (orderOk) {
            // 重建该 category 的子项映射（避免指针/keys 不一致导致崩溃）
            QVector<QString> keysToRemove;
            keysToRemove.reserve(static_cast<int>(typeSubcategoryItems_.size()));
            for (auto it = typeSubcategoryItems_.cbegin(); it != typeSubcategoryItems_.cend(); ++it) {
                if (it.key().startsWith(prefix)) {
                    keysToRemove.append(it.key());
                }
            }
            for (const QString &k : keysToRemove) {
                typeSubcategoryItems_.remove(k);
            }

            for (int i = 0; i < item->childCount(); ++i) {
                QTreeWidgetItem *ch = item->child(i);
                if (!ch) {
                    continue;
                }
                const QString sub = ch->data(0, Qt::UserRole + 7).toString();
                const QString key = prefix + sub;
                typeSubcategoryItems_.insert(key, ch);
                updateTreeItemCount(ch);
            }

            item->setData(0, Qt::UserRole + 9, true);
            item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
            return;
        }
    }

    // 需要重建时：保留旧的 checkState（只用于 fixedChildren 中的那些子项）。
    QHash<QString, Qt::CheckState> oldCheckByLower;
    oldCheckByLower.reserve(static_cast<int>(fixedChildren.size()));
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem *ch = item->child(i);
        if (!ch) {
            continue;
        }
        const QString sub = ch->data(0, Qt::UserRole + 7).toString();
        oldCheckByLower.insert(sub.toLower(), ch->checkState(0));
    }

    // 清空当前 category 下所有子节点（彻底杜绝串类/残留）。
    while (item->childCount() > 0) {
        QTreeWidgetItem *ch = item->takeChild(0);
        delete ch;
    }

    // 清空该 category 的子项映射，之后再按 fixedChildren 重建。
    QVector<QString> keysToRemove;
    keysToRemove.reserve(static_cast<int>(typeSubcategoryItems_.size()));
    for (auto it = typeSubcategoryItems_.cbegin(); it != typeSubcategoryItems_.cend(); ++it) {
        if (it.key().startsWith(prefix)) {
            keysToRemove.append(it.key());
        }
    }
    for (const QString &k : keysToRemove) {
        typeSubcategoryItems_.remove(k);
    }

    // 固定列表重建子节点
    for (const QString &subLabel : fixedChildren) {
        const QString key = prefix + subLabel;
        QTreeWidgetItem *child = createTypeSubItem(category, subLabel);

        Qt::CheckState st = oldCheckByLower.value(subLabel.toLower(), Qt::Unchecked);
        if (item->checkState(0) == Qt::Checked) {
            st = Qt::Checked;
        } else if (isTypeUnderCheckedNode(category, subLabel)) {
            st = Qt::Checked;
        }

        if (st == Qt::Checked) {
            child->setCheckState(0, Qt::Checked);
        }

        item->addChild(child);
        typeSubcategoryItems_.insert(key, child);
        updateTreeItemCount(child);
    }

    item->setData(0, Qt::UserRole + 9, true);
    item->setChildIndicatorPolicy(fixedChildren.isEmpty()
                                      ? QTreeWidgetItem::DontShowIndicatorWhenChildless
                                      : QTreeWidgetItem::ShowIndicator);
}

QTreeWidgetItem *RecoveryScanPage::ensurePathTreeItemByRelativePath(const QString &relPath)
{
    QTreeWidgetItem *existing = findPathTreeItemByRelativePath(relPath);
    if (existing || !pathTree_ || !pathRoot_) {
        return existing;
    }

    if (relPath.isEmpty()) {
        return pathRoot_;
    }

    QStringList parts = relPath.split(QChar('\\'), QString::SkipEmptyParts);
    QTreeWidgetItem *current = pathRoot_;
    QString running;
    for (const QString &part : parts) {
        ensurePathChildrenLoaded(current, 2, "ensurePathTreeItemByRelativePath");
        running = running.isEmpty() ? part : (running + QStringLiteral("\\") + part);
        QTreeWidgetItem *next = findPathChildInsensitive(current, part);
        if (!next) {
            next = createPathTreeItem(running, part);
            current->addChild(next);
            updateTreeItemCount(next);
        }
        current = next;
    }
    return current;
}

QTreeWidgetItem *RecoveryScanPage::ensureTypeTreeItemByKey(const QString &category, const QString &subLabel)
{
    if (!typeTree_ || category.isEmpty()) {
        return nullptr;
    }
    QTreeWidgetItem *catItem = typeCategoryItems_.value(category);
    if (!catItem) {
        catItem = createTypeCategoryItem(category);
        typeTree_->insertTopLevelItem(desiredTypeCategoryInsertIndex(typeTree_, category), catItem);
        typeCategoryItems_.insert(category, catItem);
        updateTreeItemCount(catItem);
    }
    if (subLabel.isEmpty()) {
        return catItem;
    }
    ensureTypeChildrenLoaded(catItem);
    const QString key = category + QStringLiteral("::") + subLabel;
    return typeSubcategoryItems_.value(key, nullptr);
}

void RecoveryScanPage::refreshCheckedKeyCaches()
{
    checkedPathKeys_.clear();
    checkedTypeKeys_.clear();
    const QStringList pathKeys = checkedPathTreeItems();
    for (const QString &key : pathKeys) {
        checkedPathKeys_.insert(key);
    }
    const QStringList typeKeys = checkedTypeTreeItems();
    for (const QString &key : typeKeys) {
        checkedTypeKeys_.insert(key);
    }
}

Qt::CheckState RecoveryScanPage::computePathParentState(const QString &relPath) const
{
    if (checkedPathKeys_.contains(relPath)) {
        return Qt::Checked;
    }
    bool hasCheckedDescendant = false;
    for (auto it = checkedPathKeys_.cbegin(); it != checkedPathKeys_.cend(); ++it) {
        const QString &checkedKey = *it;
        if (relPath.isEmpty()) {
            hasCheckedDescendant = true;
            break;
        }
        if (checkedKey.startsWith(relPath + QStringLiteral("\\"))) {
            hasCheckedDescendant = true;
            break;
        }
    }
    return hasCheckedDescendant ? Qt::PartiallyChecked : Qt::Unchecked;
}

Qt::CheckState RecoveryScanPage::computeTypeParentState(const QString &key) const
{
    if (checkedTypeKeys_.contains(key)) {
        return Qt::Checked;
    }
    bool hasCheckedDescendant = false;
    for (auto it = checkedTypeKeys_.cbegin(); it != checkedTypeKeys_.cend(); ++it) {
        const QString &checkedKey = *it;
        if (checkedKey.startsWith(key + QStringLiteral("::"))) {
            hasCheckedDescendant = true;
            break;
        }
    }
    return hasCheckedDescendant ? Qt::PartiallyChecked : Qt::Unchecked;
}

bool RecoveryScanPage::isTypeUnderCheckedNode(const QString &category, const QString &subLabel) const
{
    if (category.isEmpty()) {
        return false;
    }
    const QString leafKey = category + QStringLiteral("::") + subLabel;
    return checkedTypeKeys_.contains(category) || checkedTypeKeys_.contains(leafKey);
}

int RecoveryScanPage::targetVisibleRowCount() const
{
    const int rowHeight = previewMode_ ? 232 : 36;
    const int viewportHeight = previewMode_
                                   ? (gridView_ && gridView_->viewport() ? gridView_->viewport()->height() : 0)
                                   : (tableView_ && tableView_->viewport() ? tableView_->viewport()->height() : 0);
    const int viewportRows = (viewportHeight > 0 && rowHeight > 0) ? (viewportHeight / rowHeight) + 6 : 24;
    return qMax(24, viewportRows);
}

void RecoveryScanPage::resetVisibleResults(const QList<ScanFileDto> &visibleFiles, Qt::CheckState defaultState)
{
    ++visibleResultsGeneration_;
    visibleResultsUseTypePaging_ = false;
    visibleResultsContinuationScheduled_ = false;
    visibleTypeCandidateIds_.clear();
    visibleTypeCandidateCursor_ = 0;
    visibleResults_ = visibleFiles;
    visibleResultsDefaultState_ = defaultState;
    visibleResultsLoaded_ = 0;

    if (gridView_ && previewMode_) {
        gridView_->clear();
        gridView_->setSpacing(14);
        gridView_->setGridSize(QSize(228, 246));
    }

    Q_UNUSED(defaultState);
    if (visibleResults_.isEmpty()) {
        updateHeaderSelectAllCheckState();
        refreshResultsHeader();
        return;
    }
    ensureVisibleResultRowsLoaded(true);
}

void RecoveryScanPage::ensureVisibleResultRowsLoaded(bool force)
{
    if ((!tableView_ && !table_ && !gridView_) || (!visibleResultsUseTypePaging_ && visibleResults_.isEmpty())) {
        updateHeaderSelectAllCheckState();
        refreshResultsHeader();
        return;
    }

    int desiredCount = force ? targetVisibleRowCount() : visibleResultsLoaded_;
    if (previewMode_) {
        if (gridView_ && gridView_->verticalScrollBar()) {
            QScrollBar *bar = gridView_->verticalScrollBar();
            if (force || bar->value() >= bar->maximum() - 160) {
                desiredCount = qMax(desiredCount, visibleResultsLoaded_ + targetVisibleRowCount());
            }
        }
    } else {
        if (tableView_ && tableView_->verticalScrollBar()) {
            QScrollBar *bar = tableView_->verticalScrollBar();
            if (force || bar->value() >= bar->maximum() - 160) {
                desiredCount = qMax(desiredCount, visibleResultsLoaded_ + targetVisibleRowCount());
            }
        }
    }
    if (visibleResultsUseTypePaging_) {
        const quint64 generation = visibleResultsGeneration_;
        QElapsedTimer batchTimer;
        batchTimer.start();
        const int maxCandidateChecks = force ? qMax(4096, desiredCount * 32) : qMax(2048, desiredCount * 16);
        int checkedCandidates = 0;
        const int loadedBefore = visibleResultsLoaded_;
        while (visibleResultsLoaded_ < desiredCount &&
               generation == visibleResultsGeneration_ &&
               visibleTypeCandidateCursor_ < visibleTypeCandidateIds_.size() &&
               checkedCandidates < maxCandidateChecks) {
            if (generation != visibleResultsGeneration_ ||
                visibleTypeCandidateCursor_ >= visibleTypeCandidateIds_.size()) {
                break;
            }
            const quint64 id = visibleTypeCandidateIds_.at(visibleTypeCandidateCursor_++);
            ++checkedCandidates;
            ScanFileDto dto;
            if (!resultStore_ || !resultStore_->tryGetFile(id, &dto) || dto.isDirectory || !matchesSearch(dto)) {
                continue;
            }
            Qt::CheckState state = Qt::Unchecked;
            currentSelectionAcceptsDto(dto, &state);
            if (previewMode_) {
                appendGridRow(dto, state);
            } else {
                appendTableRow(dto, state);
            }
            ++visibleResultsLoaded_;
            if (generation != visibleResultsGeneration_) {
                break;
            }
        }
        if (generation != visibleResultsGeneration_) {
            LOG_INFO(L"RecoveryScanPage::ensureVisibleResultRowsLoaded type-page abort stale generation"
                     L" generation=" + std::to_wstring(static_cast<uint64_t>(generation)) +
                     L" currentGeneration=" + std::to_wstring(static_cast<uint64_t>(visibleResultsGeneration_)));
            return;
        }
        const bool hasMoreCandidates = visibleTypeCandidateCursor_ < visibleTypeCandidateIds_.size();
        const bool needsMoreRows = visibleResultsLoaded_ < desiredCount;
        const qint64 elapsedMs = batchTimer.elapsed();
        if (checkedCandidates > 0 || force) {
            LOG_INFO(L"RecoveryScanPage::ensureVisibleResultRowsLoaded type-page"
                     L" force=" + std::to_wstring(force ? 1 : 0) +
                     L" loadedBefore=" + std::to_wstring(static_cast<uint64_t>(loadedBefore)) +
                     L" loadedAfter=" + std::to_wstring(static_cast<uint64_t>(visibleResultsLoaded_)) +
                     L" desired=" + std::to_wstring(static_cast<uint64_t>(desiredCount)) +
                     L" checkedCandidates=" + std::to_wstring(static_cast<uint64_t>(checkedCandidates)) +
                     L" cursor=" + std::to_wstring(static_cast<uint64_t>(visibleTypeCandidateCursor_)) +
                     L"/" + std::to_wstring(static_cast<uint64_t>(visibleTypeCandidateIds_.size())) +
                     L" elapsedMs=" + std::to_wstring(static_cast<uint64_t>(elapsedMs)));
        }
        if (needsMoreRows && hasMoreCandidates && !visibleResultsContinuationScheduled_) {
            visibleResultsContinuationScheduled_ = true;
            QTimer::singleShot(0, this, [this]() {
                visibleResultsContinuationScheduled_ = false;
                ensureVisibleResultRowsLoaded(false);
            });
        }
    } else {
        desiredCount = qMin(desiredCount, visibleResults_.size());
        if (desiredCount <= visibleResultsLoaded_) {
            updateHeaderSelectAllCheckState();
            refreshResultsHeader();
            return;
        }

        for (int i = visibleResultsLoaded_; i < desiredCount; ++i) {
            Qt::CheckState state = Qt::Unchecked;
            currentSelectionAcceptsDto(visibleResults_.at(i), &state);
            if (previewMode_) {
                appendGridRow(visibleResults_.at(i), state);
            } else {
                appendTableRow(visibleResults_.at(i), state);
            }
        }
        visibleResultsLoaded_ = desiredCount;
    }
    syncTableChecksFromTreeSelection();
    updateHeaderSelectAllCheckState();
    refreshResultsHeader();
}

QList<ScanFileDto> RecoveryScanPage::collectVisibleFilesForPath(const QString &folderRel) const
{
    QList<ScanFileDto> visibleFiles;
    const QVector<quint64> ids = resultStore_ ? resultStore_->pathDirectChildIds(folderRel) : QVector<quint64>();
    for (quint64 id : ids) {
        ScanFileDto dto;
        if (!resultStore_ || !resultStore_->tryGetFile(id, &dto)) {
            continue;
        }
        if (dto.isDirectory) {
            const QString fullPath = dto.fullPath.isEmpty()
                                         ? (diskLetter_ + QStringLiteral("\\") + dto.name)
                                         : dto.fullPath;
            if (matchesSearch(dto.name, QStringLiteral("文件夹"), fullPath)) {
                visibleFiles.append(dto);
            }
        } else if (matchesSearch(dto)) {
            visibleFiles.append(dto);
        }
    }
    std::sort(visibleFiles.begin(), visibleFiles.end(), [](const ScanFileDto &a, const ScanFileDto &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return visibleFiles;
}

QList<ScanFileDto> RecoveryScanPage::collectVisibleFilesForType(QTreeWidgetItem *item) const
{
    QList<ScanFileDto> visibleFiles;
    if (!item) {
        return visibleFiles;
    }

    const QString cat = item->data(0, Qt::UserRole + 6).toString();
    const QString sub = item->data(0, Qt::UserRole + 7).toString();
    const QString key = sub.isEmpty() ? cat : (cat + QStringLiteral("::") + sub);
    const QVector<quint64> ids = resultStore_ ? resultStore_->typeNodeFileIds(key) : QVector<quint64>();
    for (quint64 id : ids) {
        ScanFileDto dto;
        if (!resultStore_ || !resultStore_->tryGetFile(id, &dto)) {
            continue;
        }
        if (!dto.isDirectory && matchesSearch(dto)) {
            visibleFiles.append(dto);
        }
    }
    std::sort(visibleFiles.begin(), visibleFiles.end(), [](const ScanFileDto &a, const ScanFileDto &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return visibleFiles;
}

static bool lessTypeCandidateId(quint64 lhs, quint64 rhs)
{
    return lhs < rhs;
}

static bool shouldSortTypeCandidates(const QString &category)
{
    return category != QStringLiteral("其他") &&
           category != QStringLiteral("未保存的文件");
}

static QStringList preferredTypeCategoryOrder()
{
    return QStringList()
        << QStringLiteral("图片")
        << QStringLiteral("视频")
        << QStringLiteral("文档")
        << QStringLiteral("音频")
        << QStringLiteral("压缩文件")
        << QStringLiteral("邮件")
        << QStringLiteral("书签")
        << QStringLiteral("其他")
        << QStringLiteral("未保存的文件");
}

static int typeCategoryOrderIndex(const QString &category)
{
    const QStringList preferred = preferredTypeCategoryOrder();
    const int preferredIndex = preferred.indexOf(category);
    return preferredIndex >= 0 ? preferredIndex : (preferred.size() + 1000);
}

static int desiredTypeCategoryInsertIndex(QTreeWidget *tree, const QString &category)
{
    if (!tree) {
        return 0;
    }
    const int targetOrder = typeCategoryOrderIndex(category);
    const QString targetFolded = category.toCaseFolded();
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *existing = tree->topLevelItem(i);
        if (!existing) {
            continue;
        }
        const QString existingCategory = existing->data(0, Qt::UserRole + 6).toString();
        const int existingOrder = typeCategoryOrderIndex(existingCategory);
        if (targetOrder < existingOrder) {
            return i;
        }
        if (targetOrder == existingOrder &&
            targetFolded.compare(existingCategory.toCaseFolded(), Qt::CaseInsensitive) < 0) {
            return i;
        }
    }
    return tree->topLevelItemCount();
}

void RecoveryScanPage::onTreeItemExpanded(QTreeWidgetItem *item)
{
    if (!item) {
        return;
    }
    if (suppressTreeExpandLoad_) {
        return;
    }
    if (item->treeWidget() == pathTree_) {
        ensurePathChildrenLoaded(item, item == pathRoot_ ? 4 : 2, "onTreeItemExpanded");
        if (item->checkState(0) == Qt::Checked) {
            syncingTreeChecks_ = true;
            for (int i = 0; i < item->childCount(); ++i) {
                item->child(i)->setCheckState(0, Qt::Checked);
            }
            syncingTreeChecks_ = false;
        }
    } else if (item->treeWidget() == typeTree_) {
        const QString category = item->data(0, Qt::UserRole + 6).toString();
        const QString subLabel = item->data(0, Qt::UserRole + 7).toString();
        const QString label = baseTreeLabel(item);
        if (category == QStringLiteral("图片") || label == QStringLiteral("图片")) {
            LOG_INFO(std::wstring(L"RecoveryScanPage::onTreeItemExpanded"
                                  L" tree=[type]") +
                     L" category=[" + std::wstring(reinterpret_cast<const wchar_t *>(category.utf16()),
                                                  static_cast<size_t>(category.size())) + L"]" +
                     L" sub=[" + std::wstring(reinterpret_cast<const wchar_t *>(subLabel.utf16()),
                                             static_cast<size_t>(subLabel.size())) + L"]" +
                     L" label=[" + std::wstring(reinterpret_cast<const wchar_t *>(label.utf16()),
                                               static_cast<size_t>(label.size())) + L"]" +
                     L" childCountBefore=" + std::to_wstring(static_cast<uint64_t>(item->childCount())));
        }
        ensureTypeChildrenLoaded(item);
        if (item->checkState(0) == Qt::Checked) {
            syncingTreeChecks_ = true;
            for (int i = 0; i < item->childCount(); ++i) {
                item->child(i)->setCheckState(0, Qt::Checked);
            }
            syncingTreeChecks_ = false;
        }
        if (category == QStringLiteral("图片") || label == QStringLiteral("图片")) {
            LOG_INFO(std::wstring(L"RecoveryScanPage::onTreeItemExpanded"
                                  L" tree=[type]") +
                     L" category=[" + std::wstring(reinterpret_cast<const wchar_t *>(category.utf16()),
                                                  static_cast<size_t>(category.size())) + L"]" +
                     L" sub=[" + std::wstring(reinterpret_cast<const wchar_t *>(subLabel.utf16()),
                                             static_cast<size_t>(subLabel.size())) + L"]" +
                     L" label=[" + std::wstring(reinterpret_cast<const wchar_t *>(label.utf16()),
                                               static_cast<size_t>(label.size())) + L"]" +
                     L" childCountAfter=" + std::to_wstring(static_cast<uint64_t>(item->childCount())));
        }
    }
}

void RecoveryScanPage::onTreeItemCollapsed(QTreeWidgetItem *item)
{
    if (!item || item->treeWidget() != typeTree_) {
        return;
    }
    const QString category = item->data(0, Qt::UserRole + 6).toString();
    const QString subLabel = item->data(0, Qt::UserRole + 7).toString();
    const QString label = baseTreeLabel(item);
    if (category == QStringLiteral("图片") || label == QStringLiteral("图片")) {
        LOG_INFO(std::wstring(L"RecoveryScanPage::onTreeItemCollapsed"
                              L" tree=[type]") +
                 L" category=[" + std::wstring(reinterpret_cast<const wchar_t *>(category.utf16()),
                                              static_cast<size_t>(category.size())) + L"]" +
                 L" sub=[" + std::wstring(reinterpret_cast<const wchar_t *>(subLabel.utf16()),
                                         static_cast<size_t>(subLabel.size())) + L"]" +
                 L" label=[" + std::wstring(reinterpret_cast<const wchar_t *>(label.utf16()),
                                           static_cast<size_t>(label.size())) + L"]" +
                 L" childCount=" + std::to_wstring(static_cast<uint64_t>(item->childCount())));
    }
}

void RecoveryScanPage::onResultViewportChanged()
{
    ensureVisibleResultRowsLoaded(false);
}

void RecoveryScanPage::refreshResultsHeader()
{
    if (!resultsMode_) {
        return;
    }

    QString title = QStringLiteral("扫描结果");
    if (leftTabs_ && leftTabs_->currentIndex() == 0) {
        QTreeWidgetItem *item = pathTree_ ? pathTree_->currentItem() : nullptr;
        if (item) {
            const QString rel = itemRelativeFolderPath(item);
            title = rel.isEmpty() ? QStringLiteral("本地磁盘(%1)").arg(diskLetter_.toUpper())
                                  : rel;
        }
    } else {
        QTreeWidgetItem *item = typeTree_ ? typeTree_->currentItem() : nullptr;
        if (item) {
            const QString cat = item->data(0, Qt::UserRole + 6).toString();
            const QString sub = item->data(0, Qt::UserRole + 7).toString();
            if (!cat.isEmpty() && !sub.isEmpty()) {
                title = QStringLiteral("%1 / %2").arg(cat, sub);
            } else {
                title = baseTreeLabel(item);
            }
        }
    }
    if (pathLabel_) {
        pathLabel_->setText(title);
    }

    int checkedCount = scanModel_ ? scanModel_->checkedCount() : 0;
    const int visibleCount = previewMode_ && gridView_ ? gridView_->count()
                                                       : (filterModel_ ? filterModel_->rowCount() : 0);
    if (resultsMetaLabel_) {
        resultsMetaLabel_->setText(QStringLiteral("已选：%1 项 | 当前显示：%2 项")
                                       .arg(checkedCount)
                                       .arg(visibleCount));
    }
}

bool RecoveryScanPage::treeItemMatchesSearch(QTreeWidgetItem *item, bool pathTree) const
{
    if (!item) {
        return false;
    }

    const QString keyword = searchKeyword_.trimmed();
    if (keyword.isEmpty()) {
        return false;
    }

    const QString name = baseTreeLabel(item);
    QString type;
    QString path;
    if (pathTree) {
        type = treeItemKind(item) == TreeItemKind::Directory ? QStringLiteral("文件夹") : QStringLiteral("路径");
        const QString relPath = itemRelativeFolderPath(item);
        path = relPath.isEmpty() ? diskLetter_ : (diskLetter_ + QStringLiteral("\\") + relPath);
    } else {
        const QString cat = item->data(0, Qt::UserRole + 6).toString();
        const QString sub = item->data(0, Qt::UserRole + 7).toString();
        type = cat.isEmpty() ? QStringLiteral("类型") : cat;
        path = sub.isEmpty() ? cat : (cat + QStringLiteral("\\") + sub);
    }
    return matchesSearch(name, type, path);
}

QTreeWidgetItem *RecoveryScanPage::findFirstSearchMatch(QTreeWidget *tree, bool pathTree) const
{
    if (!tree || searchKeyword_.trimmed().isEmpty()) {
        return nullptr;
    }

    QList<QTreeWidgetItem *> stack;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        stack.append(tree->topLevelItem(i));
    }

    while (!stack.isEmpty()) {
        QTreeWidgetItem *item = stack.takeFirst();
        if (treeItemMatchesSearch(item, pathTree)) {
            return item;
        }
        for (int i = 0; i < item->childCount(); ++i) {
            stack.append(item->child(i));
        }
    }
    return nullptr;
}

void RecoveryScanPage::refreshTreeSearchHighlight(QTreeWidget *tree)
{
    if (!tree) {
        return;
    }

    const bool isPathTree = (tree == pathTree_);
    const QString keyword = searchKeyword_.trimmed();
    QList<QTreeWidgetItem *> stack;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        stack.append(tree->topLevelItem(i));
    }

    while (!stack.isEmpty()) {
        QTreeWidgetItem *item = stack.takeLast();
        const bool matched = !keyword.isEmpty() && treeItemMatchesSearch(item, isPathTree);
        if (matched) {
            item->setForeground(0, QColor(QStringLiteral("#16a34a")));
            item->setForeground(1, QColor(QStringLiteral("#16a34a")));
            QFont font0 = item->font(0);
            QFont font1 = item->font(1);
            font0.setUnderline(true);
            font1.setUnderline(true);
            item->setFont(0, font0);
            item->setFont(1, font1);
        } else {
            refreshTreeCaption(item);
        }

        if (!keyword.isEmpty() && matched && item->parent()) {
            QTreeWidgetItem *parent = item->parent();
            while (parent) {
                parent->setExpanded(true);
                parent = parent->parent();
            }
        }

        for (int i = 0; i < item->childCount(); ++i) {
            stack.append(item->child(i));
        }
    }
}

void RecoveryScanPage::refreshSearchHighlight()
{
    refreshTreeSearchHighlight(pathTree_);
    refreshTreeSearchHighlight(typeTree_);

    if (searchKeyword_.trimmed().isEmpty()) {
        return;
    }

    if (leftTabs_ && leftTabs_->currentIndex() == 0) {
        QTreeWidgetItem *first = findFirstSearchMatch(pathTree_, true);
        if (first && pathTree_) {
            pathTree_->setCurrentItem(first);
            pathTree_->scrollToItem(first);
        }
    } else {
        QTreeWidgetItem *first = findFirstSearchMatch(typeTree_, false);
        if (first && typeTree_) {
            typeTree_->setCurrentItem(first);
            typeTree_->scrollToItem(first);
        }
    }
}

void RecoveryScanPage::beginScanSession(const QString &locationOption,
                                       const QString &diskLetter)
{
    hideRecoveryCompletePanel();
    useMockData_ = false;
    {
        const QByteArray v = qgetenv("PHOENIX_MOCK_SCAN_COUNT");
        if (!v.isEmpty() && v != "0") {
            useMockData_ = true;
        }
    }
    locationOption_ = locationOption;
    diskLetter_ = diskLetter.trimmed();
    const bool imageSession = QFileInfo(diskLetter_).isFile() ||
                               diskLetter_.endsWith(QStringLiteral(".img"), Qt::CaseInsensitive) ||
                               diskLetter_.endsWith(QStringLiteral(".dd"), Qt::CaseInsensitive) ||
                               diskLetter_.endsWith(QStringLiteral(".raw"), Qt::CaseInsensitive) ||
                               diskLetter_.endsWith(QStringLiteral(".vhd"), Qt::CaseInsensitive) ||
                               diskLetter_.endsWith(QStringLiteral(".vhdx"), Qt::CaseInsensitive) ||
                               diskLetter_.endsWith(QStringLiteral(".vmdk"), Qt::CaseInsensitive) ||
                               diskLetter_.endsWith(QStringLiteral(".qcow2"), Qt::CaseInsensitive) ||
                               diskLetter_.endsWith(QStringLiteral(".vdi"), Qt::CaseInsensitive);
    const bool physicalPartitionSession =
        diskLetter_.startsWith(QStringLiteral("PhysicalDrive"), Qt::CaseInsensitive);
    if (!imageSession && !physicalPartitionSession && !diskLetter_.endsWith(QLatin1Char(':'))) {
        diskLetter_ += QLatin1Char(':');
    }

    const QString letter = diskLetter_.toUpper();
    setResultsMode(false);
    pathLabel_->setText(QStringLiteral("扫描 本地磁盘(%1)").arg(letter));

    if (resultStore_) {
        resultStore_->clear();
    }
    files_.clear();
    typeCategoryItems_.clear();
    typeSubcategoryItems_.clear();
    pathChildDirectories_.clear();
    pathNodeCounts_.clear();
    pathDirectChildIds_.clear();
    typeCategoryChildren_.clear();
    typeNodeCounts_.clear();
    typeNodeFileIds_.clear();
    visibleResults_.clear();
    ++visibleResultsGeneration_;
    visibleTypeCandidateIds_.clear();
    visibleTypeCandidateCursor_ = 0;
    visibleResultsUseTypePaging_ = false;
    visibleResultsContinuationScheduled_ = false;
    visibleResultsLoaded_ = 0;
    pendingStoreRevision_ = 0;
    lastVisibleStoreRevision_ = 0;
    lastPathRefreshKey_.clear();
    lastPathRefreshItemCount_ = -1;
    lastPathRefreshRevision_ = 0;
    lastTypeRefreshKey_.clear();
    lastTypeRefreshItemCount_ = -1;
    lastTypeRefreshRevision_ = 0;
    if (uiPumpTimer_ && uiPumpTimer_->isActive()) {
        uiPumpTimer_->stop();
    }
    pathTree_->clear();
    typeTree_->clear();
    if (scanModel_) {
        scanModel_->clear();
    }
    if (filterModel_) {
        filterModel_->clearAllFilters();
    }
    if (gridView_) {
        gridView_->clear();
    }
    reconstructedRoot_ = nullptr;

    pathRoot_ = new QTreeWidgetItem(QStringList()
                                    << QStringLiteral("□ 本地磁盘(%1)").arg(letter)
                                    << QString());
    pathRoot_->setData(0, Qt::UserRole, QVariant::fromValue<quint64>(0));
    pathRoot_->setData(0, Qt::UserRole + 2, QStringLiteral("本地磁盘(%1)").arg(letter));
    pathRoot_->setData(0, Qt::UserRole + 3, 0);
    pathRoot_->setData(0, Qt::UserRole + 4, static_cast<int>(TreeItemKind::Root));
    pathRoot_->setData(0, Qt::UserRole + 5, QString());
    pathRoot_->setData(0, Qt::UserRole + 9, false);
    enableTreeCheckbox(pathRoot_, false);
    pathTree_->addTopLevelItem(pathRoot_);
    updateTreeItemCount(pathRoot_);
    ensurePathChildrenLoaded(pathRoot_, 4, "beginScanSession");
    suppressTreeExpandLoad_ = true;
    pathRoot_->setExpanded(true);
    suppressTreeExpandLoad_ = false;

    reconstructedRoot_ = new QTreeWidgetItem(QStringList()
                                             << QStringLiteral("重构的文件")
                                             << QString());
    reconstructedRoot_->setData(0, Qt::UserRole, QVariant::fromValue<quint64>(0));
    reconstructedRoot_->setData(0, Qt::UserRole + 2, QStringLiteral("重构的文件"));
    reconstructedRoot_->setData(0, Qt::UserRole + 3, 0);
    reconstructedRoot_->setData(0, Qt::UserRole + 4, static_cast<int>(TreeItemKind::Root));
    reconstructedRoot_->setData(0, Qt::UserRole + 5, QStringLiteral("__reconstructed__"));
    reconstructedRoot_->setData(0, Qt::UserRole + 9, false);
    enableTreeCheckbox(reconstructedRoot_, false);
    reconstructedRoot_->setHidden(true);
    pathTree_->addTopLevelItem(reconstructedRoot_);

    foundTotal_ = 0;
    scanRunning_ = true;
    setResultsMode(false);
    scanElapsedTimer_.restart();
    if (stopScanBtn_) stopScanBtn_->setEnabled(true);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    statusLabel_->setText(QStringLiteral("扫描进行中"));
    setMetricLabelText(metricFoundLabel_, QStringLiteral("0"), QStringLiteral("已找到"));
    setMetricLabelText(metricScannedLabel_, QStringLiteral("0 GB"), QStringLiteral("已扫描"));
    setMetricLabelText(metricTotalLabel_, QStringLiteral("—"), QStringLiteral("总计"));
    setMetricLabelText(metricElapsedLabel_, QStringLiteral("00:00"), QStringLiteral("用时"));
    setMetricLabelText(metricSpeedLabel_, QStringLiteral("0 MB/s"), QStringLiteral("速度"));
    if (scanPercentLabel_) scanPercentLabel_->setText(QStringLiteral("0%"));
    if (scanPhaseLabel_) scanPhaseLabel_->setText(QStringLiteral("正在进行深度扫描"));
    if (scanDescriptionLabel_) {
        scanDescriptionLabel_->setText(QStringLiteral("自动模式正在按顺序检查文件系统、删除记录和文件结构。"));
    }
    if (scanCardProgressBar_) scanCardProgressBar_->setValue(0);
    if (scanProgressTextLabel_) scanProgressTextLabel_->setText(QStringLiteral("已扫描 0 B / —"));
    if (scanEtaLabel_) scanEtaLabel_->setText(QStringLiteral("预计还需 —"));
    if (scanFoundLabel_) scanFoundLabel_->setText(QStringLiteral("0"));
    if (scanScannedLabel_) scanScannedLabel_->setText(QStringLiteral("0 B"));
    if (scanSpeedLabel_) scanSpeedLabel_->setText(QStringLiteral("0 B/s"));
    if (footerFoundLabel_) footerFoundLabel_->setText(QStringLiteral("扫描中，已找到文件：0"));
    if (footerSubLabel_) footerSubLabel_->setText(QStringLiteral("恢复前可继续筛选并勾选需要的文件"));
    if (footerScannedLabel_) footerScannedLabel_->clear();
    if (footerElapsedLabel_) footerElapsedLabel_->clear();
    if (footerSpeedLabel_) footerSpeedLabel_->clear();

    if (useMockData_) {
        loadMockScanData();
        return;
    }

    pathTree_->setCurrentItem(pathRoot_);
    refillTableFromPathItem(pathRoot_);
    onLeftTabChanged(leftTabs_->currentIndex());
}

void RecoveryScanPage::setNonDiskPlaceholder(const QString &locationOption)
{
    hideRecoveryCompletePanel();
    useMockData_ = false;
    setResultsMode(false);
    locationOption_ = locationOption;
    diskLetter_.clear();
    pathLabel_->setText(QStringLiteral("%1").arg(locationOption_));

    if (resultStore_) {
        resultStore_->clear();
    }
    files_.clear();
    typeCategoryItems_.clear();
    typeSubcategoryItems_.clear();
    pathChildDirectories_.clear();
    pathNodeCounts_.clear();
    pathDirectChildIds_.clear();
    typeCategoryChildren_.clear();
    typeNodeCounts_.clear();
    typeNodeFileIds_.clear();
    visibleResults_.clear();
    ++visibleResultsGeneration_;
    visibleTypeCandidateIds_.clear();
    visibleTypeCandidateCursor_ = 0;
    visibleResultsUseTypePaging_ = false;
    visibleResultsContinuationScheduled_ = false;
    visibleResultsLoaded_ = 0;
    pendingStoreRevision_ = 0;
    lastVisibleStoreRevision_ = 0;
    lastPathRefreshKey_.clear();
    lastPathRefreshItemCount_ = -1;
    lastPathRefreshRevision_ = 0;
    lastTypeRefreshKey_.clear();
    lastTypeRefreshItemCount_ = -1;
    lastTypeRefreshRevision_ = 0;
    if (uiPumpTimer_ && uiPumpTimer_->isActive()) {
        uiPumpTimer_->stop();
    }
    pathTree_->clear();
    typeTree_->clear();
    if (scanModel_) {
        scanModel_->clear();
    }
    if (filterModel_) {
        filterModel_->clearAllFilters();
    }
    if (gridView_) {
        gridView_->clear();
    }
    pathRoot_ = nullptr;
    reconstructedRoot_ = nullptr;

    scanRunning_ = false;
    if (stopScanBtn_) stopScanBtn_->setEnabled(false);
    progressBar_->setRange(0, 100);
    progressBar_->setValue(0);
    statusLabel_->setText(QStringLiteral("当前版本仅支持从本地磁盘进入扫描"));
    setMetricLabelText(metricFoundLabel_, QStringLiteral("0"), QStringLiteral("已找到"));
    setMetricLabelText(metricScannedLabel_, QStringLiteral("0 GB"), QStringLiteral("已扫描"));
    setMetricLabelText(metricTotalLabel_, QStringLiteral("—"), QStringLiteral("总计"));
    setMetricLabelText(metricElapsedLabel_, QStringLiteral("00:00"), QStringLiteral("用时"));
    setMetricLabelText(metricSpeedLabel_, QStringLiteral("0 MB/s"), QStringLiteral("速度"));
}

void RecoveryScanPage::appendScanFileBatch(const QVector<ScanFileDto> &batch)
{
    if (scanModel_ && !batch.isEmpty()) {
        scanModel_->appendBatch(batch);
    }
}

void RecoveryScanPage::onScanIndexUpdated(quint64 revision)
{
    if (useMockData_ || !resultStore_) {
        return;
    }
    if (scanModel_) {
        scanModel_->onStoreRevisionChanged(revision);
    }
    pendingStoreRevision_ = (std::max)(pendingStoreRevision_, revision);
    if (!uiPumpTimer_->isActive()) {
        uiPumpTimer_->start();
    }
}

void RecoveryScanPage::processUiPump()
{
    if (useMockData_ || !resultStore_ || pendingStoreRevision_ <= lastVisibleStoreRevision_) {
        if (uiPumpTimer_->isActive()) {
            uiPumpTimer_->stop();
        }
        return;
    }

    if (pathTree_ && !pathTree_->currentItem() && pathRoot_) {
        pathTree_->setCurrentItem(pathRoot_);
    }
    if (leftTabs_ && leftTabs_->currentIndex() == 1 &&
        typeTree_ && !typeTree_->currentItem() && typeTree_->topLevelItemCount() > 0) {
        typeTree_->setCurrentItem(typeTree_->topLevelItem(0));
    }

    const QString currentPathRel = (pathTree_ && pathTree_->currentItem())
                                       ? itemRelativeFolderPath(pathTree_->currentItem())
                                       : QString();
    const QString currentTypeCat = (typeTree_ && typeTree_->currentItem())
                                       ? typeTree_->currentItem()->data(0, Qt::UserRole + 6).toString()
                                       : QString();
    const QString currentTypeSub = (typeTree_ && typeTree_->currentItem())
                                       ? typeTree_->currentItem()->data(0, Qt::UserRole + 7).toString()
                                       : QString();
    const QString currentTypeKey = currentTypeSub.isEmpty() ? currentTypeCat
                                                            : (currentTypeCat + QStringLiteral("::") + currentTypeSub);
    const int rootDirCount = resultStore_->pathChildDirectories(QString()).size();
    const int rootItemCount = resultStore_->pathDirectChildIds(QString()).size();
    const int rootTreeChildCount = pathRoot_ ? pathRoot_->childCount() : 0;
    const int currentPathItemCount = resultStore_->pathDirectChildIds(currentPathRel).size();
    const int currentTypeItemCount = currentTypeKey.isEmpty() ? 0 : resultStore_->typeNodeFileIds(currentTypeKey).size();
    const bool pathSelectionActive = !leftTabs_ || leftTabs_->currentIndex() == 0;
    const bool typeSelectionActive = leftTabs_ && leftTabs_->currentIndex() == 1;
    const bool rootNeedsAppend = pathRoot_ && rootTreeChildCount < rootDirCount;
    bool currentNeedsAppend = false;
    if (pathTree_ && pathTree_->currentItem() && pathTree_->currentItem()->isExpanded()) {
        const QStringList currentChildDirs = resultStore_->pathChildDirectories(currentPathRel);
        currentNeedsAppend = pathTree_->currentItem()->childCount() < currentChildDirs.size();
    }

    if (!scanBatchLogTimer_.isValid() || scanBatchLogTimer_.elapsed() >= 2000) {
        LOG_INFO(L"RecoveryScanPage::processUiPump"
                 L" pendingRevision=" + std::to_wstring(static_cast<uint64_t>(pendingStoreRevision_)) +
                 L" visibleRevision=" + std::to_wstring(static_cast<uint64_t>(lastVisibleStoreRevision_)) +
                 L" rootDirs=" + std::to_wstring(static_cast<uint64_t>(rootDirCount)) +
                 L" rootItems=" + std::to_wstring(static_cast<uint64_t>(rootItemCount)) +
                 L" rootTreeChildren=" + std::to_wstring(static_cast<uint64_t>(rootTreeChildCount)) +
                 L" currentPath=[" + std::wstring(reinterpret_cast<const wchar_t *>(currentPathRel.utf16()),
                                                  static_cast<size_t>(currentPathRel.size())) + L"]" +
                 L" currentPathItems=" + std::to_wstring(static_cast<uint64_t>(currentPathItemCount)) +
                 L" currentType=[" + std::wstring(reinterpret_cast<const wchar_t *>(currentTypeKey.utf16()),
                                                  static_cast<size_t>(currentTypeKey.size())) + L"]" +
                 L" currentTypeItems=" + std::to_wstring(static_cast<uint64_t>(currentTypeItemCount)) +
                 L" rootNeedsAppend=" + std::wstring(rootNeedsAppend ? L"yes" : L"no") +
                 L" currentNeedsAppend=" + std::wstring(currentNeedsAppend ? L"yes" : L"no"));
        scanBatchLogTimer_.restart();
    }

    if (!rootNeedsAppend &&
        !currentNeedsAppend &&
        pendingStoreRevision_ == lastVisibleStoreRevision_ + 1 &&
        !typeSelectionActive) {
        LOG_INFO(L"RecoveryScanPage::processUiPump skip no-op"
                 L" revision=" + std::to_wstring(static_cast<uint64_t>(pendingStoreRevision_)) +
                 L" currentPath=[" + std::wstring(reinterpret_cast<const wchar_t *>(currentPathRel.utf16()),
                                                  static_cast<size_t>(currentPathRel.size())) + L"]");
        lastVisibleStoreRevision_ = pendingStoreRevision_;
        return;
    }

    batchingUiUpdate_ = true;
    int appendedRoot = 0;
    if (pathRoot_) {
        updateTreeItemCount(pathRoot_);
        if (rootNeedsAppend) {
            appendedRoot = ensurePathChildrenLoaded(pathRoot_, 2, "processUiPump.root");
        }
        suppressTreeExpandLoad_ = true;
        pathRoot_->setExpanded(true);
        suppressTreeExpandLoad_ = false;
    }
    int appendedCurrentPath = 0;
    if (pathTree_ && pathTree_->currentItem() && pathTree_->currentItem()->isExpanded()) {
        if (currentNeedsAppend) {
            appendedCurrentPath = ensurePathChildrenLoaded(pathTree_->currentItem(),
                                                          pathTree_->currentItem() == pathRoot_ ? 0 : 1,
                                                          "processUiPump.current");
        }
        updateTreeItemCount(pathTree_->currentItem());
    } else if (pathTree_ && pathTree_->currentItem()) {
        updateTreeItemCount(pathTree_->currentItem());
    }

    const QVector<QString> categories = resultStore_->typeCategories();
    for (int i = 0; i < categories.size(); ++i) {
        const QString &category = categories.at(i);
        if (!typeCategoryItems_.contains(category) && typeTree_) {
            QTreeWidgetItem *catItem = createTypeCategoryItem(category);
            typeTree_->insertTopLevelItem(desiredTypeCategoryInsertIndex(typeTree_, category), catItem);
            typeCategoryItems_.insert(category, catItem);
        }
        updateTreeItemCount(typeCategoryItems_.value(category));
    }
    if (typeTree_ && typeTree_->currentItem() && typeTree_->currentItem()->isExpanded()) {
        ensureTypeChildrenLoaded(typeTree_->currentItem());
        updateTreeItemCount(typeTree_->currentItem());
    } else if (typeTree_ && typeTree_->currentItem()) {
        updateTreeItemCount(typeTree_->currentItem());
    }

    bool shouldRefill = false;
    if (pathSelectionActive) {
        if ((appendedRoot > 0 || appendedCurrentPath > 0) ||
            lastPathRefreshKey_ != currentPathRel || lastPathRefreshItemCount_ != currentPathItemCount) {
            shouldRefill = true;
            lastPathRefreshKey_ = currentPathRel;
            lastPathRefreshItemCount_ = currentPathItemCount;
        }
    } else if (typeSelectionActive) {
        if (lastTypeRefreshKey_ != currentTypeKey) {
            LOG_INFO(L"RecoveryScanPage::processUiPump schedule type refill by key change"
                     L" key=[" + std::wstring(reinterpret_cast<const wchar_t *>(currentTypeKey.utf16()),
                                             static_cast<size_t>(currentTypeKey.size())) + L"]");
            shouldRefill = true;
            lastTypeRefreshKey_ = currentTypeKey;
            lastTypeRefreshItemCount_ = currentTypeItemCount;
        }
    }
    if (shouldRefill) {
        refillTableFromSelection();
    }
    if (!searchKeyword_.trimmed().isEmpty()) {
        refreshSearchHighlight();
    }
    flushDirtyTreeCaptions();
    batchingUiUpdate_ = false;
    lastVisibleStoreRevision_ = pendingStoreRevision_;
}

void RecoveryScanPage::setScanProgress(int percent,
                                       quint32 foundCount,
                                       quint64 scannedBytes,
                                       quint64 totalBytes,
                                       quint64 elapsedMs,
                                       const QString &phaseText)
{
    if (useMockData_) {
        return;
    }
    const int bytePercent = (totalBytes > 0)
                                ? static_cast<int>(qBound<quint64>(0, (scannedBytes * 100) / totalBytes, 100))
                                : percent;
    int p = qBound(0, (std::max)(percent, bytePercent), 100);
    if (scanRunning_ && scannedBytes > 0 && totalBytes > 0 && p == 0) {
        p = 1;
    }
    const bool percentChanged = (p != lastProgressUiPercent_);
    const bool foundChanged = (foundCount != lastProgressUiFound_);
    const bool scannedAdvanced = (scannedBytes > lastProgressUiScanned_);
    const bool totalChanged = (totalBytes != lastProgressUiTotal_);
    const bool intervalReached = !progressUiThrottleTimer_.isValid() || progressUiThrottleTimer_.elapsed() >= 120;
    if (!percentChanged && !foundChanged && !totalChanged && !intervalReached && !scannedAdvanced) {
        return;
    }

    lastProgressUiPercent_ = p;
    lastProgressUiFound_ = foundCount;
    lastProgressUiScanned_ = scannedBytes;
    lastProgressUiTotal_ = totalBytes;
    lastProgressUiElapsedMs_ = elapsedMs;
    progressUiThrottleTimer_.restart();
    LOG_INFO(L"RecoveryScanPage::setScanProgress"
             L" percent=" + std::to_wstring(static_cast<uint64_t>(percent)) +
             L" bytePercent=" + std::to_wstring(static_cast<uint64_t>(bytePercent)) +
             L" uiPercent=" + std::to_wstring(static_cast<uint64_t>(p)) +
             L" found=" + std::to_wstring(static_cast<uint64_t>(foundCount)) +
             L" scannedBytes=" + std::to_wstring(scannedBytes) +
             L" totalBytes=" + std::to_wstring(totalBytes) +
             L" elapsedMs=" + std::to_wstring(elapsedMs));

    setResultsMode(false);
    foundTotal_ = foundCount;
    scannedBytes_ = scannedBytes;
    totalBytes_ = totalBytes;
    if (!scanElapsedTimer_.isValid()) {
        scanElapsedTimer_.start();
    }
    const qint64 effectiveElapsedMs = static_cast<qint64>(elapsedMs);
    const QString elapsedText = formatElapsedCompact(effectiveElapsedMs);
    const double elapsedSeconds = qMax(0.001, static_cast<double>(effectiveElapsedMs) / 1000.0);
    const quint64 bytesPerSecond = static_cast<quint64>(static_cast<double>(scannedBytes) / elapsedSeconds);
    const QString speedText = bytesPerSecond > 0
                                  ? (formatSize(static_cast<qint64>(bytesPerSecond)) + QStringLiteral("/s"))
                                  : QStringLiteral("0 B/s");
    progressBar_->setValue(p);
    const QString phase = phaseText.trimmed().isEmpty() ? QStringLiteral("扫描进行中") : phaseText.trimmed();
    statusLabel_->setText(QStringLiteral("扫描进行中 · %1").arg(phase));
    if (scanPercentLabel_) {
        scanPercentLabel_->setText(QStringLiteral("%1%").arg(p));
    }
    if (scanCardProgressBar_) {
        scanCardProgressBar_->setValue(p);
    }
    if (scanPhaseLabel_) {
        scanPhaseLabel_->setText(phase);
    }
    if (scanDescriptionLabel_) {
        scanDescriptionLabel_->setText(QStringLiteral("扫描结果会持续保存，可以随时停止并查看已发现文件。"));
    }
    if (scanProgressTextLabel_) {
        scanProgressTextLabel_->setText(QStringLiteral("已扫描 %1 / %2")
                                             .arg(formatSize(static_cast<qint64>(scannedBytes)))
                                             .arg(totalBytes > 0 ? formatSize(static_cast<qint64>(totalBytes))
                                                                  : QStringLiteral("—")));
    }
    if (scanEtaLabel_) {
        QString eta = QStringLiteral("预计还需 —");
        if (p > 0 && p < 100 && effectiveElapsedMs > 0) {
            const qint64 remainingMs = static_cast<qint64>(
                (static_cast<double>(effectiveElapsedMs) * (100 - p)) / p);
            const qint64 remainingMinutes = qMax<qint64>(1, (remainingMs + 59999) / 60000);
            eta = QStringLiteral("预计还需 %1 分钟").arg(remainingMinutes);
        }
        scanEtaLabel_->setText(eta);
    }
    if (scanFoundLabel_) scanFoundLabel_->setText(QLocale::system().toString(foundCount));
    if (scanScannedLabel_) scanScannedLabel_->setText(formatSize(static_cast<qint64>(scannedBytes)));
    if (scanSpeedLabel_) scanSpeedLabel_->setText(speedText);
    setMetricLabelText(metricFoundLabel_, QString::number(foundCount), QStringLiteral("已找到"));
    setMetricLabelText(metricScannedLabel_, formatSize(scannedBytes), QStringLiteral("已扫描"));
    setMetricLabelText(metricTotalLabel_, totalBytes > 0 ? formatSize(totalBytes) : QStringLiteral("—"), QStringLiteral("总计"));
    if (metricElapsedLabel_) {
        setMetricLabelText(metricElapsedLabel_, elapsedText, QStringLiteral("用时"));
    }
    if (metricSpeedLabel_) {
        setMetricLabelText(metricSpeedLabel_, speedText, QStringLiteral("速度"));
    }
    if (footerFoundLabel_) {
        footerFoundLabel_->setText(QStringLiteral("扫描中，已找到文件：%1").arg(static_cast<qulonglong>(foundCount)));
    }
    if (footerScannedLabel_) {
        footerScannedLabel_->setText(QStringLiteral("已扫描 %1 / %2")
                                         .arg(formatSize(scannedBytes))
                                         .arg(totalBytes > 0 ? formatSize(totalBytes) : QStringLiteral("—")));
    }
    if (footerElapsedLabel_) footerElapsedLabel_->setText(QStringLiteral("用时 %1").arg(elapsedText));
    if (footerSpeedLabel_) footerSpeedLabel_->setText(QStringLiteral("速度 %1").arg(speedText));
}

void RecoveryScanPage::setScanFinished(bool ok, const QString &message)
{
    scanRunning_ = false;
    if (stopScanBtn_) stopScanBtn_->setEnabled(false);
    progressBar_->setValue(ok ? 100 : progressBar_->value());
    statusLabel_->setText(message);
    if (footerFoundLabel_) {
        footerFoundLabel_->setText(QStringLiteral("扫描完成，找到文件：%1").arg(static_cast<qulonglong>(foundTotal_)));
    }
    if (footerScannedLabel_) {
        footerScannedLabel_->setText(QStringLiteral("已扫描 %1 / %2")
                                         .arg(formatSize(scannedBytes_))
                                         .arg(totalBytes_ > 0 ? formatSize(totalBytes_) : QStringLiteral("—")));
    }
    const QString elapsedText = lastProgressUiElapsedMs_ > 0
                                    ? formatElapsedCompact(static_cast<qint64>(lastProgressUiElapsedMs_))
                                    : QStringLiteral("—");
    const double elapsedSeconds = qMax(0.001, static_cast<double>(lastProgressUiElapsedMs_) / 1000.0);
    const quint64 bytesPerSecond =
        (lastProgressUiElapsedMs_ > 0)
            ? static_cast<quint64>(static_cast<double>(scannedBytes_) / elapsedSeconds)
            : 0;
    const QString speedText = bytesPerSecond > 0
                                  ? (formatSize(static_cast<qint64>(bytesPerSecond)) + QStringLiteral("/s"))
                                  : QStringLiteral("—");
    if (metricElapsedLabel_) {
        setMetricLabelText(metricElapsedLabel_, elapsedText, QStringLiteral("用时"));
    }
    if (metricSpeedLabel_) {
        setMetricLabelText(metricSpeedLabel_, speedText, QStringLiteral("速度"));
    }
    if (footerElapsedLabel_) footerElapsedLabel_->setText(QStringLiteral("用时 %1").arg(elapsedText));
    if (footerSpeedLabel_) footerSpeedLabel_->setText(QStringLiteral("速度 %1").arg(speedText));

    if (!useMockData_ && pathRoot_) {
        rebuildTreeViews();
        // rebuildTreeViews() 内部已尝试恢复扫描过程中的 currentPathRel/currentType。
        // 这里不要强行回退到 pathRoot_，否则会导致“扫描完成后路径项与扫描中不一致”。
        if (pathTree_ && !pathTree_->currentItem()) {
            pathTree_->setCurrentItem(pathRoot_);
        }
    }

    if (leftTabs_->currentIndex() == 0 && pathTree_->currentItem()) {
        refillTableFromPathItem(pathTree_->currentItem());
    } else if (leftTabs_->currentIndex() == 1 && typeTree_->currentItem()) {
        refillTableFromTypeItem(typeTree_->currentItem());
    }
    if (tableView_ && filterModel_ && filterModel_->rowCount() > 0) {
        tableView_->selectRow(0);
    }
    pathLabel_->setText(QStringLiteral("扫描结果"));
    setResultsMode(true);
}

void RecoveryScanPage::rebuildTreeViews()
{
    if (!pathTree_ || !typeTree_) {
        return;
    }

    // rebuildTreeViews() 会清空并重建 pathTree_/typeTree_。
    // 为了避免“扫描过程中展开了 N 个子项，扫描完成后却变回固定少量”的观感差异，
    // 这里在清空之前先记录当前 UI 已经加载/展开的子节点数量，并在重建后按同样数量补齐。
    const int oldLoadedPathRootChildren = pathRoot_ ? pathRoot_->childCount() : 0;
    const int oldLoadedCurrentPathChildren =
        (pathTree_ && pathTree_->currentItem()) ? pathTree_->currentItem()->childCount() : 0;

    const QString currentPathRel = pathTree_ && pathTree_->currentItem()
                                       ? itemRelativeFolderPath(pathTree_->currentItem())
                                       : QString();
    const QString currentTypeCat = typeTree_ && typeTree_->currentItem()
                                       ? typeTree_->currentItem()->data(0, Qt::UserRole + 6).toString()
                                       : QString();
    const QString currentTypeSub = typeTree_ && typeTree_->currentItem()
                                       ? typeTree_->currentItem()->data(0, Qt::UserRole + 7).toString()
                                       : QString();
    const QStringList checkedPathKeys = checkedPathTreeItems();
    const QStringList checkedTypeKeys = checkedTypeTreeItems();

    pathTree_->clear();
    typeTree_->clear();
    typeCategoryItems_.clear();
    typeSubcategoryItems_.clear();
    reconstructedRoot_ = nullptr;

    const QString letter = diskLetter_.toUpper();
    pathRoot_ = new QTreeWidgetItem(QStringList()
                                    << QStringLiteral("□ 本地磁盘(%1)").arg(letter)
                                    << QString());
    pathRoot_->setData(0, Qt::UserRole, QVariant::fromValue<quint64>(0));
    pathRoot_->setData(0, Qt::UserRole + 2, QStringLiteral("本地磁盘(%1)").arg(letter));
    pathRoot_->setData(0, Qt::UserRole + 3, 0);
    pathRoot_->setData(0, Qt::UserRole + 4, static_cast<int>(TreeItemKind::Root));
    pathRoot_->setData(0, Qt::UserRole + 5, QString());
    pathRoot_->setData(0, Qt::UserRole + 9, false);
    enableTreeCheckbox(pathRoot_, false);
    pathTree_->addTopLevelItem(pathRoot_);
    updateTreeItemCount(pathRoot_);
    ensurePathChildrenLoaded(pathRoot_, oldLoadedPathRootChildren, "rebuildTreeViews");
    suppressTreeExpandLoad_ = true;
    pathRoot_->setExpanded(true);
    suppressTreeExpandLoad_ = false;

    reconstructedRoot_ = new QTreeWidgetItem(QStringList()
                                             << QStringLiteral("重构的文件")
                                             << QString());
    reconstructedRoot_->setData(0, Qt::UserRole, QVariant::fromValue<quint64>(0));
    reconstructedRoot_->setData(0, Qt::UserRole + 2, QStringLiteral("重构的文件"));
    reconstructedRoot_->setData(0, Qt::UserRole + 3, 0);
    reconstructedRoot_->setData(0, Qt::UserRole + 4, static_cast<int>(TreeItemKind::Root));
    reconstructedRoot_->setData(0, Qt::UserRole + 5, QStringLiteral("__reconstructed__"));
    reconstructedRoot_->setData(0, Qt::UserRole + 9, false);
    enableTreeCheckbox(reconstructedRoot_, false);
    reconstructedRoot_->setHidden(true);
    pathTree_->addTopLevelItem(reconstructedRoot_);
    const QVector<QString> categories = resultStore_ ? resultStore_->typeCategories() : QVector<QString>();
    for (int i = 0; i < categories.size(); ++i) {
        const QString &category = categories.at(i);
        QTreeWidgetItem *catItem = createTypeCategoryItem(category);
        typeTree_->insertTopLevelItem(desiredTypeCategoryInsertIndex(typeTree_, category), catItem);
        typeCategoryItems_.insert(category, catItem);
        updateTreeItemCount(catItem);
    }

    // rebuildTreeViews() 只会预加载较少的 path children，需要沿 currentPathRel 补齐到目标节点，
    // 否则完成后当前路径会退回到根目录，导致展示结果集合变化。
    QTreeWidgetItem *pathItem = ensurePathTreeItemByRelativePath(currentPathRel);
    if (!pathItem) {
        pathItem = pathRoot_;
    }
    pathTree_->setCurrentItem(pathItem);
    // 仅当当前节点不是 root 时，才额外补齐当前节点的子项数量；
    // root 的子项数量在上面已经按 oldLoadedPathRootChildren 加载过了。
    if (pathItem && pathItem != pathRoot_) {
        ensurePathChildrenLoaded(pathItem, oldLoadedCurrentPathChildren, "rebuildTreeViews.currentPathItem");
        updateTreeItemCount(pathItem);
    }

    QTreeWidgetItem *typeItem = nullptr;
    if (!currentTypeCat.isEmpty()) {
        QTreeWidgetItem *catItem = typeCategoryItems_.value(currentTypeCat, nullptr);
        if (catItem && !currentTypeSub.isEmpty()) {
            // 子节点恢复前先把动态 children 补齐到 typeSubcategoryItems_，避免类型恢复失败。
            ensureTypeChildrenLoaded(catItem);
        }
        if (!currentTypeSub.isEmpty()) {
            typeItem = typeSubcategoryItems_.value(currentTypeCat + QStringLiteral("::") + currentTypeSub, nullptr);
        }
        if (!typeItem) {
            typeItem = catItem;
        }
    }
    if (!typeItem && typeTree_->topLevelItemCount() > 0) {
        typeItem = typeTree_->topLevelItem(0);
    }
    if (typeItem) {
        typeTree_->setCurrentItem(typeItem);
    }
    restoreTreeChecks(pathTree_, checkedPathKeys, true);
    restoreTreeChecks(typeTree_, checkedTypeKeys, false);
    syncTableChecksFromTreeSelection();
}

QStringList RecoveryScanPage::checkedPathTreeItems() const
{
    QStringList result;
    if (!pathTree_) {
        return result;
    }

    QList<QTreeWidgetItem *> stack;
    for (int i = 0; i < pathTree_->topLevelItemCount(); ++i) {
        stack.append(pathTree_->topLevelItem(i));
    }
    while (!stack.isEmpty()) {
        QTreeWidgetItem *item = stack.takeLast();
        if (item->checkState(0) == Qt::Checked) {
            result.append(itemRelativeFolderPath(item));
        }
        for (int i = 0; i < item->childCount(); ++i) {
            stack.append(item->child(i));
        }
    }
    return result;
}

QStringList RecoveryScanPage::checkedTypeTreeItems() const
{
    QStringList result;
    if (!typeTree_) {
        return result;
    }

    QList<QTreeWidgetItem *> stack;
    for (int i = 0; i < typeTree_->topLevelItemCount(); ++i) {
        stack.append(typeTree_->topLevelItem(i));
    }
    while (!stack.isEmpty()) {
        QTreeWidgetItem *item = stack.takeLast();
        if (item->checkState(0) == Qt::Checked) {
            const QString cat = item->data(0, Qt::UserRole + 6).toString();
            const QString sub = item->data(0, Qt::UserRole + 7).toString();
            // 类别节点(sub 为空)不带 "::"。
            result.append(cat.isEmpty() ? baseTreeLabel(item) : (sub.isEmpty() ? cat : (cat + QStringLiteral("::") + sub)));
        }
        for (int i = 0; i < item->childCount(); ++i) {
            stack.append(item->child(i));
        }
    }
    return result;
}

void RecoveryScanPage::restoreTreeChecks(QTreeWidget *tree, const QStringList &keys, bool pathTree)
{
    if (!tree || keys.isEmpty()) {
        return;
    }

    syncingTreeChecks_ = true;
    if (pathTree) {
        for (const QString &key : keys) {
            QTreeWidgetItem *item = ensurePathTreeItemByRelativePath(key);
            if (!item) {
                continue;
            }
            item->setCheckState(0, Qt::Checked);
            updateParentCheckState(item);
        }
    } else {
        // typeTree 在 rebuild 后只会保留固定子节点，需要按 checked key 补齐子节点，
        // 否则 checkedTypeKeys_ 的恢复会不完整（进而影响 table 加载的集合与勾选态）。
        for (const QString &key : keys) {
            const int sep = key.indexOf(QStringLiteral("::"));
            const QString cat = (sep >= 0) ? key.left(sep) : key;
            const QString sub = (sep >= 0) ? key.mid(sep + 2) : QString();
            if (cat.isEmpty()) {
                continue;
            }

            QTreeWidgetItem *catItem = typeCategoryItems_.value(cat, nullptr);
            if (catItem && sub.isEmpty()) {
                // 类别节点被勾选时，补齐其子节点，保证 setTreeSubtreeCheckState 能正确递归。
                ensureTypeChildrenLoaded(catItem);
            }

            QTreeWidgetItem *item = ensureTypeTreeItemByKey(cat, sub);
            if (!item) {
                continue;
            }
            item->setCheckState(0, Qt::Checked);
            setTreeSubtreeCheckState(item, Qt::Checked);
            updateParentCheckState(item);
        }
    }
    QList<QTreeWidgetItem *> stack;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        stack.append(tree->topLevelItem(i));
    }
    while (!stack.isEmpty()) {
        QTreeWidgetItem *item = stack.takeLast();
        QString key;
        if (pathTree) {
            key = itemRelativeFolderPath(item);
        } else {
            const QString cat = item->data(0, Qt::UserRole + 6).toString();
            const QString sub = item->data(0, Qt::UserRole + 7).toString();
            key = cat.isEmpty()
                      ? baseTreeLabel(item)
                      : (sub.isEmpty() ? cat : (cat + QStringLiteral("::") + sub));
        }

        if (keys.contains(key)) {
            item->setCheckState(0, Qt::Checked);
            setTreeSubtreeCheckState(item, Qt::Checked);
            updateParentCheckState(item);
        }
        for (int i = 0; i < item->childCount(); ++i) {
            stack.append(item->child(i));
        }
    }
    syncingTreeChecks_ = false;
}

void RecoveryScanPage::loadMockScanData()
{
    struct MockEntry {
        QString name;
        QString fullPath;
        QString ext;
        qint64 sizeBytes;
        bool isDirectory;
    };

    QVector<MockEntry> mockEntries = {
        {QStringLiteral("用户"),              QStringLiteral("C:\\用户"),                                  QString(),      0,          true},
        {QStringLiteral("桌面"),              QStringLiteral("C:\\用户\\桌面"),                            QString(),      0,          true},
        {QStringLiteral("文档"),              QStringLiteral("C:\\用户\\文档"),                            QString(),      0,          true},
        {QStringLiteral("下载"),              QStringLiteral("C:\\用户\\下载"),                            QString(),      0,          true},
        {QStringLiteral("项目资料"),          QStringLiteral("C:\\用户\\桌面\\项目资料"),                  QString(),      0,          true},
        {QStringLiteral("设计稿"),            QStringLiteral("C:\\用户\\桌面\\项目资料\\设计稿"),          QString(),      0,          true},
        {QStringLiteral("阶段一"),            QStringLiteral("C:\\用户\\桌面\\项目资料\\设计稿\\阶段一"),  QString(),      0,          true},
        {QStringLiteral("阶段二"),            QStringLiteral("C:\\用户\\桌面\\项目资料\\设计稿\\阶段二"),  QString(),      0,          true},
        {QStringLiteral("导出"),              QStringLiteral("C:\\用户\\桌面\\项目资料\\导出"),            QString(),      0,          true},
        {QStringLiteral("需求说明.docx"),     QStringLiteral("C:\\用户\\桌面\\项目资料\\需求说明.docx"),   QStringLiteral("docx"),  1850368,   false},
        {QStringLiteral("线框图.fig"),        QStringLiteral("C:\\用户\\桌面\\项目资料\\设计稿\\阶段一\\线框图.fig"), QStringLiteral("fig"), 532480, false},
        {QStringLiteral("界面草图.png"),      QStringLiteral("C:\\用户\\桌面\\项目资料\\界面草图.png"),    QStringLiteral("png"),   826432,    false},
        {QStringLiteral("导出视频.mp4"),      QStringLiteral("C:\\用户\\桌面\\项目资料\\导出\\导出视频.mp4"),QStringLiteral("mp4"), 254803968, false},
        {QStringLiteral("回收文件"),          QStringLiteral("C:\\恢复数据\\回收文件"),                    QString(),      0,          true},
        {QStringLiteral("文档"),              QStringLiteral("C:\\恢复数据\\文档"),                        QString(),      0,          true},
        {QStringLiteral("扫描报告.pdf"),      QStringLiteral("C:\\恢复数据\\回收文件\\扫描报告.pdf"),      QStringLiteral("pdf"),   2367488,   false},
        {QStringLiteral("压缩备份.zip"),      QStringLiteral("C:\\恢复数据\\回收文件\\压缩备份.zip"),      QStringLiteral("zip"),   87428352,  false},
        {QStringLiteral("图片"),              QStringLiteral("C:\\恢复数据\\图片"),                        QString(),      0,          true},
        {QStringLiteral("旅行照片001.jpg"),   QStringLiteral("C:\\恢复数据\\图片\\旅行照片001.jpg"),       QStringLiteral("jpg"),   3412992,   false},
        {QStringLiteral("旅行照片002.jpg"),   QStringLiteral("C:\\恢复数据\\图片\\旅行照片002.jpg"),       QStringLiteral("jpg"),   2985984,   false},
        {QStringLiteral("截图_恢复.webp"),    QStringLiteral("C:\\恢复数据\\图片\\截图_恢复.webp"),        QStringLiteral("webp"),  512384,    false},
        {QStringLiteral("音频"),              QStringLiteral("C:\\恢复数据\\音频"),                        QString(),      0,          true},
        {QStringLiteral("会议录音.m4a"),      QStringLiteral("C:\\恢复数据\\音频\\会议录音.m4a"),          QStringLiteral("m4a"),   18579456,  false},
        {QStringLiteral("收藏"),              QStringLiteral("C:\\恢复数据\\收藏"),                        QString(),      0,          true},
        {QStringLiteral("视频"),              QStringLiteral("C:\\恢复数据\\视频"),                        QString(),      0,          true},
        {QStringLiteral("临时目录"),          QStringLiteral("C:\\恢复数据\\视频\\临时目录"),              QString(),      0,          true},
        {QStringLiteral("bookmarks.html"),    QStringLiteral("C:\\恢复数据\\收藏\\bookmarks.html"),      QStringLiteral("html"),  248576,    false},
        {QStringLiteral("未命名文件.txt"),    QStringLiteral("C:\\恢复数据\\收藏\\未命名文件.txt"),        QStringLiteral("txt"),   4096,      false},
        {QStringLiteral("恢复片段.mov"),      QStringLiteral("C:\\恢复数据\\视频\\临时目录\\恢复片段.mov"), QStringLiteral("mov"),  73400320,  false},
    };

    const int targetCount = qMax(0, QString::fromLocal8Bit(qgetenv("PHOENIX_MOCK_SCAN_COUNT")).toInt());
    if (targetCount > 0) {
        const QString root = QStringLiteral("C:\\Mock");
        const int folderCount = qMax(10, qMin(500, targetCount / 200));
        const QStringList exts = {
            QStringLiteral("jpg"), QStringLiteral("png"), QStringLiteral("pdf"), QStringLiteral("zip"),
            QStringLiteral("mp4"), QStringLiteral("docx"), QStringLiteral("xlsx"), QStringLiteral("pptx"),
            QStringLiteral("txt"), QStringLiteral("json"), QStringLiteral("mov"), QStringLiteral("webp")
        };
        mockEntries.push_back({QStringLiteral("Mock"), root, QString(), 0, true});
        for (int d = 0; d < folderCount; ++d) {
            const QString dirName = QStringLiteral("dir_%1").arg(d, 4, 10, QLatin1Char('0'));
            const QString dirPath = root + QStringLiteral("\\") + dirName;
            mockEntries.push_back({dirName, dirPath, QString(), 0, true});
        }
        int generated = 0;
        for (int i = 0; i < targetCount; ++i) {
            const int d = i % folderCount;
            const QString dirName = QStringLiteral("dir_%1").arg(d, 4, 10, QLatin1Char('0'));
            const QString ext = exts.at(i % exts.size());
            const QString fileName = QStringLiteral("file_%1.%2").arg(i, 6, 10, QLatin1Char('0')).arg(ext);
            const QString filePath = root + QStringLiteral("\\") + dirName + QStringLiteral("\\") + fileName;
            const qint64 size = 1024 + (i % 2048) * 73;
            mockEntries.push_back({fileName, filePath, ext, size, false});
            ++generated;
        }
        LOG_INFO(L"RecoveryScanPage mock generate entries=" + std::to_wstring(static_cast<uint64_t>(generated)) +
                 L" folders=" + std::to_wstring(static_cast<uint64_t>(folderCount)));
    }

    quint64 nextId = 1;
    for (const MockEntry &entry : mockEntries) {
        ScanFileDto dto;
        dto.id = nextId++;
        dto.name = entry.name;
        dto.fullPath = entry.fullPath;
        dto.relativePath = relativeVolumePath(entry.fullPath, diskLetter_);
        dto.extension = entry.ext;
        dto.sizeBytes = entry.sizeBytes;
        dto.isDirectory = entry.isDirectory;
        dto.modifyTimeFt = 0;
        if (resultStore_) {
            resultStore_->upsert(dto);
        }
    }

    foundTotal_ = 0;
    for (const MockEntry &e : mockEntries) {
        if (!e.isDirectory) {
            ++foundTotal_;
        }
    }
    scannedBytes_ = 220ull * 1024ull * 1024ull * 1024ull;
    totalBytes_ = 500ull * 1024ull * 1024ull * 1024ull;
    scanRunning_ = true;

    progressBar_->setValue(44);
    statusLabel_->setText(QStringLiteral("扫描进行中（模拟数据）"));
    setMetricLabelText(metricFoundLabel_, QString::number(foundTotal_), QStringLiteral("已找到"));
    setMetricLabelText(metricScannedLabel_, formatSize(scannedBytes_), QStringLiteral("已扫描"));
    setMetricLabelText(metricTotalLabel_, formatSize(totalBytes_), QStringLiteral("总计"));
    setMetricLabelText(metricElapsedLabel_, QStringLiteral("00:18"), QStringLiteral("用时"));
    setMetricLabelText(metricSpeedLabel_, QStringLiteral("12.2 MB/s"), QStringLiteral("速度"));
    if (scanPercentLabel_) scanPercentLabel_->setText(QStringLiteral("44%"));
    if (scanCardProgressBar_) scanCardProgressBar_->setValue(44);
    if (scanPhaseLabel_) scanPhaseLabel_->setText(QStringLiteral("正在分析 NTFS 删除记录"));
    if (scanProgressTextLabel_) {
        scanProgressTextLabel_->setText(QStringLiteral("已扫描 %1 / %2")
                                             .arg(formatSize(static_cast<qint64>(scannedBytes_)))
                                             .arg(formatSize(static_cast<qint64>(totalBytes_))));
    }
    if (scanEtaLabel_) scanEtaLabel_->setText(QStringLiteral("预计还需 12 分钟"));
    if (scanFoundLabel_) scanFoundLabel_->setText(QLocale::system().toString(foundTotal_));
    if (scanScannedLabel_) scanScannedLabel_->setText(formatSize(static_cast<qint64>(scannedBytes_)));
    if (scanSpeedLabel_) scanSpeedLabel_->setText(QStringLiteral("12.2 MB/s"));
    if (footerFoundLabel_) {
        footerFoundLabel_->setText(QStringLiteral("扫描中，已找到文件：%1").arg(foundTotal_));
    }
    if (stopScanBtn_) {
        stopScanBtn_->setEnabled(true);
    }

    if (pathRoot_) {
        pathTree_->setCurrentItem(pathRoot_);
        refillTableFromPathItem(pathRoot_);
    }
    if (typeTree_->topLevelItemCount() > 0) {
        typeTree_->setCurrentItem(typeTree_->topLevelItem(0));
    }
    onLeftTabChanged(leftTabs_->currentIndex());

    if (resultStore_) {
        onScanIndexUpdated(resultStore_->revision());
    }
}

QString RecoveryScanPage::relativeVolumePath(const QString &fullPath, const QString &diskLetter)
{
    QString d = diskLetter;
    if (!d.endsWith(QLatin1Char(':'))) {
        d += QLatin1Char(':');
    }
    QString p = fullPath;
    if (p.startsWith(d, Qt::CaseInsensitive)) {
        p = p.mid(d.size());
    }
    if (p.startsWith(QLatin1Char('\\')) || p.startsWith(QLatin1Char('/'))) {
        p = p.mid(1);
    }
    return p;
}

QString RecoveryScanPage::fileTypeDisplay(const ScanFileDto &dto)
{
    if (dto.isDirectory) {
        return QStringLiteral("文件夹");
    }
    if (dto.extension.isEmpty()) {
        return QStringLiteral("文件");
    }
    return QStringLiteral("%1 文件").arg(dto.extension.toUpper());
}

QString RecoveryScanPage::formatSize(qint64 bytes)
{
    if (bytes < 0) {
        bytes = 0;
    }
    return QLocale::system().formattedDataSize(bytes, 2, QLocale::DataSizeIecFormat);
}

QString RecoveryScanPage::formatFileTime(quint64 fileTime100ns)
{
    if (fileTime100ns == 0) {
        return QStringLiteral("—");
    }
    ULARGE_INTEGER uli;
    uli.QuadPart = fileTime100ns;
    FILETIME ft;
    ft.dwLowDateTime = uli.LowPart;
    ft.dwHighDateTime = uli.HighPart;
    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    if (!FileTimeToSystemTime(&ft, &utc)) {
        return QStringLiteral("—");
    }
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    const QDateTime dt(QDate(local.wYear, local.wMonth, local.wDay),
                       QTime(local.wHour, local.wMinute, local.wSecond));
    return QLocale::system().toString(dt, QLocale::ShortFormat);
}

QString RecoveryScanPage::categoryForExtension(const QString &ext)
{
    return QString::fromStdWString(
        UiFileTypeCatalog::CategoryForExtension(ext.toLower().toStdWString()));
}

void RecoveryScanPage::insertPathForDto(const ScanFileDto &dto)
{
    QString rel = relativeVolumePath(dto.fullPath, diskLetter_);
    rel.replace(QLatin1Char('/'), QLatin1Char('\\'));
    if (rel.isEmpty() && !dto.name.isEmpty()) {
        rel = dto.name;
    }
    rel = rel.trimmed();
    while (rel.endsWith(QLatin1Char('\\'))) {
        rel.chop(1);
    }

    const QStringList fullParts = rel.split(QChar('\\'), QString::SkipEmptyParts);
    if (fullParts.isEmpty()) {
        return;
    }

    QString parentRel;
    QString nodeRel = rel;
    if (dto.isDirectory) {
        const int slash = rel.lastIndexOf(QLatin1Char('\\'));
        parentRel = (slash >= 0) ? rel.left(slash) : QString();
    } else {
        const int slash = rel.lastIndexOf(QLatin1Char('\\'));
        parentRel = (slash >= 0) ? rel.left(slash) : QString();
        nodeRel = parentRel;
    }

    pathNodeCounts_[QString()] += 1;
    pathDirectChildIds_[parentRel].append(dto.id);

    if (nodeRel.isEmpty()) {
        if (pathRoot_) {
            updateTreeItemCount(pathRoot_);
        }
        return;
    }

    const QStringList folderParts = nodeRel.split(QChar('\\'), QString::SkipEmptyParts);
    QString currentParentRel;
    for (int i = 0; i < folderParts.size(); ++i) {
        const QString seg = folderParts.at(i);
        const QString currentRel = currentParentRel.isEmpty() ? seg : (currentParentRel + QStringLiteral("\\") + seg);
        pathNodeCounts_[currentRel] += 1;
        if (!pathChildDirectories_[currentParentRel].contains(seg, Qt::CaseInsensitive)) {
            pathChildDirectories_[currentParentRel].append(seg);
        }
        if (pathTree_) {
            QTreeWidgetItem *parentItem = findPathTreeItemByRelativePath(currentParentRel);
            if (parentItem) {
                updateTreeItemCount(parentItem);
                if (parentItem->isExpanded()) {
                    ensurePathChildrenLoaded(parentItem, parentItem == pathRoot_ ? 4 : 2, "syncTableChecksFromTreeSelection");
                }
            }
            QTreeWidgetItem *currentItem = findPathTreeItemByRelativePath(currentRel);
            if (currentItem) {
                updateTreeItemCount(currentItem);
            }
        }
        currentParentRel = currentRel;
    }
}

void RecoveryScanPage::addFileToTypeTree(const ScanFileDto &dto)
{
    if (dto.isDirectory) {
        return;
    }
    const QString cat = categoryForExtension(dto.extension);
    const QString ext = dto.extension.trimmed().toLower();
    const QString subLabel = ext.isEmpty() ? QStringLiteral("无扩展名") : ext;
    const QString subKey = cat + QStringLiteral("::") + subLabel;
    typeNodeCounts_[cat] += 1;
    typeNodeCounts_[subKey] += 1;
    typeNodeFileIds_[cat].append(dto.id);
    typeNodeFileIds_[subKey].append(dto.id);
    if (!typeCategoryChildren_[cat].contains(subLabel, Qt::CaseInsensitive)) {
        typeCategoryChildren_[cat].append(subLabel);
    }

    if (typeTree_) {
        QTreeWidgetItem *catItem = typeCategoryItems_.value(cat);
        if (!catItem) {
            catItem = createTypeCategoryItem(cat);
            typeTree_->insertTopLevelItem(desiredTypeCategoryInsertIndex(typeTree_, cat), catItem);
            typeCategoryItems_.insert(cat, catItem);
        }
        updateTreeItemCount(catItem);
        if (catItem->isExpanded()) {
            ensureTypeChildrenLoaded(catItem);
        }
        QTreeWidgetItem *subItem = typeSubcategoryItems_.value(subKey);
        if (subItem) {
            updateTreeItemCount(subItem);
        }
    }
}

void RecoveryScanPage::refillTableFromPathItem(QTreeWidgetItem *item)
{
    if (!tableView_ && !table_) {
        return;
    }
    if (!item) {
        if (filterModel_) {
            filterModel_->setPathPrefix(QString());
        }
        resetVisibleResults(QList<ScanFileDto>(), Qt::Unchecked);
        return;
    }
    const QString folderRel = itemRelativeFolderPath(item);
    if (filterModel_) {
        filterModel_->setPathPrefix(folderRel);
    }
    const QList<ScanFileDto> visible = collectVisibleFilesForPath(folderRel);
    const quint64 currentRevision = resultStore_ ? resultStore_->revision() : 0;
    if (lastPathRefreshKey_ == folderRel &&
        lastPathRefreshItemCount_ == visible.size() &&
        lastPathRefreshRevision_ == currentRevision) {
        return;
    }
    lastPathRefreshKey_ = folderRel;
    lastPathRefreshItemCount_ = visible.size();
    lastPathRefreshRevision_ = currentRevision;
    resetVisibleResults(visible, Qt::Unchecked);
}

void RecoveryScanPage::refillTableFromTypeItem(QTreeWidgetItem *item)
{
    if (!tableView_ && !table_) {
        return;
    }
    if (!item) {
        resetVisibleResults(QList<ScanFileDto>(), Qt::Unchecked);
        return;
    }
    const Qt::CheckState typeState =
        item->checkState(0) == Qt::Checked ? Qt::Checked : Qt::Unchecked;
    const QString cat = item->data(0, Qt::UserRole + 6).toString();
    const QString sub = item->data(0, Qt::UserRole + 7).toString();
    const QString key = sub.isEmpty() ? cat : (cat + QStringLiteral("::") + sub);
    QElapsedTimer refillTimer;
    refillTimer.start();
    QVector<quint64> ids = resultStore_ ? resultStore_->typeNodeFileIds(key) : QVector<quint64>();
    const bool sortCandidates = shouldSortTypeCandidates(cat);
    if (sortCandidates) {
        std::sort(ids.begin(), ids.end(), lessTypeCandidateId);
    }
    const quint64 currentRevision = resultStore_ ? resultStore_->revision() : 0;
    if (lastTypeRefreshKey_ == key &&
        lastTypeRefreshItemCount_ == ids.size() &&
        lastTypeRefreshRevision_ == currentRevision) {
        LOG_INFO(L"RecoveryScanPage::refillTableFromTypeItem skip duplicate"
                 L" type=[" + std::wstring(reinterpret_cast<const wchar_t *>(key.utf16()),
                                          static_cast<size_t>(key.size())) + L"]" +
                 L" candidateCount=" + std::to_wstring(static_cast<uint64_t>(ids.size())) +
                 L" revision=" + std::to_wstring(static_cast<uint64_t>(currentRevision)));
        return;
    }
    LOG_INFO(L"RecoveryScanPage::refillTableFromTypeItem"
             L" type=[" + std::wstring(reinterpret_cast<const wchar_t *>(key.utf16()),
                                      static_cast<size_t>(key.size())) + L"]" +
             L" candidateCount=" + std::to_wstring(static_cast<uint64_t>(ids.size())) +
             L" sorted=" + std::wstring(sortCandidates ? L"yes" : L"no") +
             L" prepMs=" + std::to_wstring(static_cast<uint64_t>(refillTimer.elapsed())) +
             L" revision=" + std::to_wstring(static_cast<uint64_t>(currentRevision)));
    lastTypeRefreshKey_ = key;
    lastTypeRefreshItemCount_ = ids.size();
    lastTypeRefreshRevision_ = currentRevision;
    ++visibleResultsGeneration_;
    visibleResults_.clear();
    visibleResultsUseTypePaging_ = true;
    visibleResultsContinuationScheduled_ = false;
    visibleTypeCandidateIds_ = ids;
    visibleTypeCandidateCursor_ = 0;
    visibleResultsDefaultState_ = typeState;
    visibleResultsLoaded_ = 0;
    if (gridView_ && previewMode_) {
        gridView_->clear();
        gridView_->setSpacing(14);
        gridView_->setGridSize(QSize(228, 246));
    }
    ensureVisibleResultRowsLoaded(true);
}

void RecoveryScanPage::refillTableFromSelection()
{
    if (leftTabs_->currentIndex() == 0) {
        refillTableFromPathItem(pathTree_->currentItem());
    } else {
        refillTableFromTypeItem(typeTree_->currentItem());
    }
    refreshResultsHeader();
}

void RecoveryScanPage::onPathTreeCurrentChanged(QTreeWidgetItem *current)
{
    Q_UNUSED(current);
    lastPathRefreshKey_.clear();
    lastPathRefreshItemCount_ = -1;
    refillTableFromPathItem(current);
    if (tableView_ && filterModel_ && filterModel_->rowCount() > 0) {
        tableView_->selectRow(0);
    }
    refreshResultsHeader();
}

void RecoveryScanPage::setTreeSubtreeCheckState(QTreeWidgetItem *item, Qt::CheckState state)
{
    if (!item) {
        return;
    }
    const Qt::CheckState oldState = item->checkState(0);
    bool hasChildNeedingUpdate = false;
    for (int i = 0; i < item->childCount(); ++i) {
        if (item->child(i)->checkState(0) != state) {
            hasChildNeedingUpdate = true;
            break;
        }
    }
    if (oldState == state && !hasChildNeedingUpdate) {
        return;
    }
    LOG_INFO(L"RecoveryScanPage::setTreeSubtreeCheckState"
             L" tree=" + std::wstring(item->treeWidget() == pathTree_ ? L"path" : (item->treeWidget() == typeTree_ ? L"type" : L"unknown")) +
             L" childCount=" + std::to_wstring(static_cast<uint64_t>(item->childCount())) +
             L" expanded=" + std::wstring(item->isExpanded() ? L"yes" : L"no") +
             L" oldState=" + std::wstring(oldState == Qt::Checked ? L"checked" : (oldState == Qt::Unchecked ? L"unchecked" : L"partial")) +
             L" newState=" + std::wstring(state == Qt::Checked ? L"checked" : (state == Qt::Unchecked ? L"unchecked" : L"partial")));
    item->setCheckState(0, state);
    for (int i = 0; i < item->childCount(); ++i) {
        if (item->child(i)->checkState(0) != state) {
            setTreeSubtreeCheckState(item->child(i), state);
        }
    }
}

void RecoveryScanPage::updateParentCheckState(QTreeWidgetItem *item)
{
    if (!item) {
        return;
    }

    QTreeWidgetItem *parent = item->parent();
    if (!parent) {
        return;
    }

    refreshCheckedKeyCaches();
    Qt::CheckState parentState = Qt::Unchecked;
    if (parent->treeWidget() == pathTree_) {
        parentState = computePathParentState(itemRelativeFolderPath(parent));
    } else if (parent->treeWidget() == typeTree_) {
        const QString cat = parent->data(0, Qt::UserRole + 6).toString();
        const QString sub = parent->data(0, Qt::UserRole + 7).toString();
        const QString key = sub.isEmpty() ? cat : (cat + QStringLiteral("::") + sub);
        parentState = computeTypeParentState(key);
    }
    if (parent->checkState(0) == parentState) {
        return;
    }
    parent->setCheckState(0, parentState);
    updateParentCheckState(parent);
}

QTreeWidgetItem *RecoveryScanPage::findPathTreeItemByRelativePath(const QString &relPath) const
{
    if (!pathTree_) {
        return nullptr;
    }

    QList<QTreeWidgetItem *> stack;
    for (int i = 0; i < pathTree_->topLevelItemCount(); ++i) {
        stack.append(pathTree_->topLevelItem(i));
    }

    while (!stack.isEmpty()) {
        QTreeWidgetItem *item = stack.takeLast();
        if (itemRelativeFolderPath(item).compare(relPath, Qt::CaseInsensitive) == 0) {
            return item;
        }
        for (int i = 0; i < item->childCount(); ++i) {
            stack.append(item->child(i));
        }
    }
    return nullptr;
}

bool RecoveryScanPage::isPathUnderCheckedFolder(const QString &folderRelPath, bool isDirectory) const
{
    Q_UNUSED(isDirectory);
    for (auto it = checkedPathKeys_.cbegin(); it != checkedPathKeys_.cend(); ++it) {
        const QString &checkedFolder = *it;
        if (checkedFolder.isEmpty()) {
            return true;
        }
        if (folderRelPath == checkedFolder ||
            folderRelPath.startsWith(checkedFolder + QStringLiteral("\\"))) {
            return true;
        }
    }
    return false;
}

void RecoveryScanPage::syncTableChecksFromTreeSelection()
{
    if (syncingTableChecks_) {
        return;
    }

    refreshCheckedKeyCaches();
    syncingTableChecks_ = true;

    if (scanModel_) {
        for (int row = 0; row < scanModel_->rowCount(); ++row) {
            const ScanFileDto *dto = scanModel_->dtoAtRow(row);
            if (!dto) continue;
            Qt::CheckState state = Qt::Unchecked;
            if (leftTabs_ && leftTabs_->currentIndex() == 1) {
                const QString category = categoryForExtension(dto->extension);
                const QString ext = dto->extension.trimmed().toLower();
                const QString sub = category == QStringLiteral("\u5176\u4ed6")
                                        ? otherFixedSubtypeForUi(ext)
                                        : (ext.isEmpty() ? QStringLiteral("\u65e0\u6269\u5c55\u540d") : ext);
                state = isTypeUnderCheckedNode(category, sub)
                            ? Qt::Checked
                            : Qt::Unchecked;
            } else {
                const QString relPath = dto->relativePath;
                const bool isDir = dto->isDirectory;
                state = isPathUnderCheckedFolder(relPath, isDir) ? Qt::Checked : Qt::Unchecked;
            }
            scanModel_->setChecked(row, state == Qt::Checked);
        }
    }

    if (gridView_) {
        for (int i = 0; i < gridView_->count(); ++i) {
            QListWidgetItem *gridItem = gridView_->item(i);
            QWidget *card = gridItem ? gridView_->itemWidget(gridItem) : nullptr;
            QCheckBox *checkBox = gridCardCheckBoxAt(i);
            if (!checkBox) {
                continue;
            }
            Qt::CheckState state = Qt::Unchecked;
            if (leftTabs_ && leftTabs_->currentIndex() == 1) {
                const quint64 id = checkBox->property("rowId").toULongLong();
                ScanFileDto dto;
                if (resultStore_ && resultStore_->tryGetFile(id, &dto)) {
                    const QString category = categoryForExtension(dto.extension);
                    const QString ext = dto.extension.trimmed().toLower();
                    const QString sub = category == QStringLiteral("\u5176\u4ed6")
                                            ? otherFixedSubtypeForUi(ext)
                                            : (ext.isEmpty() ? QStringLiteral("\u65e0\u6269\u5c55\u540d") : ext);
                    state = isTypeUnderCheckedNode(category, sub)
                                ? Qt::Checked
                                : Qt::Unchecked;
                }
            } else {
                const QString relPath = checkBox->property("relPath").toString();
                const bool isDir = checkBox->property("isDirectory").toBool();
                state = isPathUnderCheckedFolder(relPath, isDir) ? Qt::Checked : Qt::Unchecked;
            }
            checkBox->setCheckState(state);
            if (card) {
                card->setStyleSheet(state == Qt::Checked
                    ? QStringLiteral(
                          "#scanThumbCard { background: #f7fbf8; border: 2px solid #22c55e; border-radius: 14px; }"
                          "#scanThumbCard QLabel[role='name'] { color: #111827; font-size: 13px; font-weight: 700; }"
                          "#scanThumbCard QLabel[role='meta'] { color: #6b7280; font-size: 11px; }")
                    : QStringLiteral(
                          "#scanThumbCard { background: #ffffff; border: 1px solid #e5e7eb; border-radius: 14px; }"
                          "#scanThumbCard QLabel[role='name'] { color: #111827; font-size: 13px; font-weight: 700; }"
                          "#scanThumbCard QLabel[role='meta'] { color: #6b7280; font-size: 11px; }"));
            }
        }
    }
    syncingTableChecks_ = false;
    updateHeaderSelectAllCheckState();
}

QCheckBox *RecoveryScanPage::tableRowCheckBoxAt(int row) const
{
    Q_UNUSED(row);
    return nullptr;
}

QCheckBox *RecoveryScanPage::gridCardCheckBoxAt(int index) const
{
    if (!gridView_ || index < 0 || index >= gridView_->count()) {
        return nullptr;
    }
    QListWidgetItem *item = gridView_->item(index);
    if (!item) {
        return nullptr;
    }
    QWidget *card = gridView_->itemWidget(item);
    if (!card) {
        return nullptr;
    }
    return card->findChild<QCheckBox *>();
}

QWidget *RecoveryScanPage::createTableCheckBoxWidget(quint64 id,
                                                     bool isDirectory,
                                                     const QString &relPath,
                                                     Qt::CheckState state)
{
    auto *wrap = new QWidget(this);
    wrap->setAttribute(Qt::WA_StyledBackground, true);
    wrap->setStyleSheet(QStringLiteral("background: transparent;"));
    auto *lay = new QHBoxLayout(wrap);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    auto *cb = new QCheckBox(wrap);
    cb->setTristate(false);
    cb->setCheckState(state);
    cb->setProperty("rowId", QVariant::fromValue(id));
    cb->setProperty("isDirectory", isDirectory);
    cb->setProperty("relPath", relPath);
    cb->setStyleSheet(QStringLiteral(
        "QCheckBox { background: transparent; spacing: 0px; padding: 0px; margin: 0px; }"
        "QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid #e5e7eb; border-radius: 4px; background: #ffffff; }"
        "QCheckBox::indicator:hover { border-color: #16a34a; background: #ffffff; }"
        "QCheckBox::indicator:checked { border: 1px solid #16a34a; border-radius: 4px; background: #16a34a;"
        " image: url(:/qt-project.org/styles/commonstyle/images/checkbox_checked.png); }"
        "QCheckBox::indicator:indeterminate { border: 1px solid #16a34a; border-radius: 4px; background: #eefbf3;"
        " image: url(:/qt-project.org/styles/commonstyle/images/checkbox_indeterminate.png); }"));
    cb->setFixedSize(20, 20);
    lay->addStretch(1);
    lay->addWidget(cb, 0, Qt::AlignCenter);
    lay->addStretch(1);
    wrap->setMinimumSize(24, 24);

    connect(cb, &QCheckBox::stateChanged, this, [this, cb](int) {
        if (syncingTableChecks_) {
            return;
        }

        syncingTreeChecks_ = true;
        if (leftTabs_ && leftTabs_->currentIndex() == 1) {
            const quint64 id = cb->property("rowId").toULongLong();
            ScanFileDto dto;
            if (resultStore_ && resultStore_->tryGetFile(id, &dto)) {
                const QString category = categoryForExtension(dto.extension);
                const QString ext = dto.extension.trimmed().toLower();
                const QString sub = category == QStringLiteral("其他")
                                        ? otherFixedSubtypeForUi(ext)
                                        : (ext.isEmpty() ? QStringLiteral("无扩展名") : ext);
                QTreeWidgetItem *treeItem = ensureTypeTreeItemByKey(category, sub);
                if (treeItem) {
                    treeItem->setCheckState(0, cb->checkState());
                    updateParentCheckState(treeItem);
                }
            }
        } else {
            const QString relPath = cb->property("relPath").toString();
            const bool isDir = cb->property("isDirectory").toBool();
            if (!relPath.isEmpty()) {
                QString folderRel = relPath;
                if (!isDir) {
                    const int slash = folderRel.lastIndexOf(QLatin1Char('\\'));
                    folderRel = (slash >= 0) ? folderRel.left(slash) : QString();
                }
                QTreeWidgetItem *treeItem = ensurePathTreeItemByRelativePath(folderRel);
                if (treeItem) {
                    treeItem->setCheckState(0, cb->checkState());
                    if (isDir) {
                        setTreeSubtreeCheckState(treeItem, cb->checkState());
                    }
                    updateParentCheckState(treeItem);
                }
            }
        }
        syncingTreeChecks_ = false;

        syncTableChecksFromTreeSelection();
        updateHeaderSelectAllCheckState();
    });

    return wrap;
}

void RecoveryScanPage::updateHeaderSelectAllCheckState()
{
    if (!headerSelectAllCheckBox_ || !scanModel_) {
        return;
    }

    const int checked = scanModel_->checkedCount();
    const int total = scanModel_->rowCount();
    const int unchecked = total - checked;

    syncingHeaderCheck_ = true;
    if (checked > 0 && unchecked > 0) {
        headerSelectAllCheckBox_->setTristate(true);
        headerSelectAllCheckBox_->setCheckState(Qt::PartiallyChecked);
    } else {
        headerSelectAllCheckBox_->setTristate(false);
        headerSelectAllCheckBox_->setCheckState(checked > 0 ? Qt::Checked : Qt::Unchecked);
    }
    syncingHeaderCheck_ = false;

    if (resultsMode_ && resultsMetaLabel_) {
        resultsMetaLabel_->setText(
            QStringLiteral("已选：%1 项 · 预计恢复：%2")
                .arg(checked)
                .arg(formatSize(static_cast<qint64>(checkedRecoverableBytes()))));
    }
    if (recoverAllBtn_) {
        recoverAllBtn_->setEnabled(checked > 0);
    }
}

QVector<quint64> RecoveryScanPage::checkedRecoverableIdsFromTable() const
{
    QVector<quint64> ids;
    if (!scanModel_ || !resultStore_) {
        return ids;
    }
    const QVector<quint64> allChecked = scanModel_->checkedIds();
    ids.reserve(allChecked.size());
    for (const quint64 id : allChecked) {
        if (id == 0) continue;
        ScanFileDto dto;
        if (!resultStore_->tryGetFile(id, &dto) || dto.isDirectory) {
            continue;
        }
        ids.append(id);
    }
    return ids;
}

quint64 RecoveryScanPage::checkedRecoverableBytes() const
{
    quint64 total = 0;
    const QVector<quint64> ids = checkedRecoverableIdsFromTable();
    for (const quint64 id : ids) {
        ScanFileDto dto;
        if (!resultStore_ || !resultStore_->tryGetFile(id, &dto) || dto.sizeBytes <= 0) {
            continue;
        }
        const quint64 size = static_cast<quint64>(dto.sizeBytes);
        if (std::numeric_limits<quint64>::max() - total < size) {
            return std::numeric_limits<quint64>::max();
        }
        total += size;
    }
    return total;
}

bool RecoveryScanPage::isRepairableDto(const ScanFileDto& dto) const
{
    if (dto.isDirectory) {
        return false;
    }
    return RepairFormatUtil::isRepairableExtension(dto.extension);
}

QVector<quint64> RecoveryScanPage::repairCandidateIdsFromSelection() const
{
    QVector<quint64> ids = checkedRecoverableIdsFromTable();
    if (!ids.isEmpty()) {
        QVector<quint64> filtered;
        for (quint64 id : ids) {
            ScanFileDto dto;
            if (resultStore_->tryGetFile(id, &dto) && isRepairableDto(dto)) {
                filtered.append(id);
            }
        }
        return filtered;
    }

    if (tableView_ && filterModel_ && scanModel_) {
        const QModelIndex idx = tableView_->currentIndex();
        if (idx.isValid()) {
            const int srcRow = filterModel_->mapToSource(idx).row();
            if (const ScanFileDto* dto = scanModel_->dtoAtRow(srcRow)) {
                if (isRepairableDto(*dto)) {
                    return {dto->id};
                }
            }
        }
    }
    return {};
}

QStringList RecoveryScanPage::exportFilesToRepairStaging(const QVector<quint64>& fileIds)
{
    QStringList paths;
    if (!resultStore_ || fileIds.isEmpty() || diskLetter_.isEmpty()) {
        return paths;
    }

    Partition sourcePartition;
    if (!resultStore_->tryGetScanVolumePartition(&sourcePartition)) {
        return paths;
    }

    const QString stagingRoot = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                                + QStringLiteral("/PhoenixRecover/repair_staging");
    QDir stagingDir(stagingRoot);
    if (stagingDir.exists()) {
        stagingDir.removeRecursively();
    }
    stagingDir.mkpath(QStringLiteral("."));

    SectorReader reader;
    if (!openScanSourceReader(diskLetter_, resultStore_, reader)) {
        return paths;
    }

    RecoverEngine engine;
    RecoverOptions recoverOpts;
    recoverOpts.formatAwareFatFragments =
        formatAwareFragmentCheck_ ? formatAwareFragmentCheck_->isChecked() : true;
    engine.SetOptions(recoverOpts);
    for (quint64 id : fileIds) {
        FileNode node;
        if (!resultStore_->tryGetRecoveredFileNode(id, &node) || node.isDirectory) {
            continue;
        }
        const QString perIdDir = stagingRoot + QLatin1Char('/') + QString::number(id);
        QDir().mkpath(perIdDir);
        const std::wstring targetDirW = QStringToWString(perIdDir);
        const FileRecoverResult rr = engine.RecoverFile(node, targetDirW, false, reader, sourcePartition);
        if (rr.status == RecoverStatus::Success || rr.status == RecoverStatus::Partial) {
            paths.append(QString::fromStdWString(rr.targetPath));
        }
    }
    return paths;
}

void RecoveryScanPage::onRepairRequested()
{
    if (qEnvironmentVariableIsSet("PHOENIX_TEST_MODE") ||
        !qgetenv("PHOENIX_MOCK_SCAN_COUNT").isEmpty()) {
        QMessageBox::information(this, QStringLiteral("测试模式"),
                                 QStringLiteral("测试模式不会导出待修复文件或写入恢复副本。"));
        return;
    }
    if (!resultsMode_ || scanRunning_) {
        QMessageBox::information(this, QStringLiteral("文件修复"),
                                 QStringLiteral("请等待扫描完成后再尝试修复。"));
        return;
    }

    const QVector<quint64> ids = repairCandidateIdsFromSelection();
    if (ids.isEmpty()) {
        QMessageBox::information(
            this,
            QStringLiteral("文件修复"),
            QStringLiteral("请勾选或选中一个支持修复的文件（图片/视频/Office/ZIP 等）。"));
        return;
    }

    QProgressDialog progress(QStringLiteral("正在导出待修复副本…"), QString(), 0, ids.size(), this);
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    progress.setValue(0);
    QCoreApplication::processEvents();

    const QStringList paths = exportFilesToRepairStaging(ids);
    progress.setValue(ids.size());

    if (paths.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("文件修复"),
                             QStringLiteral("无法从扫描结果导出可修复副本，请尝试先「恢复」到磁盘后再修复。"));
        return;
    }

    emit requestRepairFiles(paths);
}

void RecoveryScanPage::onRecoverAllClicked()
{
    if (qEnvironmentVariableIsSet("PHOENIX_TEST_MODE") ||
        !qgetenv("PHOENIX_MOCK_SCAN_COUNT").isEmpty()) {
        QMessageBox::information(this, QStringLiteral("测试模式"),
                                 QStringLiteral("测试模式不会执行恢复写入，只展示模拟扫描结果。"));
        return;
    }
    if (recoveryRunning_) {
        return;
    }
    if (!resultStore_) {
        QMessageBox::warning(this, QStringLiteral("恢复"), QStringLiteral("当前没有可用的扫描结果。"));
        return;
    }

    const QVector<quint64> selectedIds = checkedRecoverableIdsFromTable();
    if (selectedIds.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("恢复"), QStringLiteral("请先勾选至少一个可恢复文件。"));
        return;
    }

    Partition sourcePartition;
    if (!resultStore_->tryGetScanVolumePartition(&sourcePartition)) {
        QMessageBox::warning(this, QStringLiteral("恢复"), QStringLiteral("无法获取源分区信息，暂时不能恢复。"));
        return;
    }

    QString targetDir;
    if (recoveryRetryPending_ && !recoveryLastTargetDir_.trimmed().isEmpty()) {
        targetDir = recoveryLastTargetDir_;
        recoveryRetryPending_ = false;
    } else {
        targetDir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("选择恢复目录"),
        QDir::homePath(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    }
    if (targetDir.isEmpty()) {
        return;
    }

    const std::wstring targetDirW = QStringToWString(QDir::toNativeSeparators(targetDir));
    if (!RecoverEngine::ValidateTargetPath(targetDirW, sourcePartition)) {
        QMessageBox::warning(
            this,
            QStringLiteral("恢复"),
            QStringLiteral("恢复目录不能与源分区相同，请选择其他磁盘位置。"));
        return;
    }

    const quint64 selectedBytes = checkedRecoverableBytes();
    const QStorageInfo targetStorage(targetDir);
    if (selectedBytes > 0 && targetStorage.isValid() && targetStorage.isReady()) {
        const qint64 availableSigned = targetStorage.bytesAvailable();
        const quint64 availableBytes = availableSigned > 0
                                           ? static_cast<quint64>(availableSigned)
                                           : 0;
        if (selectedBytes > availableBytes) {
            QMessageBox::warning(
                this,
                QStringLiteral("恢复空间不足"),
                QStringLiteral("预计恢复数据量：%1\n目标位置可用空间：%2\n\n请清理目标磁盘或选择其他位置后重试。")
                    .arg(formatSize(static_cast<qint64>(selectedBytes)))
                    .arg(formatSize(static_cast<qint64>(availableBytes))));
            return;
        }
    }

    // 目标卷安全校验始终保留；该设置控制的是用户在选择目录后的二次确认，
    // 不允许通过关闭确认来绕过“禁止写回源盘”的硬性保护。
    QSettings prefs;
    if (prefs.value(QStringLiteral("recovery/confirmTarget"), true).toBool()) {
        const auto confirm = QMessageBox::question(
            this,
            QStringLiteral("确认恢复位置"),
            QStringLiteral("确定将选中的 %1 个文件恢复到以下目录吗？\n预计数据量：%2\n\n%3")
                .arg(selectedIds.size())
                .arg(formatSize(static_cast<qint64>(selectedBytes)))
                .arg(QDir::toNativeSeparators(targetDir)),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::Yes);
        if (confirm != QMessageBox::Yes) {
            return;
        }
    }

    std::vector<FileNode> nodes;
    nodes.reserve(static_cast<size_t>(selectedIds.size()));
    for (int i = 0; i < selectedIds.size(); ++i) {
        FileNode node;
        if (resultStore_->tryGetRecoveredFileNode(selectedIds.at(i), &node) && !node.isDirectory) {
            nodes.push_back(std::move(node));
        }
    }
    if (nodes.empty()) {
        QMessageBox::warning(
            this,
            QStringLiteral("恢复"),
            QStringLiteral("当前勾选项缺少底层恢复数据，请重新扫描后重试。"));
        return;
    }

    recoveryRunning_ = true;
    if (recoverAllBtn_) {
        recoverAllBtn_->setEnabled(false);
    }

    recoveryNodeStorage_ = std::make_shared<std::vector<FileNode>>(std::move(nodes));
    recoveryCancel_ = std::make_shared<std::atomic<bool>>(false);
    recoveryPause_ = std::make_shared<std::atomic<bool>>(false);
    const auto nodeStorage = recoveryNodeStorage_;
    const auto cancel = recoveryCancel_;
    const auto pause = recoveryPause_;
    const auto recoveryTimer = std::make_shared<QElapsedTimer>();
    recoveryTimer->start();
    const QString sourceId = diskLetter_;
    const bool keepDirStructure = keepDirStructureCheck_ ? keepDirStructureCheck_->isChecked() : true;
    const bool formatAwareFragments =
        formatAwareFragmentCheck_ ? formatAwareFragmentCheck_->isChecked() : true;

    recoveryProgressDialog_ = new QProgressDialog(
        QStringLiteral("正在恢复文件…"), QStringLiteral("取消"),
        0, static_cast<int>(nodeStorage->size()), this);
    recoveryProgressDialog_->setWindowTitle(QStringLiteral("恢复进度"));
    recoveryProgressDialog_->setWindowModality(Qt::WindowModal);
    recoveryProgressDialog_->setMinimumDuration(0);
    recoveryProgressDialog_->setAutoClose(false);
    recoveryProgressDialog_->setValue(0);
    recoveryPauseBtn_ = new QPushButton(QStringLiteral("暂停"), recoveryProgressDialog_);
    recoveryProgressDialog_->layout()->addWidget(recoveryPauseBtn_);
    connect(recoveryPauseBtn_, &QPushButton::clicked, this, [this]() {
        if (!recoveryPause_ || !recoveryProgressDialog_) {
            return;
        }
        const bool paused = !recoveryPause_->load();
        recoveryPause_->store(paused);
        if (recoveryPauseBtn_) {
            recoveryPauseBtn_->setText(paused ? QStringLiteral("继续") : QStringLiteral("暂停"));
        }
        recoveryProgressDialog_->setLabelText(
            paused ? QStringLiteral("已暂停，当前文件完成后等待继续…")
                   : QStringLiteral("正在继续恢复…"));
    });
    connect(recoveryProgressDialog_, &QProgressDialog::canceled, this, [this]() {
        if (recoveryCancel_) {
            recoveryCancel_->store(true);
        }
        if (recoveryPause_) {
            recoveryPause_->store(false);
        }
        if (recoveryProgressDialog_) {
            recoveryProgressDialog_->setLabelText(QStringLiteral("正在停止恢复，请等待当前文件完成…"));
            recoveryProgressDialog_->setCancelButton(nullptr);
        }
    });
    recoveryProgressDialog_->show();

    auto *worker = QThread::create([this, nodeStorage, cancel, pause, recoveryTimer, sourceId, targetDirW,
                                    targetDir, sourcePartition, keepDirStructure,
                                    formatAwareFragments]() {
        SectorReader reader;
        if (!openScanSourceReader(sourceId, resultStore_, reader)) {
            QMetaObject::invokeMethod(this, [this]() {
                recoveryRunning_ = false;
                if (recoveryProgressDialog_) {
                    recoveryProgressDialog_->close();
                    recoveryProgressDialog_->deleteLater();
                    recoveryProgressDialog_ = nullptr;
                }
                recoveryCancel_.reset();
                recoveryPause_.reset();
                recoveryPauseBtn_ = nullptr;
                recoveryNodeStorage_.reset();
                if (recoverAllBtn_) recoverAllBtn_->setEnabled(true);
                QMessageBox::warning(this, QStringLiteral("恢复"),
                                     QStringLiteral("无法打开源卷 %1，请以管理员身份运行后重试。")
                                         .arg(diskLetter_));
            }, Qt::QueuedConnection);
            return;
        }

        RecoverEngine engine;
        RecoverOptions recoverOpts;
        recoverOpts.formatAwareFatFragments = formatAwareFragments;
        engine.SetOptions(recoverOpts);
        engine.SetPauseCallback([cancel, pause]() {
            while (pause->load() && !cancel->load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            return !cancel->load();
        });
        engine.SetProgressCallback([this, cancel, recoveryTimer](uint32_t done, uint32_t total) {
            QMetaObject::invokeMethod(this, [this, cancel, recoveryTimer, done, total]() {
                if (recoveryProgressDialog_) {
                    recoveryProgressDialog_->setMaximum(static_cast<int>(total));
                    recoveryProgressDialog_->setValue(static_cast<int>(done));
                    const double elapsedSeconds = qMax(0.001, recoveryTimer->elapsed() / 1000.0);
                    const double filesPerSecond = static_cast<double>(done) / elapsedSeconds;
                    QString progressText = QStringLiteral("正在恢复文件… 已完成 %1 / %2")
                                                .arg(done).arg(total);
                    if (filesPerSecond > 0.01 && done < total) {
                        const qint64 remainingSeconds = qMax<qint64>(1,
                            static_cast<qint64>((total - done) / filesPerSecond));
                        const qint64 minutes = remainingSeconds / 60;
                        const qint64 seconds = remainingSeconds % 60;
                        progressText += QStringLiteral(" · %1 个/秒 · 预计剩余 %2:%3")
                                            .arg(filesPerSecond, 0, 'f', 1)
                                            .arg(minutes, 2, 10, QLatin1Char('0'))
                                            .arg(seconds, 2, 10, QLatin1Char('0'));
                    } else if (done > 0) {
                        progressText += QStringLiteral(" · %1 个/秒")
                                            .arg(filesPerSecond, 0, 'f', 1);
                    }
                    recoveryProgressDialog_->setLabelText(progressText);
                    if (recoveryProgressDialog_->wasCanceled()) {
                        cancel->store(true);
                    }
                }
            }, Qt::QueuedConnection);
        });

        const RecoverBatchResult batch = engine.RecoverBatch(
            *nodeStorage, targetDirW, keepDirStructure, reader, sourcePartition, cancel.get());
        const QString details = buildRecoverBatchDetailText(batch);
        QMetaObject::invokeMethod(this, [this, nodeStorage, cancel, batch, targetDir, details]() {
            // nodeStorage 必须保持到完成页关闭之后，因为 batch.details 中的 node
            // 指针指向这块存储中的 FileNode。
            recoveryNodeStorage_ = nodeStorage;
            recoveryRunning_ = false;
            if (recoveryProgressDialog_) {
                recoveryProgressDialog_->setValue(recoveryProgressDialog_->maximum());
                recoveryProgressDialog_->close();
                recoveryProgressDialog_->deleteLater();
                recoveryProgressDialog_ = nullptr;
            }
            recoveryCancel_.reset();
            recoveryPause_.reset();
            recoveryPauseBtn_ = nullptr;
            if (recoverAllBtn_) recoverAllBtn_->setEnabled(true);
            updateHeaderSelectAllCheckState();
            showRecoveryCompletePanel(batch, targetDir, details);
        }, Qt::QueuedConnection);
    });
    recoveryThread_ = worker;
    worker->setParent(this);
    connect(worker, &QThread::finished, this, [this, worker]() {
        if (recoveryThread_ == worker) {
            recoveryThread_ = nullptr;
        }
        worker->deleteLater();
    });
    worker->start();
}

void RecoveryScanPage::showRecoveryCompletePanel(const RecoverBatchResult &batch,
                                                 const QString &targetDir,
                                                 const QString &detailText)
{
    if (!recoveryCompletePanel_) {
        return;
    }
    recoveryLastDetailText_ = detailText;
    recoveryLastTargetDir_ = QDir::toNativeSeparators(targetDir);
    if (recoveryCompleteSummaryLabel_) {
        recoveryCompleteSummaryLabel_->setText(
            QStringLiteral("成功: %1  失败: %2  跳过: %3%4")
                .arg(batch.succeeded)
                .arg(batch.failed)
                .arg(batch.skipped)
                .arg(batch.cancelled ? QStringLiteral("  （已取消）") : QString()));
    }
    if (recoveryCompletePathLabel_) {
        recoveryCompletePathLabel_->setText(
            QStringLiteral("目标目录：%1").arg(recoveryLastTargetDir_.isEmpty() ? QStringLiteral("-")
                                                                          : recoveryLastTargetDir_));
    }
    recoveryLastFailedIds_.clear();
    if (recoveryResultFilterCombo_) {
        QSignalBlocker blocker(recoveryResultFilterCombo_);
        recoveryResultFilterCombo_->setCurrentIndex(0);
    }
    if (recoveryCompleteTable_) {
        recoveryCompleteTable_->setRowCount(0);
        recoveryCompleteTable_->setSortingEnabled(false);
        int row = 0;
        for (size_t i = 0; i < batch.details.size(); ++i) {
            const FileRecoverResult &item = batch.details[i];
            if (item.status == RecoverStatus::Failed && item.node) {
                recoveryLastFailedIds_.insert(static_cast<quint64>(item.node->id));
            }
            const QString status = recoveryStatusLabel(item.status);
            const QString name = (item.node && !item.node->name.empty())
                                     ? QString::fromStdWString(item.node->name)
                                     : QStringLiteral("(unknown)");
            const QString target = item.targetPath.empty()
                                       ? QStringLiteral("-")
                                       : QDir::toNativeSeparators(QString::fromStdWString(item.targetPath));
            const QString reason = item.reason.empty()
                                       ? QStringLiteral("-")
                                       : QString::fromStdWString(item.reason);
            recoveryCompleteTable_->insertRow(row);
            auto *statusItem = new QTableWidgetItem(status);
            statusItem->setData(Qt::UserRole, static_cast<int>(item.status));
            recoveryCompleteTable_->setItem(row, 0, statusItem);
            recoveryCompleteTable_->setItem(row, 1, new QTableWidgetItem(name));
            recoveryCompleteTable_->setItem(row, 2, new QTableWidgetItem(target));
            recoveryCompleteTable_->setItem(row, 3, new QTableWidgetItem(reason));
            ++row;
        }
        applyRecoveryResultFilter(recoveryResultFilterCombo_
                                      ? recoveryResultFilterCombo_->currentIndex()
                                      : 0);
    }
    if (recoveryCompleteExportBtn_) {
        recoveryCompleteExportBtn_->setEnabled(!recoveryLastDetailText_.trimmed().isEmpty());
    }
    if (recoveryCompleteOpenDirBtn_) {
        recoveryCompleteOpenDirBtn_->setEnabled(!recoveryLastTargetDir_.trimmed().isEmpty());
    }
    if (recoveryRetryFailedBtn_) {
        recoveryRetryFailedBtn_->setEnabled(!recoveryLastFailedIds_.isEmpty());
    }
    recoveryCompletePanel_->setGeometry(rect().adjusted(18, 18, -18, -18));
    recoveryCompletePanel_->show();
    recoveryCompletePanel_->raise();
}

void RecoveryScanPage::hideRecoveryCompletePanel()
{
    if (!recoveryCompletePanel_) {
        return;
    }
    recoveryCompletePanel_->hide();
}

void RecoveryScanPage::applyRecoveryResultFilter(int filterIndex)
{
    if (!recoveryCompleteTable_) {
        return;
    }
    const int wantedStatus = filterIndex - 1;
    for (int row = 0; row < recoveryCompleteTable_->rowCount(); ++row) {
        const auto *statusItem = recoveryCompleteTable_->item(row, 0);
        const int status = statusItem ? statusItem->data(Qt::UserRole).toInt() : -1;
        recoveryCompleteTable_->setRowHidden(
            row, filterIndex > 0 && status != wantedStatus);
    }
}

void RecoveryScanPage::onRetryFailedClicked()
{
    if (recoveryLastFailedIds_.isEmpty() || recoveryRunning_ || !scanModel_) {
        return;
    }
    if (qEnvironmentVariableIsSet("PHOENIX_TEST_MODE") ||
        !qgetenv("PHOENIX_MOCK_SCAN_COUNT").isEmpty()) {
        QMessageBox::information(this, QStringLiteral("测试模式"),
                                 QStringLiteral("测试模式不会执行恢复写入，只验证扫描和预览流程。"));
        return;
    }

    for (int row = 0; row < scanModel_->rowCount(); ++row) {
        const ScanFileDto *dto = scanModel_->dtoAtRow(row);
        scanModel_->setChecked(row, dto && recoveryLastFailedIds_.contains(dto->id));
    }
    updateHeaderSelectAllCheckState();
    recoveryRetryPending_ = true;
    hideRecoveryCompletePanel();
    QTimer::singleShot(0, this, &RecoveryScanPage::onRecoverAllClicked);
}

void RecoveryScanPage::exportRecoveryDetailReport()
{
    if (recoveryLastDetailText_.trimmed().isEmpty()) {
        QMessageBox::information(this, QStringLiteral("恢复结果"), QStringLiteral("当前没有可导出的明细。"));
        return;
    }
    const QString base = recoveryLastTargetDir_.isEmpty()
                             ? QDir::homePath()
                             : recoveryLastTargetDir_;
    const QString savePath = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("导出恢复明细"),
        QDir::toNativeSeparators(base + QStringLiteral("/recover_report.txt")),
        QStringLiteral("Text Files (*.txt);;All Files (*)"));
    if (savePath.isEmpty()) {
        return;
    }
    QString err;
    if (exportRecoverDetailsToFile(savePath, recoveryLastDetailText_, &err)) {
        QMessageBox::information(
            this,
            QStringLiteral("恢复结果"),
            QStringLiteral("恢复明细已导出到：\n%1").arg(QDir::toNativeSeparators(savePath)));
    } else {
        QMessageBox::warning(
            this,
            QStringLiteral("恢复结果"),
            QStringLiteral("导出失败：%1").arg(err));
    }
}

void RecoveryScanPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (recoveryCompletePanel_ && recoveryCompletePanel_->isVisible()) {
        recoveryCompletePanel_->setGeometry(rect().adjusted(18, 18, -18, -18));
        recoveryCompletePanel_->raise();
    }
}

void RecoveryScanPage::onPathTreeItemChanged(QTreeWidgetItem *item, int column)
{
    if (column != 0 || syncingTreeChecks_) {
        return;
    }
    syncingTreeChecks_ = true;
    if (item->checkState(0) == Qt::Checked || item->checkState(0) == Qt::Unchecked) {
        setTreeSubtreeCheckState(item, item->checkState(0));
    }
    updateParentCheckState(item);
    syncingTreeChecks_ = false;
    syncTableChecksFromTreeSelection();
}

void RecoveryScanPage::onTypeTreeItemChanged(QTreeWidgetItem *item, int column)
{
    if (column != 0 || syncingTreeChecks_) {
        return;
    }
    syncingTreeChecks_ = true;
    if (item->checkState(0) == Qt::Checked || item->checkState(0) == Qt::Unchecked) {
        setTreeSubtreeCheckState(item, item->checkState(0));
    }
    updateParentCheckState(item);
    syncingTreeChecks_ = false;
    syncTableChecksFromTreeSelection();
    refreshResultsHeader();
}



void RecoveryScanPage::onTypeTreeCurrentChanged(QTreeWidgetItem *current)
{
    Q_UNUSED(current);
    lastTypeRefreshKey_.clear();
    lastTypeRefreshItemCount_ = -1;
    refillTableFromTypeItem(current);
    if (tableView_ && filterModel_ && filterModel_->rowCount() > 0) {
        tableView_->selectRow(0);
    }
    refreshResultsHeader();
}

void RecoveryScanPage::onLeftTabChanged(int index)
{
    QElapsedTimer tabTimer;
    tabTimer.start();
    LOG_INFO(L"RecoveryScanPage::onLeftTabChanged"
             L" index=" + std::to_wstring(static_cast<uint64_t>(index)) +
             L" pathCurrent=[" + std::wstring(reinterpret_cast<const wchar_t *>((pathTree_ && pathTree_->currentItem()
                                                                                     ? itemRelativeFolderPath(pathTree_->currentItem())
                                                                                     : QString()).utf16()),
                                              static_cast<size_t>((pathTree_ && pathTree_->currentItem()
                                                                       ? itemRelativeFolderPath(pathTree_->currentItem())
                                                                       : QString()).size())) + L"]" +
             L" typeCurrent=[" + std::wstring(reinterpret_cast<const wchar_t *>((typeTree_ && typeTree_->currentItem()
                                                                                     ? typeTree_->currentItem()->data(0, Qt::UserRole + 6).toString()
                                                                                     : QString()).utf16()),
                                              static_cast<size_t>((typeTree_ && typeTree_->currentItem()
                                                                       ? typeTree_->currentItem()->data(0, Qt::UserRole + 6).toString()
                                                                       : QString()).size())) + L"]");
    if (pathTree_) {
        pathTree_->setVisible(index == 0);
    }
    if (typeTree_) {
        typeTree_->setVisible(index == 1);
    }
    if (index == 1 && typeTree_->topLevelItemCount() > 0 && !typeTree_->currentItem()) {
        lastTypeRefreshKey_.clear();
        lastTypeRefreshItemCount_ = -1;
        typeTree_->setCurrentItem(typeTree_->topLevelItem(0));
    }
    if (index == 0 && pathRoot_ && !pathTree_->currentItem()) {
        lastPathRefreshKey_.clear();
        lastPathRefreshItemCount_ = -1;
        pathTree_->setCurrentItem(pathRoot_);
    }
    refreshResultsHeader();
    if (tabRefreshScheduled_) {
        LOG_INFO(L"RecoveryScanPage::onLeftTabChanged coalesce pending refresh");
        return;
    }
    tabRefreshScheduled_ = true;
    QTimer::singleShot(0, this, [this, index, startedMs = tabTimer.elapsed()]() {
        tabRefreshScheduled_ = false;
        LOG_INFO(L"RecoveryScanPage::onLeftTabChanged deferred refresh"
                 L" index=" + std::to_wstring(static_cast<uint64_t>(index)) +
                 L" queuedAfterMs=" + std::to_wstring(static_cast<uint64_t>(startedMs)));
        QElapsedTimer refreshTimer;
        refreshTimer.start();
        refillTableFromSelection();
        if (tableView_ && filterModel_ && filterModel_->rowCount() > 0) {
            tableView_->selectRow(0);
        }
        refreshResultsHeader();
        LOG_INFO(L"RecoveryScanPage::onLeftTabChanged refresh done"
                 L" index=" + std::to_wstring(static_cast<uint64_t>(index)) +
                 L" rowCount=" + std::to_wstring(static_cast<uint64_t>(filterModel_ ? filterModel_->rowCount() : 0)) +
                 L" elapsedMs=" + std::to_wstring(static_cast<uint64_t>(refreshTimer.elapsed())));
    });
}

void RecoveryScanPage::updateDetailFromTableRow(int row)
{
    if (!scanModel_ || !previewContentLabel_ || !previewMetaLabel_ || row < 0) {
        return;
    }
    const ScanFileDto *dto = scanModel_->dtoAtRow(row);
    if (!dto) {
        return;
    }

    const ScanFileDto selected = *dto;
    previewCurrentFileId_ = selected.id;
    previewTitleLabel_->setText(selected.name);
    previewMetaLabel_->setText(QStringLiteral("%1 · %2\n%3")
                                   .arg(selected.isDirectory ? QStringLiteral("文件夹") : fileTypeDisplay(selected))
                                   .arg(selected.isDirectory ? QStringLiteral("—") : formatSize(selected.sizeBytes))
                                   .arg(selected.fullPath));
    previewContentLabel_->setPixmap(QPixmap());
    previewContentLabel_->setStyleSheet(QStringLiteral(
        "color: #9ca3af; font-size: 13px; background: transparent;"));
    previewContentLabel_->setText(QStringLiteral("正在读取预览…"));

    if (selected.isDirectory || !resultStore_ || diskLetter_.isEmpty()) {
        previewContentLabel_->setText(selected.isDirectory
                                          ? QStringLiteral("文件夹没有可预览内容")
                                          : QStringLiteral("当前扫描源不支持预览读取"));
        return;
    }

    const quint64 id = selected.id;
    const QString letter = diskLetter_;
    ScanResultStore *store = resultStore_;
    auto *job = new LambdaRunnable([this, id, letter, store]() {
        const QByteArray payload = loadPreviewThumbBytes(letter, store, id);
        QMetaObject::invokeMethod(this, [this, id, payload]() {
            if (id != previewCurrentFileId_ || !previewContentLabel_ || !resultStore_) {
                return;
            }

            ScanFileDto current;
            if (!resultStore_->tryGetFile(id, &current)) {
                return;
            }
            const QString ext = current.extension.trimmed().toLower();
            const bool archiveLike = QStringList{
                QStringLiteral("zip"), QStringLiteral("docx"), QStringLiteral("pptx"),
                QStringLiteral("xlsx"), QStringLiteral("odt"), QStringLiteral("ods"),
                QStringLiteral("odp"), QStringLiteral("epub")
            }.contains(ext);
            const bool textLike = QStringList{
                QStringLiteral("txt"), QStringLiteral("log"), QStringLiteral("csv"), QStringLiteral("json"),
                QStringLiteral("xml"), QStringLiteral("html"), QStringLiteral("htm"), QStringLiteral("md"),
                QStringLiteral("ini"), QStringLiteral("cfg"), QStringLiteral("cpp"), QStringLiteral("c"),
                QStringLiteral("h"), QStringLiteral("hpp"), QStringLiteral("yaml"), QStringLiteral("yml")
            }.contains(ext);

            const auto showFallback = [this, &payload, textLike]() {
                previewContentLabel_->setPixmap(QPixmap());
                previewContentLabel_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
                previewContentLabel_->setStyleSheet(QStringLiteral(
                    "color: #374151; font-family: Consolas, 'Courier New'; font-size: 12px;"
                    " background: #ffffff; border: 1px solid #e5e7eb; border-radius: 8px; padding: 12px;"));
                if (payload.isEmpty()) {
                    previewContentLabel_->setText(QStringLiteral("无法读取文件内容"));
                    return;
                }
                if (textLike) {
                    QString text = QString::fromUtf8(payload.left(512));
                    text.replace(QChar('\0'), QChar(' '));
                    previewContentLabel_->setText(text.left(100));
                    return;
                }
                const QByteArray firstBytes = payload.left(100);
                QStringList hexLines;
                for (int offset = 0; offset < firstBytes.size(); offset += 16) {
                    const QByteArray line = firstBytes.mid(offset, 16);
                    hexLines.append(QStringLiteral("%1  %2")
                                         .arg(offset, 4, 16, QLatin1Char('0'))
                                         .arg(QString::fromLatin1(line.toHex(' '))));
                }
                previewContentLabel_->setText(QStringLiteral("前 100 字节（十六进制）\n") + hexLines.join(QLatin1Char('\n')));
            };

            if (payload.isEmpty()) {
                showFallback();
                return;
            }

            if (GridPreview::isImageExt(ext)) {
                QImage image;
                if (image.loadFromData(payload)) {
                    previewContentLabel_->setAlignment(Qt::AlignCenter);
                    previewContentLabel_->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
                    previewContentLabel_->setText(QString());
                    previewContentLabel_->setPixmap(QPixmap::fromImage(
                        image.scaled(420, 280, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                    return;
                }
            }

            if (GridPreview::isVideoExt(ext) || GridPreview::isDocumentExt(ext)) {
                QString safeExt = ext;
                if (safeExt.isEmpty() || safeExt.size() > 8) safeExt = QStringLiteral("bin");
                QTemporaryFile temp(QDir::tempPath() + QStringLiteral("/phxrv_preview_XXXXXX.") + safeExt);
                temp.setAutoRemove(false);
                const QString tempPath = temp.fileName();
                QPixmap pixmap;
                QStringList archiveEntries;
                if (temp.open() && temp.write(payload) == payload.size()) {
                    temp.flush();
                    temp.close();
                    pixmap = GridPreview::isVideoExt(ext)
                                 ? MediaThumbnails::videoPreviewPixmapForLocalFile(tempPath)
                                 : MediaThumbnails::documentPreviewPixmapForLocalFile(tempPath);
                    if (pixmap.isNull() && archiveLike) {
                        archiveEntries = listZipLocalEntryNamesFromPath(tempPath);
                    }
                } else {
                    temp.close();
                }
                QFile::remove(tempPath);
                if (!pixmap.isNull()) {
                    previewContentLabel_->setAlignment(Qt::AlignCenter);
                    previewContentLabel_->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
                    previewContentLabel_->setText(GridPreview::isVideoExt(ext)
                                                      ? QStringLiteral("视频第一帧")
                                                      : QString());
                    previewContentLabel_->setPixmap(pixmap.scaled(420, 280, Qt::KeepAspectRatio,
                                                                  Qt::SmoothTransformation));
                    return;
                }
                if (!archiveEntries.isEmpty()) {
                    previewContentLabel_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
                    previewContentLabel_->setStyleSheet(QStringLiteral(
                        "color: #374151; font-family: Consolas, 'Courier New'; font-size: 12px;"
                        " background: #ffffff; border: 1px solid #e5e7eb; border-radius: 8px; padding: 12px;"));
                    QStringList lines;
                    lines << QStringLiteral("容器结构预览")
                          << QStringLiteral("条目数量：%1").arg(archiveEntries.size());
                    const int limit = qMin(30, archiveEntries.size());
                    for (int i = 0; i < limit; ++i) {
                        lines << QStringLiteral("• %1").arg(archiveEntries.at(i));
                    }
                    if (archiveEntries.size() > limit) {
                        lines << QStringLiteral("… 其余 %1 个条目未展开")
                                      .arg(archiveEntries.size() - limit);
                    }
                    previewContentLabel_->setText(lines.join(QLatin1Char('\n')));
                    return;
                }
            }

            showFallback();
        }, Qt::QueuedConnection);
    });
    job->setAutoDelete(true);
    QThreadPool::globalInstance()->start(job);
}
