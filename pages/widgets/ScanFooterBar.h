#pragma once

#include <QtWidgets/QWidget>

class QLabel;
class QPushButton;
class QCheckBox;

class ScanFooterBar : public QWidget
{
    Q_OBJECT

public:
    explicit ScanFooterBar(QWidget *parent = nullptr);

    void setFileCount(int total, int checked);
    void setEnabled(bool enabled);
    void setResultsMode(bool enabled);
    bool keepDirStructure() const;

    QPushButton *recoverAllBtn() const { return recoverAllBtn_; }
    QCheckBox *keepDirStructureCheck() const { return keepDirStructureCheck_; }

signals:
    void recoverClicked();
    void exportClicked();

private:
    QLabel *footerSubLabel_ = nullptr;
    QPushButton *recoverAllBtn_ = nullptr;
    QCheckBox *keepDirStructureCheck_ = nullptr;
};
