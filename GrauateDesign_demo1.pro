QT += core gui serialport sql charts serialbus printsupport

include(QXlsx/QXlsx.pri)
QT += concurrent  # 用于多线程操作

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

CONFIG += debug

QMAKE_CXXFLAGS += -g
QMAKE_LFLAGS += -g

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

# 指定生成的 UI 头文件输出到构建目录下的 ui_headers 子目录
UI_DIR = $$OUT_PWD/ui_headers      # UI 头文件路径
MOC_DIR = $$OUT_PWD/mocs           # MOC 生成文件路径
OBJECTS_DIR = $$OUT_PWD/objs       # 编译中间文件路径

# 添加头文件搜索路径
INCLUDEPATH += $$UI_DIR            # 告诉编译器从此目录查找 UI 头文件

SOURCES += \
    blackbodycontroller.cpp \
    calibrationmanager.cpp \
    customtitlebar.cpp \
    database.cpp \
    dataexcelprocessor.cpp \
    dualtemperaturechart.cpp \
    humiditycontroller.cpp \
    loginwindow.cpp \
    main.cpp \
    mainwindow.cpp \
    modelingpointdialog.cpp \
    pythonprocessor.cpp \
    serialportthread.cpp

HEADERS += \
    blackbodycontroller.h \
    calibrationmanager.h \
    customtitlebar.h \
    database.h \
    dataexcelprocessor.h \
    dualtemperaturechart.h \
    humiditycontroller.h \
    loginwindow.h \
    mainwindow.h \
    modelingpointdialog.h \
    pythonprocessor.h \
    serialportthread.h

FORMS += \
    loginwindow.ui \
    mainwindow.ui

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    fonts.qrc \
    pictures.qrc
