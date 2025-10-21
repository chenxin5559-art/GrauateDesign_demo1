#include "loginwindow.h"
#include "ui_loginwindow.h"
#include <QMessageBox> // 用于弹出提示框
#include <QUuid>                // 添加 QUuid 的支持
#include <QCryptographicHash>   // 添加 QCryptographicHash 的支持
#include <QSqlQuery>

LoginWindow::LoginWindow(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::LoginWindow)
    ,settings(new QSettings("MyCompany", "MyApp", this)) // 初始化 QSettings
{
    ui->setupUi(this);

    // 初始化数据库
    if (!m_db.initialize()) {
        QMessageBox::critical(this, "错误", "数据库初始化失败！");
        close();
        return;
    }

    // 绑定按钮事件
    connect(ui->loginButton, &QPushButton::clicked, this, &LoginWindow::onLoginClicked);
    connect(ui->cancelButton, &QPushButton::clicked, this, &LoginWindow::onCancelClicked);

    // 设置窗口标题
    this->setWindowTitle("用户登录");

    // 加载本地保存的用户名和密码
    loadSettings();
}

LoginWindow::~LoginWindow()
{
    delete ui;
}

void LoginWindow::saveSettings() {
    QString username = ui->usernameLineEdit->text();
    bool rememberPassword = ui->rememberPasswordCheckBox->isChecked();

    settings->setValue("username", username);
    settings->setValue("rememberPassword", rememberPassword);

    if (rememberPassword) {
        // 直接保存密码明文
        settings->setValue("password_plain", m_plainPassword);
    } else {
        settings->remove("password_plain");
    }
}

void LoginWindow::loadSettings() {
    QString username = settings->value("username", "").toString();
    QString password = settings->value("password_plain", "").toString();
    bool rememberPassword = settings->value("rememberPassword", false).toBool();

    ui->usernameLineEdit->setText(username);
    ui->rememberPasswordCheckBox->setChecked(rememberPassword);

    // 直接填充密码明文
    if (rememberPassword) {
        ui->passwordLineEdit->setText(password);
    }
}

// 登录按钮点击事件
void LoginWindow::onLoginClicked() {
    QString username = ui->usernameLineEdit->text();
    QString password = ui->passwordLineEdit->text();

    // 临时存储密码明文
    m_plainPassword = password;

    if (m_db.validateUser(username, password)) {
        saveSettings(); // 保存明文
        accept();
    } else {
        QMessageBox::warning(this, "失败", "验证失败");
    }
}

// 取消按钮点击事件
void LoginWindow::onCancelClicked()
{
    reject(); // 关闭登录窗口，并返回失败状态
}
