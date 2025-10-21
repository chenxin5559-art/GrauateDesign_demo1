#ifndef LOGINWINDOW_H
#define LOGINWINDOW_H

#include <QDialog>
#include <QSettings>
#include "database.h"  // 添加数据库头文件

namespace Ui {
class LoginWindow;
}

class LoginWindow : public QDialog
{
    Q_OBJECT

public:
    explicit LoginWindow(QWidget *parent = nullptr);
    ~LoginWindow();

private slots:
    void onLoginClicked();  // 登录按钮点击事件
    void onCancelClicked(); // 取消按钮点击事件

private:
    Ui::LoginWindow *ui;
    QSettings *settings; // 用于保存登录信息
    QString m_plainPassword; // 临时存储密码明文

    void loadSettings(); // 加载本地存储的信息
    void saveSettings(); // 保存用户选择的信息
    Database m_db;  // 数据库实例

};

#endif // LOGINWINDOW_H
