#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSerialPort>
#include <QSerialPortInfo>
#include "BlackbodyController.h"
#include <QProgressDialog>
#include "dataexcelprocessor.h"
#include "pythonprocessor.h"
#include "HumidityController.h"
#include <QMutex>
#include "calibrationmanager.h"
#include "customtitlebar.h"
#include "serialportthread.h"
#include <QComboBox>
#include <QTextEdit>
#include <QCheckBox>
#include <QMessageBox>
#include <QFileDialog>
#include <QDesktopServices>
#include <QHeaderView>
#include <QUrl>
#include "dualtemperaturechart.h"
#include "ServoMotorController.h"

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class ModelingPointDialog;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    // 获取指定COM口的红外平均数据
    CalibrationManager::InfraredData getIrAverage(const QString& comPort);

private slots:
    // 新增恒温恒湿箱相关槽函数
    void setupHumidityControls();          // 初始化湿度控制
    void onHumidityConnectionStatusChanged(bool connected);  // 连接状态变化
    void onHumidityMasterControlChanged(bool acquired);      // 控制权变化
    void updateCurrentTemperature(float temp);               // 更新当前温度显示
    void updateCurrentHumidity(float humidity);              // 更新当前湿度显示
    void onSetTargetTemperatureClicked();                     // 设置目标温度
    void onStartStopButtonClicked(bool checked);             // 启动/停止设备
    void onChangeSensorButtonClicked(int direction);         // 更换测温仪（上一个/下一个）
    void onToggleCalibrationWindowClicked();                 // 打开/关闭标定窗口
    void onOpenComButton2Clicked();       // 处理OpenComButton_2点击
    void updateHumidityPortComboBox();    // 更新端口选择框

    void handleTemperatureUpdate(float temp);
    void onEnableSaveToggled(bool checked);
    void onSelectPathClicked();
    void saveTemperatureData(float temp);
    void onOpenComButtonClicked();
    void updatePortComboBox();

    // 按钮点击处理
    void onSingleHeadButtonClicked();
    void onMultiHeadButtonClicked();

    // 处理器信号响应
    void handleOperationCompleted(bool success, const QString& resultPath);
    void handleError(const QString& message);

    void onPythonProgress(const QString& message);
    void onPythonFinished(bool success, const QString& path);
    void onPythonError(const QString& error);

    void handleProgressUpdated(int percent, const QString& message);

    void handleTemperatureUpdate2(float temp);
    void handleHumidityUpdate(float humidity);
    void onHumidityEnableSaveToggled(bool checked);
    void onHumiditySelectPathClicked();

    void onStartCalibrationClicked();
    void onPauseResumeClicked();      // 新增
    void onCancelClicked();           // 新增
    void onCalibrationStateChanged(CalibrationManager::State state); // 新增
    void updateButtonStates();

    void onCalibrationFinished();
    void onCalibrationError(const QString &error);

    // 修改原进度更新槽函数为日志更新
    void updateOperationLog(const QString &progressText);
    // 新增倒计时显示槽函数
    void updateCountdownDisplay(int remainingSeconds, const QString &stage);

    void updateCalibrationProgress(int progress);


    void onIntegratedProcessClicked();
    QString generateIntegratedTemplate(QXlsx::Document& srcDoc,
                                       const QString& sheetName,
                                       const QVector<bool>& selections,
                                       const QString& sourceFilePath);
    void processAllTemplatesFitting();
    void processIntegratedTemplatesWithDialog(const QString& mergedFilePath);
    void processNextTemplate();

    void onIntegratedMultiProcessClicked();

    void onBrowseButtonClicked(); // 添加文件选择槽函数声明

    void loadConfigToUI();  // 从配置文件加载到UI控件
    void on_saveConfigButton_clicked();  // 保存按钮点击事件（Qt自动关联命名）

    // 标定类型选择相关
    void on_calibrationTypeComboBox_currentIndexChanged(int index);
    // 标定配置保存
    void on_saveCalibrationConfigButton_clicked();

    void onIrMeasurementStarted(const QString &comPort); // 红外测量开始
    void onIrMeasurementStopped(); // 红外测量结束
    void updateIrChartFromTable(); // 从表格更新红外图表数据

