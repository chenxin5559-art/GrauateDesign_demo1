#include "mainwindow.h"
#include "loginwindow.h"
#include <QApplication>
#include <QFontDatabase>  // 字体数据库头文件
#include <QIcon>

int main(int argc, char *argv[]) {
    QApplication a(argc, argv);

    // ====================== 字体加载逻辑 ======================
    // 加载字体文件（假设资源路径为 ":/fonts/PingFang.ttf"，需与 qrc 文件一致）
    int fontId = QFontDatabase::addApplicationFont(":/fonts/PingFang Regular_0.ttf");
    if (fontId == -1) {
        qWarning() << "错误：字体文件加载失败，请检查资源路径是否正确";
    } else {
        // 获取字体家族名称（可能包含多个，如 "PingFang SC"）
        QStringList fontFamilies = QFontDatabase::applicationFontFamilies(fontId);
        if (!fontFamilies.isEmpty()) {
            // 设置全局字体（整个应用使用该字体）
            QFont globalFont(fontFamilies.first());
            globalFont.setPixelSize(14);  // 设定字体大小（避免高 DPI 模糊）
            globalFont.setStyleStrategy(QFont::PreferAntialias);  // 启用抗锯齿
            a.setFont(globalFont);  // 应用到整个应用程序
            qDebug() << "成功加载字体：" << fontFamilies.first();
        } else {
            qWarning() << "错误：字体文件中未找到有效字体家族";
        }
    }

    // ====================== 设置窗口图标 ======================
    // 加载图标
    QIcon icon(":/images/images/software-icon.png"); // 假设图标在资源文件的 /images 前缀下
    if (!icon.isNull()) {
        a.setWindowIcon(icon);
    }


    // ====================== 登录与主窗口逻辑 ======================
    LoginWindow login;
    if (login.exec() == QDialog::Accepted) {
        MainWindow mainWindow;
        mainWindow.show();
        return a.exec();
    }

    return 0;
}
