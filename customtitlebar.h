// customtitlebar.h
#ifndef CUSTOMTITLEBAR_H
#define CUSTOMTITLEBAR_H

#include <QWidget>
#include <QPushButton>
#include <QLabel>  // 新增 QLabel 头文件
#include <QMouseEvent>

class CustomTitleBar : public QWidget {
    Q_OBJECT

public:
    explicit CustomTitleBar(QWidget *parent = nullptr);

signals:
    void minimizeClicked();
    void maximizeRestoreClicked();
    void closeClicked();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    QPushButton* createCustomButton(const QString &iconPath, const QString &toolTip);
    void initLayout();

    // 新增成员变量：软件图标标签
    QLabel *m_iconLabel;       // 软件图标
    QLabel *m_titleLabel;      // 标题标签
    QPushButton *m_minimizeButton;
    QPushButton *m_maximizeRestoreButton;
    QPushButton *m_closeButton;
    QPoint lastPos;
};

#endif // CUSTOMTITLEBAR_H