private:
    Ui::MainWindow *ui;

    // 确保声明为QTabWidget
    QTabWidget *IRTCommTab;

    ServoMotorController *m_servoController;
    BlackbodyController *m_blackbodyController;
    HumidityController *m_humidityController = nullptr;
    void setupBlackbodyControls();
    void onConnectionStatusChanged(bool connected);
    QList<QPair<QDateTime, float>> temperatureHistory;
    QString currentSavePath;
    bool saveEnabled = false;
    DataExcelProcessor *excelProcessor;
    void setupConnections();
    void populatePortComboBox();
    QSettings *m_settings;

    void populateHumidityPortComboBox();  // 填充湿度控制器端口列表
    QSettings* m_humiditySettings;       // 湿度控制器独立配置

    void initProgressDialog();
    void processMergedFile(const QString& filePath);
    QString processSelectedData(QXlsx::Document& srcDoc,
                                const QString& sheetName,
                                const QVector<bool>& selections,
                                const QString& sourceFilePath);
    PythonProcessor *m_pythonProcessor;
    QProgressDialog* m_progressDialog;
    void onMergeSingleHeadClicked();
    void processSingleHeadFile(const QString& filePath);
    void generateTemplateFile(QXlsx::Document& srcDoc,
                              const QString& sheetName,
                              const QVector<bool>& selections,
                              const QString& sourceFilePath);
    QTimer* humidityTimer;

    QList<QPair<QDateTime, float>> temperatureHistory2;    // 恒温箱温度历史数据
    QList<QPair<QDateTime, float>> humidityHistory;       // 湿度历史数据

    bool humiditySaveEnabled = false;
    QString humiditySavePath;
    QString m_blackbodySavePath; // 黑体炉保存路径变量（新增）

    CalibrationManager *m_calibrationManager;
    void setupCalibrationManager();
    void checkAutoSaveSettings();
    QProgressBar *calibrationProgressBar;
    int calibrationButtonClickCount = 0;

    CustomTitleBar *m_customTitleBar;
    void setupCustomTitleBar();

    QString m_integratedMergedPath;
    QVector<QString> m_integratedTemplatePaths;
    QStringList m_templateQueue;            // 模板处理队列（新增）
    int m_currentTemplateIndex;             // 当前处理索引（新增）

    QString m_mergedFilePath; // 保存合并后的文件路径
    QStringList m_templateFiles; // 保存生成的模板文件路径列表
    QMap<QString, QVector<bool>> m_sheetSelections; // 保存每个工作表的选择状态
    QString m_currentSheet; // 当前正在处理的工作表

    // 新增私有函数声明
    void processIntegratedMergedFileWithDialog(const QString& mergedFilePath);
    void processAllMultiTemplatesFitting();
    void processNextMultiTemplate();


    // 添加IRTCommTab相关成员
    void onPortControlButtonClicked(const QString &portName,
                                    QComboBox *baudRateComboBox,
                                    QPushButton *button,
                                    QLabel *statusLabel);

    // 串口和图表相关成员
    void setupIRTCommTab();
    void initializePortComboBox(QComboBox *comboBox, const QString &defaultPort);
    void initializeBaudRateComboBox(QComboBox *comboBox);
    void loadCommonCommands(QComboBox *commandComboBox);
    void addCommandToCommonList(QComboBox* comboBox, const QString& cmd);
    void initializeSerialPort(int index, const QString &portName,
                              QTextEdit *receiveTextEdit,
                              QLineEdit *filePathEdit,
                              QCheckBox *saveCheckBox,
                              QComboBox *portComboBox,
                              QComboBox *baudRateComboBox,
                              QPushButton *portControlBtn,
                              QLabel *statusLabel);

    QVector<SerialPortThread*> m_serialThreads;
    QReadWriteLock m_dataLock;

    QMap<QString, int> portRowMap; // 串口号与表格行的映射（成员变量）
    QMutex portRowMapMutex;       // 保护映射的互斥锁

    bool calibrationInProgress = false; // 新增

    bool blackbodyPathErrorShown = false;
    bool humidityTempPathErrorShown = false;
    bool humidityPathErrorShown = false;

    QString m_lastOperation; // 记录上一次操作，用于区分新操作和倒计时更新

    DualTemperatureChart *m_dualTempChart; // 双温度曲线图表

    QTimer *m_irDataTimer; // 定时从表格提取红外数据的定时器
    QString m_currentIrComPort; // 当前正在测量的红外COM口
    QTableWidget *m_tempTable; // 指向IRTCommTab第一标签的表格

    // 在MainWindow的private成员中修改缓存定义
    QMap<QString, QVector<QPair<QPair<float, float>, float>>> m_irSingleCache; // 单头：<<TO1, TA1>, LC1>
    QMap<QString, QVector<QPair<QPair<QVector<float>, QVector<float>>, QVector<float>>>> m_irMultiCache; // 多头：<<TOs, TAs>, LCs>
    QMutex m_irCacheMutex; // 缓存锁（线程安全）

signals:
    void newTemperatureData(QDateTime time, float temp);
    void newTemperatureData2(QDateTime time, float temp);
    void newHumidityData(QDateTime time, float humi);

    // 新增：传递测试员和审核员信息给Python处理器
    void sendTesterReviewerInfo(const QString& tester, const QString& reviewer);

};
#endif // MAINWINDOW_H
