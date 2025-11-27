#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QSettings>
#include "modelingpointdialog.h"
#include <QTableWidget>
#include "dataexcelprocessor.h"
#include <QWidget> // 新增：确保识别 QWidget 的信号

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_blackbodyController(nullptr) // 显式初始化指针
    , excelProcessor(new DataExcelProcessor(this))
    ,m_pythonProcessor(new PythonProcessor(this))
    ,m_progressDialog(new QProgressDialog(this))
    ,m_settings(new QSettings("config.ini", QSettings::IniFormat))
    , m_humiditySettings(new QSettings("config.ini", QSettings::IniFormat))
    , m_lastOperation("") // 初始化上次操作记录
{
    m_progressDialog->reset();
    ui->setupUi(this);

    // 隐藏系统默认的标题栏
    setWindowFlags(Qt::FramelessWindowHint);

    // 设置自定义标题栏
    setupCustomTitleBar();

    // 强制设置 centralWidget 的尺寸
    // centralWidget()->setGeometry(rect());

    // 设置窗口的初始尺寸
    // setGeometry(100, 100, 1551, 985);

    // 输出调试信息
    qDebug() << "MainWindow geometry:" << geometry();
    qDebug() << "MainWindow centralWidget geometry:" << centralWidget()->geometry();

    // 初始化恒温恒湿箱控制（新增）
    setupHumidityControls();

    // 【新增】初始化伺服电机控制器
    m_servoController = new ServoMotorController(this);

    // 连接信号槽
    setupConnections();

    // 添加窗口标题设置（关键修改行）
    setWindowTitle("红外测温仪自动标校软件"); // 此处修改为你想要的窗口名

    // 连接按钮信号
    connect(ui->singleHeadButton, &QPushButton::clicked, this, &MainWindow::onSingleHeadButtonClicked);
    connect(ui->multiHeadButton, &QPushButton::clicked, this, &MainWindow::onMultiHeadButtonClicked);

    connect(ui->integratedProcessButton, &QPushButton::clicked, this, &MainWindow::onIntegratedProcessClicked);

    connect(ui->allInOneButton, &QPushButton::clicked, this, &MainWindow::onIntegratedMultiProcessClicked);

    connect(m_pythonProcessor, &PythonProcessor::progressChanged,
            this, &MainWindow::onPythonProgress);
    connect(m_pythonProcessor, &PythonProcessor::processingFinished,
            this, &MainWindow::onPythonFinished);
    connect(m_pythonProcessor, &PythonProcessor::errorOccurred,
            this, &MainWindow::onPythonError);

    // 初始化端口选择控件
    populatePortComboBox();

    // 连接端口控制按钮信号
    connect(ui->OpenComButton, &QPushButton::clicked, this, &MainWindow::onOpenComButtonClicked);
    connect(ui->COMcomboBox, &QComboBox::currentTextChanged, this, &MainWindow::updatePortComboBox);

    // 初始化湿度控制器端口选择
    populateHumidityPortComboBox();

    // 连接湿度控制器端口控制信号
    connect(ui->OpenComButton_2, &QPushButton::clicked, this, &MainWindow::onOpenComButton2Clicked);
    connect(ui->COMcomboBox_2, &QComboBox::currentTextChanged, this, &MainWindow::updateHumidityPortComboBox);

    ui->statusLabel->setStyleSheet(
        "QLabel {"
        "   background-color: #FF0000;"
        "   border-radius: 8px;"
        "   min-width: 16px;"
        "   min-height: 16px;"
        "}"
        );
    ui->statusLabel->setText("未连接");  // 设置初始文本

    // 先初始化黑体炉控制器
    setupBlackbodyControls(); // 新增调用

     // 设置初始状态
     ui->enableSaveCheckBox->setChecked(false); // 默认不勾选
     ui->savePathLineEdit->clear();             // 清空路径显示

     // 连接保存相关信号
     connect(ui->enableSaveCheckBox, &QCheckBox::toggled,
             this, &MainWindow::onEnableSaveToggled);
     connect(ui->selectPathButton, &QPushButton::clicked,
             this, &MainWindow::onSelectPathClicked);

     connect(m_pythonProcessor, &PythonProcessor::progressUpdated,
             this, &MainWindow::handleProgressUpdated);

     m_progressDialog->setWindowTitle("正在处理");
     m_progressDialog->setCancelButtonText("取消");
     m_progressDialog->setRange(0, 100);
     m_progressDialog->setAutoClose(true);

     // 连接取消按钮
     connect(m_progressDialog, &QProgressDialog::canceled, [this]{
         m_pythonProcessor->terminateProcess(); // 需要添加终止方法
         m_progressDialog->hide();
     });

     // 初始化IRTCommTab
     setupIRTCommTab();

     setupCalibrationManager();

     // 初始化 UI 中的进度条
     ui->calibrationProgressBar->setRange(0, 100);
     ui->calibrationProgressBar->setValue(0);


     // 连接 CalibrationManager 的进度信号
     connect(m_calibrationManager, &CalibrationManager::calibrationProgress, this, [this](int progress) {
         ui->calibrationProgressBar->setValue(progress);
         ui->calibrationProgressBar->show();
     });

     // 初始化操作日志控件
     ui->operationLogTextEdit->setReadOnly(true);
     ui->operationLogTextEdit->setFont(QFont("Consolas", 9)); // 使用等宽字体
     ui->operationLogTextEdit->setStyleSheet(R"(
        QTextEdit {
            background-color: #f8f8f8;
            border: 1px solid #ccc;
            border-radius: 4px;
            padding: 5px;
        }
    )");
     ui->operationLogTextEdit->setMinimumHeight(200); // 设置最小高度

     // 修改信号连接：将原updateProgressLabel改为updateOperationLog
     connect(m_calibrationManager, &CalibrationManager::currentOperationChanged,
             this, &MainWindow::updateOperationLog);
     // 连接倒计时信号（如果之前添加了该信号）
     connect(m_calibrationManager, &CalibrationManager::countdownUpdated,
             this, &MainWindow::updateCountdownDisplay);

     // ====== 修改：分别读取设备专属保存路径 ======
     // 1. 黑体炉路径：优先读取 blackbody/save_path
     QString blackbodySavePath = m_settings->value("blackbody/save_path",  // 读取[blackbody]节点的save_path
                                                   QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();

     // 2. 恒温箱路径：优先读取 humidity/save_path
     QString humiditySavePath1 = m_settings->value("humidity/save_path",    // 读取[humidity]节点的save_path
                                                  QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();


     // 填充黑体炉保存路径
     ui->savePathLineEdit->setText(blackbodySavePath);
     m_blackbodySavePath = blackbodySavePath;  // 若已添加黑体炉变量，同步赋值
     ui->enableSaveCheckBox->setChecked(true);
     saveEnabled = true;


     // 填充恒温箱保存路径
     ui->savePathLineEdit_3->setText(humiditySavePath1);
     this->humiditySavePath = humiditySavePath1;  // 同步恒温箱路径变量
     ui->enableSaveCheckBox_3->setChecked(true);
     humiditySaveEnabled = true;



     // ============== 新增：自动连接串口逻辑 ==============
     // 使用单次定时器延迟执行（确保UI和控制器初始化完成）
     QTimer::singleShot(500, this, [this]() {
         // 1. 自动连接黑体炉串口
         if (m_blackbodyController && !m_blackbodyController->isConnected()) {
             QString bbPort = m_settings->value("blackbody/com_port", "COM10").toString();
             bool bbSuccess = m_blackbodyController->connectDevice(bbPort);
             if (bbSuccess) {
                 qDebug() << "黑体炉串口自动连接成功，端口：" << bbPort;
                 ui->OpenComButton->setText("关闭"); // 更新按钮文本

                 // 新增：自动获取黑体炉上位机控制（相当于点击masterControlButton）
                 QTimer::singleShot(1000, this, [this]() { // 延迟1秒确保连接稳定
                     if (ui->masterControlButton && !ui->masterControlButton->isChecked()) {
                         ui->masterControlButton->click(); // 模拟点击"获取控制"按钮
                         qDebug() << "已自动获取黑体炉上位机控制";
                     }
                 });
             } else {
                 qWarning() << "黑体炉串口自动连接失败，端口：" << bbPort;
                 QMessageBox::warning(this, "连接提示", "黑体炉串口自动连接失败，可手动尝试连接");
             }
         }

         // 2. 自动连接恒温箱串口
         if (m_humidityController && !m_humidityController->isConnected()) {
             QString humPort = m_humiditySettings->value("humidity/com_port", "COM12").toString();
             bool humSuccess = m_humidityController->connectDevice(humPort);
             if (humSuccess) {
                 qDebug() << "恒温箱串口自动连接成功，端口：" << humPort;
                 ui->OpenComButton_2->setText("关闭"); // 更新按钮文本

                 // 新增：自动获取恒温箱上位机控制（相当于点击masterControlButton_2）
                 QTimer::singleShot(1000, this, [this]() { // 延迟1秒确保连接稳定
                     if (ui->masterControlButton_2 && !ui->masterControlButton_2->isChecked()) {
                         ui->masterControlButton_2->click(); // 模拟点击"获取控制"按钮
                         qDebug() << "已自动获取恒温箱上位机控制";
                     }
                 });
             } else {
                 qWarning() << "恒温箱串口自动连接失败，端口：" << humPort;
                 QMessageBox::warning(this, "连接提示", "恒温箱串口自动连接失败，可手动尝试连接");
             }
         }
     });

     // 加载配置到UI控件（新增）
     loadConfigToUI();

     // 在 MainWindow 构造函数中添加连接（初始化UI后）
     connect(ui->savePathLineEdit_3, &QLineEdit::textChanged, this, [this](const QString& text) {
         humiditySavePath = text.trimmed(); // 输入框内容变化时同步变量
     });

     // 连接信号
     connect(ui->startCalibrationButton, &QPushButton::clicked, this, &MainWindow::onStartCalibrationClicked);
     connect(ui->pauseResumeButton, &QPushButton::clicked, this, &MainWindow::onPauseResumeClicked);
     connect(ui->cancelButton, &QPushButton::clicked, this, &MainWindow::onCancelClicked);
     connect(m_calibrationManager, &CalibrationManager::stateChanged, this, &MainWindow::onCalibrationStateChanged);

     // 初始状态
     updateButtonStates();

     // 初始化标定类型选择下拉框
     ui->calibrationTypeComboBox->addItems({"单头箱内", "单头箱外", "多头箱内", "多头箱外"});
     // 连接类型选择信号
     connect(ui->calibrationTypeComboBox, QOverload<int>::of(&QComboBox::currentIndexChanged),
             this, &MainWindow::on_calibrationTypeComboBox_currentIndexChanged);

     // 初始化双温度曲线图表（放在主窗口的合适位置，例如新增的QWidget中）
     m_dualTempChart = new DualTemperatureChart(this);
     // 将图表添加到UI布局（假设UI中有一个名为"chartContainer"的QWidget作为容器）
     ui->chartContainer->setLayout(new QVBoxLayout());
     ui->chartContainer->layout()->addWidget(m_dualTempChart);

     // 关联温度数据更新信号
     // 黑体炉温度 -> 红色曲线
     connect(this, &MainWindow::newTemperatureData,
             m_dualTempChart, &DualTemperatureChart::updateBlackbodyData);
     // 恒温箱温度 -> 蓝色曲线
     connect(this, &MainWindow::newTemperatureData2,
             m_dualTempChart, &DualTemperatureChart::updateHumidityBoxData);

     // 连接CalibrationManager的红外测量信号
     connect(m_calibrationManager, &CalibrationManager::irMeasurementStarted,
             this, &MainWindow::onIrMeasurementStarted);
     connect(m_calibrationManager, &CalibrationManager::irMeasurementStopped,
             this, &MainWindow::onIrMeasurementStopped);

     // 初始化定时器（1秒刷新一次红外数据）
     m_irDataTimer = new QTimer(this);
     m_irDataTimer->setInterval(1000);
     connect(m_irDataTimer, &QTimer::timeout, this, &MainWindow::updateIrChartFromTable);

     connect(m_calibrationManager, &CalibrationManager::requestIrAverage,
             this, [this](const QString& comPort, QObject* receiver) {
                 // 调用计算接口
                 auto irData = getIrAverage(comPort);
                 // 发送结果给CalibrationManager，同时传递COM口
                 QMetaObject::invokeMethod(receiver, "onIrAverageReceived",
                                           Qt::QueuedConnection,
                                           Q_ARG(QString, comPort),
                                           Q_ARG(CalibrationManager::InfraredData, irData));
             });

}

MainWindow::~MainWindow()
{
    // 清理串口线程
    for (SerialPortThread* thread : m_serialThreads) {
        if (thread) {
            thread->closePort();
            if (!thread->wait(1000)) {
                thread->terminate();
                thread->wait();
            }
            delete thread;
        }
    }

    if (m_blackbodyController) {
        disconnect(m_blackbodyController, nullptr, this, nullptr);
        delete m_blackbodyController;
        m_blackbodyController = nullptr;
    }

    if (m_humidityController) {
        disconnect(m_humidityController, nullptr, this, nullptr);
        delete m_humidityController;
        m_humidityController = nullptr;
    }

    if (humidityTimer) { // 显式清理成员定时器
        humidityTimer->stop();
        delete humidityTimer;
        humidityTimer = nullptr;
    }

    delete ui;

    delete m_calibrationManager;
}

void MainWindow::setupBlackbodyControls()
{
    // 读取配置文件
    QSettings settings("config.ini", QSettings::IniFormat);
    QString portName = settings.value("blackbody/com_port", "COM10").toString(); // 默认COM10

    // 初始化控制器（传入配置的端口）
    m_blackbodyController = new BlackbodyController(this);

    // 初始化定时器
    QTimer *timer = new QTimer(this);
    timer->setInterval(1000);

    // 初始禁用操作按钮
    ui->setTempButton->setEnabled(false);
    ui->startStopButton->setEnabled(false);

    // 控制权状态控制定时器
    connect(m_blackbodyController, &BlackbodyController::masterControlChanged, timer, [timer](bool acquired) {
        acquired ? timer->start() : timer->stop();
    });

    connect(timer, &QTimer::timeout, m_blackbodyController, &BlackbodyController::readCurrentTemperature);

    // 连接状态信号
    connect(m_blackbodyController, &BlackbodyController::connectionStatusChanged,
            this, [this](bool connected) {


                qDebug() << "Connection Status Changed:" << connected;

                // 根据连接状态更新按钮文本
                ui->OpenComButton->setText(connected ? "关闭" : "打开");
                ui->COMcomboBox->setEnabled(!connected);

                QString color = connected ? "#00FF00" : "#FF0000";
                ui->statusLabel->setStyleSheet(QString("background-color: %1; border-radius: 8px;").arg(color));
                ui->statusLabel->setText(connected ? "已连接" : "未连接");
                ui->statusLabel->setToolTip(connected ? "已连接" : "未连接");

                ui->masterControlButton->setEnabled(connected);
                if (!connected) {
                    ui->masterControlButton->setChecked(false);
                    ui->masterControlButton->setText("获取控制");
                }

                // 断开连接时强制释放控制权
                if (!connected) {
                    if (ui->masterControlButton->isChecked()) {
                        m_blackbodyController->setMasterControl(false);
                    }
                    ui->masterControlButton->setChecked(false);
                    ui->masterControlButton->setText("获取控制");
                    ui->setTempButton->setEnabled(false);
                    ui->startStopButton->setEnabled(false);
                }
            });

    // 控制权状态更新操作按钮
    connect(m_blackbodyController, &BlackbodyController::masterControlChanged, this, [this](bool acquired) {
        // 根据实际结果更新UI
        ui->masterControlButton->setChecked(acquired);
        ui->masterControlButton->setText(acquired ? "释放控制" : "获取控制");
        ui->setTempButton->setEnabled(acquired);
        ui->startStopButton->setEnabled(acquired && !ui->targetTempInput->text().isEmpty());
    });

    // 连接设备
    // if (!m_blackbodyController->isConnected()) {
    //     QMessageBox::critical(this, "错误", "黑体炉连接失败");
    // }

    // 初始禁用启动按钮
    ui->startStopButton->setEnabled(false);

    connect(m_blackbodyController, &BlackbodyController::errorOccurred, this, [this](const QString &error) {
        QMessageBox::critical(this, "错误", error);
    });


    // 设置温度按钮点击事件
    connect(ui->setTempButton, &QPushButton::clicked, this, [this]{
        QString input = ui->targetTempInput->text();
        bool ok;
        float temp = input.toFloat(&ok);

        // 输入有效性检查
        if (!ok) {
            QMessageBox::critical(this, "错误", "请输入有效的数字");
            return;
        }

        // 温度范围检查
        if (temp < -40.0f || temp > 80.0f) {
            QMessageBox::critical(this, "错误", "温度范围必须在-40℃到80℃之间");
            return;
        }

        // 调用控制器设置温度
        m_blackbodyController->setTargetTemperature(temp);
    });

    // 温度设置成功后启用启动按钮
    connect(m_blackbodyController, &BlackbodyController::targetTemperatureSet, this, [this](bool success) {
        ui->startStopButton->setEnabled(success);
    });

    // 输入框内容变化时禁用启动按钮
    connect(ui->targetTempInput, &QLineEdit::textChanged, this, [this]{
        ui->startStopButton->setEnabled(false);
    });

    connect(ui->masterControlButton, &QPushButton::clicked, this, [this](bool checked) {
        m_blackbodyController->setMasterControl(checked);

    });

    connect(ui->startStopButton, &QPushButton::clicked, this, [this](bool checked){
        m_blackbodyController->setDeviceState(checked);
        ui->startStopButton->setText(checked ? "停止" : "启动");
    });

    connect(m_blackbodyController, &BlackbodyController::currentTemperatureUpdated,
            this, [this](float temp) {
                ui->currentTempDisplay->setText(QString::number(temp, 'f', 2));
                handleTemperatureUpdate(temp); // 直接调用处理函数
            });

}

// 填充端口选择下拉框
void MainWindow::populatePortComboBox()
{
    // 清空现有项
    ui->COMcomboBox->clear();

    // 获取所有可用串口
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &port : ports) {
        ui->COMcomboBox->addItem(port.portName());
    }

    // 从配置文件读取上次使用的端口
    QString lastPort = m_settings->value("blackbody/com_port", "COM10").toString();
    int index = ui->COMcomboBox->findText(lastPort);
    if (index >= 0) {
        ui->COMcomboBox->setCurrentIndex(index);
    }
}

// 更新端口选择
void MainWindow::updatePortComboBox()
{
    QString portName = ui->COMcomboBox->currentText();
    m_settings->setValue("blackbody/com_port", portName); // 保存到配置
}

// 处理端口打开/关闭按钮点击
void MainWindow::onOpenComButtonClicked()
{
    if (!m_blackbodyController) {
        // 初始化控制器
        QString portName = ui->COMcomboBox->currentText();
        m_blackbodyController = new BlackbodyController(this);

        // 连接状态信号到UI更新
        connect(m_blackbodyController, &BlackbodyController::connectionStatusChanged,
                this, [this](bool connected) {
                    ui->OpenComButton->setText(connected ? "关闭端口" : "打开端口");
                    ui->COMcomboBox->setEnabled(!connected);

                    // 更新状态显示
                    QString color = connected ? "#00FF00" : "#FF0000";
                    ui->statusLabel->setStyleSheet(QString("background-color: %1; border-radius: 8px;").arg(color));
                    ui->statusLabel->setText(connected ? "已连接" : "未连接");
                });
    }

    if (m_blackbodyController->isConnected()) {
        m_blackbodyController->disconnectDevice();
    } else {
        bool success = m_blackbodyController->connectDevice(ui->COMcomboBox->currentText());
        if (!success) {
            QMessageBox::critical(this, "错误", "端口连接失败");
            ui->OpenComButton->setText("打开端口"); // 显式更新文本
        } else {
            ui->OpenComButton->setText("关闭端口"); // 连接成功后更新文本
        }
    }
}

void MainWindow::onEnableSaveToggled(bool checked)
{
    // 仅控制保存功能的启用状态，不操作路径控件
    saveEnabled = checked;

    // 提示用户
    if(checked && ui->savePathLineEdit->text().isEmpty()){
        QMessageBox::information(this, "提示", "已启用保存功能，请先选择保存路径");
    }
}

// 选择路径的槽函数
void MainWindow::onSelectPathClicked()
{
    QString path = QFileDialog::getExistingDirectory(
        this,
        tr("选择保存目录"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
        );

    if(!path.isEmpty()) {
        ui->savePathLineEdit->setText(path);
        // 自动启用保存复选框
        ui->enableSaveCheckBox->setChecked(true);
        m_blackbodySavePath = path;  // 同步变量
        // 关键：写入[blackbody]节点的save_path
        m_settings->setValue("blackbody/save_path", path);
    }
}

// 修改handleTemperatureUpdate函数添加保存逻辑
void MainWindow::handleTemperatureUpdate(float temp) {
    QDateTime now = QDateTime::currentDateTime();
    temperatureHistory.append(qMakePair(now, temp));
    emit newTemperatureData(now, temp); // 触发黑体炉曲线更新
    if (saveEnabled) {
        saveTemperatureData(temp);
    }
}

// 实现数据保存功能
void MainWindow::saveTemperatureData(float temp)
{
    if(!ui->enableSaveCheckBox->isChecked() ||
        ui->savePathLineEdit->text().isEmpty()){
        // 路径有效时重置错误状态
        if (blackbodyPathErrorShown) {
            blackbodyPathErrorShown = false;
        }
        return;
    }

    QDir saveDir(ui->savePathLineEdit->text());
    if(!saveDir.exists()) {
        // 仅在未显示过错误时才弹窗
        if (!blackbodyPathErrorShown) {
            QMessageBox::warning(this, "路径错误", "黑体炉温度数据保存路径不存在！");
            blackbodyPathErrorShown = true;
        }
        return;
    }

    // 路径有效时重置错误状态
    if (blackbodyPathErrorShown) {
        blackbodyPathErrorShown = false;
    }

    // 固定使用TXT格式
    QString fileName = QDateTime::currentDateTime().toString("yyyyMMdd") + "_blackbody_tempdata.txt";
    QString fullPath = saveDir.filePath(fileName);

    QFile file(fullPath);
    if(file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&file);
        stream << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << ","
               << QString::number(temp, 'f', 2) << "\n";
        file.close();
    } else {
        // 写入错误不使用状态跟踪，因为可能是临时错误
        QMessageBox::warning(this, "保存失败",
                             QString("无法写入黑体炉温度文件：%1\n错误信息：%2").arg(fullPath).arg(file.errorString()));
    }
}

void MainWindow::setupConnections()
{
    // 连接处理器信号
    connect(excelProcessor, &DataExcelProcessor::operationCompleted,
            this, &MainWindow::handleOperationCompleted);
    connect(excelProcessor, &DataExcelProcessor::errorOccurred,
            this, &MainWindow::handleError);
}

void MainWindow::setupHumidityControls() {
    QSettings settings("config.ini", QSettings::IniFormat);
    QString portName = settings.value("humidity/com_port", "COM12").toString();

    m_humidityController = new HumidityController(this);

    // 初始化定时器
    humidityTimer = new QTimer(this);
    humidityTimer->setInterval(1000);

    // 初始禁用操作按钮
    ui->setTempButton_2->setEnabled(false);
    ui->startStopButton_2->setEnabled(false);
    ui->previousSensorButton->setEnabled(false);
    ui->nextSensorButton->setEnabled(false);
    ui->openWindowButton->setEnabled(false);

    // 手动设置statusLabel_2的初始样式（新增）
    ui->statusLabel_2->setStyleSheet(
        "QLabel {"
        "   background-color: #FF0000;"
        "   border-radius: 8px;"
        "   min-width: 16px;"
        "   min-height: 16px;"
        "}"
        );
    ui->statusLabel_2->setText("未连接");

    connect(m_humidityController, &HumidityController::connectionStatusChanged,
            this, &MainWindow::onHumidityConnectionStatusChanged);

    connect(m_humidityController, &HumidityController::masterControlChanged,
            this, &MainWindow::onHumidityMasterControlChanged);

    connect(m_humidityController, &HumidityController::currentTemperatureUpdated,
            this, &MainWindow::updateCurrentTemperature);
    connect(m_humidityController, &HumidityController::currentHumidityUpdated,
            this, &MainWindow::updateCurrentHumidity);

    connect(ui->masterControlButton_2, &QPushButton::clicked, this,[this](bool checked) {
        m_humidityController->setMasterControl(checked);

    });

    connect(ui->setTempButton_2, &QPushButton::clicked, this, &MainWindow::onSetTargetTemperatureClicked);
    ui->startStopButton_2->setCheckable(true);
    connect(ui->startStopButton_2, &QPushButton::clicked, this, &MainWindow::onStartStopButtonClicked);

    connect(ui->previousSensorButton, &QPushButton::clicked, this, [this]() {
        if (m_servoController && m_servoController->isConnected()) {
            m_servoController->moveRelative(-40.0); // 假设你在Servo类里实现了这个，或者手动发指令
            // 如果Servo类里只实现了moveToAbsolute，这里可以先不做，或者加上moveRelative的实现
        } else {
            QMessageBox::warning(this, "提示", "伺服电机未连接");
        }
    });

    // "下一个"按钮 -> 顺时针转40度
    connect(ui->nextSensorButton, &QPushButton::clicked, this, [this]() {
        if (m_servoController && m_servoController->isConnected()) {
            m_servoController->moveRelative(40.0);
        } else {
            QMessageBox::warning(this, "提示", "伺服电机未连接");
        }
    });

    connect(ui->openWindowButton, &QPushButton::clicked, this, &MainWindow::onToggleCalibrationWindowClicked);

    connect(humidityTimer, &QTimer::timeout, m_humidityController, &HumidityController::readCurrentTemperature);
    connect(humidityTimer, &QTimer::timeout, m_humidityController, &HumidityController::readCurrentHumidity);

    // 恒温箱保存功能连接
    connect(ui->enableSaveCheckBox_3, &QCheckBox::toggled, this, &MainWindow::onHumidityEnableSaveToggled);
    connect(ui->selectPathButton_3, &QPushButton::clicked, this, &MainWindow::onHumiditySelectPathClicked);

    // 恒温箱数据更新信号连接（假设HumidityController发送currentDataUpdated信号）
    connect(m_humidityController, &HumidityController::currentTemperatureUpdated,
            this, &MainWindow::handleTemperatureUpdate2);
    connect(m_humidityController, &HumidityController::currentHumidityUpdated,
            this, &MainWindow::handleHumidityUpdate);
}

// 连接状态变化处理
void MainWindow::onHumidityConnectionStatusChanged(bool connected) {
    QString color = connected ? "#00FF00" : "#FF0000";
    ui->statusLabel_2->setStyleSheet(QString("background-color: %1; border-radius: 8px;").arg(color));
    ui->statusLabel_2->setText(connected ? "已连接" : "未连接");
    ui->OpenComButton_2->setText(connected ? "关闭" : "打开"); // 保留状态变化时的文字更新
    ui->COMcomboBox_2->setEnabled(!connected);
    ui->masterControlButton_2->setEnabled(connected);

    // 断开时释放控制权并禁用按钮
    if (!connected) {
        ui->masterControlButton_2->setChecked(false);
        ui->setTempButton_2->setEnabled(false);
        ui->startStopButton_2->setEnabled(false);
        ui->previousSensorButton->setEnabled(false);
        ui->nextSensorButton->setEnabled(false);
        ui->openWindowButton->setEnabled(false);
        humidityTimer->stop();
        qDebug() << "湿度定时器已停止";
    }


}

// 控制权变化处理（最终版本）
void MainWindow::onHumidityMasterControlChanged(bool acquired) {
    // 统一控制操作按钮状态
    ui->setTempButton_2->setEnabled(acquired);
    ui->startStopButton_2->setEnabled(acquired);
    ui->previousSensorButton->setEnabled(acquired);
    ui->nextSensorButton->setEnabled(acquired);
    ui->openWindowButton->setEnabled(acquired);

    if (acquired) {
        // 获得控制：启动定时器，更新按钮文本
        humidityTimer->start();
        ui->masterControlButton_2->setText("释放控制");
    } else {
        // 释放控制或获取失败：停止定时器，禁用按钮，清空显示
        humidityTimer->stop();
        ui->currentTempDisplay_2->clear();
        ui->currentHumDisplay->clear();

        // 显式禁用所有操作按钮（与初始状态一致）
        ui->setTempButton_2->setEnabled(false);
        ui->startStopButton_2->setEnabled(false);
        ui->previousSensorButton->setEnabled(false);
        ui->nextSensorButton->setEnabled(false);
        ui->openWindowButton->setEnabled(false);

        // 恢复按钮文本为“获取控制”
        ui->masterControlButton_2->setText(acquired ? "释放控制" : "获得控制");

    }
}

// 填充湿度控制器端口选择框
void MainWindow::populateHumidityPortComboBox()
{
    ui->COMcomboBox_2->clear();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &port : ports) {
        ui->COMcomboBox_2->addItem(port.portName());
    }

    // 读取配置文件中保存的端口
    QString lastPort = m_humiditySettings->value("humidity/com_port", "COM12").toString();
    int index = ui->COMcomboBox_2->findText(lastPort);
    if (index >= 0) {
        ui->COMcomboBox_2->setCurrentIndex(index);
    }
}

// 更新湿度控制器端口选择
void MainWindow::updateHumidityPortComboBox()
{
    QString portName = ui->COMcomboBox_2->currentText();
    m_humiditySettings->setValue("humidity/com_port", portName);
}

// 处理湿度控制器端口打开/关闭
void MainWindow::onOpenComButton2Clicked()
{
    // 确保控制器已初始化
    if (!m_humidityController) {
        QString portName = ui->COMcomboBox_2->currentText();
        m_humidityController = new HumidityController(this);

        // 只在初始化时连接一次信号（使用一次性连接或检查是否已连接）
        if (!connect(m_humidityController, &HumidityController::connectionStatusChanged,
                     this, &MainWindow::onHumidityConnectionStatusChanged)) {
            qDebug() << "信号连接失败";
        }
    }

    if (m_humidityController->isConnected()) {
        m_humidityController->disconnectDevice();
    } else {
        bool success = m_humidityController->connectDevice(ui->COMcomboBox_2->currentText());
        if (!success) {
            QMessageBox::critical(this, "错误", "恒温恒湿箱端口连接失败");
            ui->OpenComButton_2->setText("打开端口"); // 显式更新文本
        } else {
            ui->OpenComButton_2->setText("关闭端口"); // 连接成功后更新文本
        }
    }
}

// 更新当前温度显示
void MainWindow::updateCurrentTemperature(float temp) {
    ui->currentTempDisplay_2->setText(QString::number(temp, 'f', 2));
}

// 更新当前湿度显示
void MainWindow::updateCurrentHumidity(float humidity) {
    ui->currentHumDisplay->setText(QString::number(humidity, 'f', 2));
}

// 设置目标温度
void MainWindow::onSetTargetTemperatureClicked() {
    bool ok;
    float temp = ui->targetTempInput_2->text().toFloat(&ok);
    if (!ok) {
        QMessageBox::critical(this, "错误", "请输入有效的温度值");
        return;
    }
    m_humidityController->setTargetTemperature(temp);
}

// 启动/停止设备
void MainWindow::onStartStopButtonClicked(bool checked) {
    m_humidityController->setDeviceState(checked);
    ui->startStopButton_2->setText(checked ? "停止" : "启动");
}

// 更换测温仪（上一个/下一个）
void MainWindow::onChangeSensorButtonClicked(int direction) {
    m_humidityController->changeSensor(direction);
}

// 打开/关闭标定窗口
void MainWindow::onToggleCalibrationWindowClicked() {
    static bool isOpen = false;
    m_humidityController->toggleCalibrationWindow(!isOpen);
    ui->openWindowButton->setText(!isOpen ? "关闭标定窗口" : "打开标定窗口");
    isOpen = !isOpen;
}

// 处理恒温箱数据更新（温度+湿度）
void MainWindow::handleTemperatureUpdate2(float temp)
{
    QDateTime now = QDateTime::currentDateTime();

    // 更新温度历史数据
    temperatureHistory2.append(qMakePair(now, temp));
    while (!temperatureHistory2.isEmpty() &&
           temperatureHistory2.first().first.secsTo(now) > 300) {
        temperatureHistory2.removeFirst();
    }

    // 转发信号给所有窗口
    emit newTemperatureData2(now, temp);

    // 保存温度数据到独立文件
    if (humiditySaveEnabled && !humiditySavePath.isEmpty()) {
        QDir saveDir(humiditySavePath);
        if (!saveDir.exists()) {
            // 仅在未显示过错误时才弹窗
            if (!humidityTempPathErrorShown) {
                QMessageBox::warning(this, "路径错误", "恒温箱温度数据保存路径不存在！");
                humidityTempPathErrorShown = true;
            }
            return;
        }

        // 路径有效时重置错误状态
        if (humidityTempPathErrorShown) {
            humidityTempPathErrorShown = false;
        }

        // 生成带日期和温度标识的文件名
        QString fileName = QDateTime::currentDateTime().toString("yyyyMMdd") + "_humiditycontroller_temp.txt";
        QString fullPath = saveDir.filePath(fileName);

        QFile file(fullPath);
        if(file.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << now.toString("yyyy-MM-dd HH:mm:ss") << ","
                   << QString::number(temp, 'f', 2) << "\n";
        } else {
            QMessageBox::warning(this, "保存失败",
                                 QString("无法写入恒温箱温度文件：%1\n错误信息：%2").arg(fullPath).arg(file.errorString()));
        }
    } else {
        // 路径有效时重置错误状态
        if (humidityTempPathErrorShown) {
            humidityTempPathErrorShown = false;
        }
    }
}

void MainWindow::handleHumidityUpdate(float humidity)
{
    QDateTime now = QDateTime::currentDateTime();

    // 更新湿度历史数据
    humidityHistory.append(qMakePair(now, humidity));
    while (!humidityHistory.isEmpty() &&
           humidityHistory.first().first.secsTo(now) > 300) {
        humidityHistory.removeFirst();
    }

    // 转发信号给所有窗口
    emit newHumidityData(now, humidity);

    // 保存湿度数据到独立文件
    if (humiditySaveEnabled && !humiditySavePath.isEmpty()) {
        QDir saveDir(humiditySavePath);
        if (!saveDir.exists()) {
            // 仅在未显示过错误时才弹窗
            if (!humidityPathErrorShown) {
                QMessageBox::warning(this, "路径错误", "湿度数据保存路径不存在！");
                humidityPathErrorShown = true;
            }
            return;
        }

        // 路径有效时重置错误状态
        if (humidityPathErrorShown) {
            humidityPathErrorShown = false;
        }

        // 生成带日期和湿度标识的文件名
        QString fileName = QDateTime::currentDateTime().toString("yyyyMMdd") + "_humiditycontroller_hum.txt";
        QString fullPath = saveDir.filePath(fileName);

        QFile file(fullPath);
        if(file.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << now.toString("yyyy-MM-dd HH:mm:ss") << ","
                   << QString::number(humidity, 'f', 2) << "\n";
        } else {
            QMessageBox::warning(this, "保存失败",
                                 QString("无法写入湿度文件：%1\n错误信息：%2").arg(fullPath).arg(file.errorString()));
        }
    } else {
        // 路径有效时重置错误状态
        if (humidityPathErrorShown) {
            humidityPathErrorShown = false;
        }
    }
}

// 恒温箱保存功能启用状态变化
void MainWindow::onHumidityEnableSaveToggled(bool checked)
{
    humiditySaveEnabled = checked;
    if (checked && humiditySavePath.isEmpty()) {
        QMessageBox::information(this, "提示", "恒温箱数据保存路径未选择，已默认设置为：C:/Users/Administrator/Documents");
    }
}

// 选择恒温箱数据保存路径
void MainWindow::onHumiditySelectPathClicked()
{
    QString path = QFileDialog::getExistingDirectory(this, "选择恒温箱数据保存目录",
                                                     QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
    if (!path.isEmpty()) {
        humiditySavePath = path;
        ui->savePathLineEdit_3->setText(path);
        ui->enableSaveCheckBox_3->setChecked(true);
        m_settings->setValue("humidity/save_path", path);

    }
}

// 单头数据处理按钮
void MainWindow::onSingleHeadButtonClicked()
{
    QString sourcePath = QFileDialog::getOpenFileName(this, "选择数据文件", "", "Excel文件 (*.xlsx)");
    if (sourcePath.isEmpty()) return;

    // 第一步：处理标准数据
    excelProcessor->startProcessing(DataExcelProcessor::StandardData, sourcePath);

    // 标准处理完成的连接（使用可断开的连接）
    QMetaObject::Connection* stdConn = new QMetaObject::Connection;
    *stdConn = QObject::connect(excelProcessor, &DataExcelProcessor::operationCompleted, this,
                                [this, sourcePath, stdConn](bool success, const QString& stdOutputPath) {
                                    QObject::disconnect(*stdConn);
                                    delete stdConn;

                                    if (!success) {
                                        // 错误处理优化：获取最新错误信息并显示
                                        QString errorMsg = excelProcessor->lastError();
                                        QMessageBox::critical(this, "标准数据处理失败",
                                                              QString("处理失败: %1\n文件: %2")
                                                                  .arg(errorMsg.isEmpty() ? "未知错误" : errorMsg)
                                                                  .arg(QFileInfo(stdOutputPath).fileName()));
                                        return;
                                    }

                                    qDebug() << "标准数据处理完成，输出路径：" << stdOutputPath;

                                    // 第二步：处理单头数据
                                    excelProcessor->startProcessing(DataExcelProcessor::SingleHead, stdOutputPath);

                                    // 单头处理完成的连接
                                    QMetaObject::Connection* singleConn = new QMetaObject::Connection;
                                    *singleConn = QObject::connect(excelProcessor, &DataExcelProcessor::operationCompleted, this,
                                                                   [this, singleConn](bool success, const QString& singleOutputPath) {
                                                                       QObject::disconnect(*singleConn);
                                                                       delete singleConn;

                                                                       if (success) {
                                                                           QMessageBox::information(this, "完成",
                                                                                                    QString("标准+单头数据处理完成！\n结果文件：%1")
                                                                                                        .arg(singleOutputPath));
                                                                       } else {
                                                                           QString errorMsg = excelProcessor->lastError();
                                                                           QMessageBox::critical(this, "单头数据处理失败",
                                                                                                 QString("处理失败: %1\n文件: %2")
                                                                                                     .arg(errorMsg.isEmpty() ? "未知错误" : errorMsg)
                                                                                                     .arg(QFileInfo(singleOutputPath).fileName()));
                                                                       }
                                                                   });
                                });
}

// 多头数据处理按钮
void MainWindow::onMultiHeadButtonClicked()
{
    QString sourcePath = QFileDialog::getOpenFileName(this, "选择数据文件", "", "Excel文件 (*.xlsx)");
    if (sourcePath.isEmpty()) return;

    // 第一步：处理标准数据
    excelProcessor->startProcessing(DataExcelProcessor::StandardData, sourcePath);

    // 使用原始指针代替unique_ptr（简化方案）
    QMetaObject::Connection* stdConn = new QMetaObject::Connection;
    *stdConn = QObject::connect(excelProcessor, &DataExcelProcessor::operationCompleted, this,
                                [this, sourcePath, stdConn](bool success, const QString& stdOutputPath) {
                                    // 断开连接并释放内存
                                    QObject::disconnect(*stdConn);
                                    delete stdConn;

                                    if (!success) {
                                        QMessageBox::critical(this, "错误", "标准数据处理失败！");
                                        return;
                                    }

                                    qDebug() << "标准数据处理完成，输出路径：" << stdOutputPath;

                                    // 第二步：处理多头数据
                                    excelProcessor->startProcessing(DataExcelProcessor::MultiHead, stdOutputPath);

                                    // 使用原始指针处理多头连接
                                    QMetaObject::Connection* multiConn = new QMetaObject::Connection;
                                    *multiConn = QObject::connect(excelProcessor, &DataExcelProcessor::operationCompleted, this,
                                                                  [this, multiConn](bool success, const QString& multiOutputPath) {
                                                                      // 断开连接并释放内存
                                                                      QObject::disconnect(*multiConn);
                                                                      delete multiConn;

                                                                      if (success) {
                                                                          QMessageBox::information(this, "完成",
                                                                                                   QString("标准+多头数据处理完成！\n结果文件：\n%1").arg(multiOutputPath));
                                                                      } else {
                                                                          QMessageBox::critical(this, "错误", "多头数据处理失败！");
                                                                      }
                                                                  });
                                });
}


// 处理完成回调
void MainWindow::handleOperationCompleted(bool success, const QString& resultPath)
{

    if (success) {
        // QMessageBox::information(this, "完成", "数据处理成功!");
        // QDesktopServices::openUrl(QUrl::fromLocalFile(resultPath));
    } else {
        QMessageBox::critical(this, "错误", "处理过程中发生错误");
    }
}

// 错误处理
void MainWindow::handleError(const QString& message) {
    QMessageBox msgBox;
    msgBox.setWindowTitle("错误");
    msgBox.setText(message);
    msgBox.setIcon(QMessageBox::Critical);

    // 更精确的样式表，确保覆盖 QMessageBox 内部控件
    msgBox.setStyleSheet(
        "QMessageBox { "
        "    background-color: #333; " // 对话框背景色
        "    color: white; "             // 整体文字颜色（影响按钮文字等）
        "}"
        "QMessageBox QLabel { "
        "    color: white; "             // 错误信息标签颜色
        "    background: transparent; "  // 标签背景透明，避免遮挡
        "}"
        "QMessageBox QPushButton { "
        "    color: white; "             // 按钮文字颜色
        "    background-color: #0984E3; "// 按钮背景色（与应用风格一致）
        "    border: none; "
        "    border-radius: 4px; "
        "    padding: 6px 12px; "
        "}"
        "QMessageBox QPushButton:hover { "
        "    background-color: #0762A5; "// 按钮悬停颜色
        "}"
        );

    msgBox.exec();
}


void MainWindow::processMergedFile(const QString& filePath)
{
    QXlsx::Document xlsx(filePath);
    if (!xlsx.load()) {
        QMessageBox::critical(this, "错误", "文件加载失败");
        return;
    }

    static const QRegularExpression re(R"(^COM\d+-多(\d+)$)");
    QVector<bool> lastSelections;  // 新增：保存上一次的选择结果

    foreach (const QString& sheetName, xlsx.sheetNames()) {
        if (re.match(sheetName).hasMatch()) {
            xlsx.selectSheet(sheetName);

            QRegularExpressionMatch match = re.match(sheetName);
            QString deviceName = "多" + match.captured(1);

            // 读取D列和P列数据
            QVector<double> temperatureValues;
            QVector<QString> testConditions;
            int emptyCount = 0;

            for (int row = 4; emptyCount < 5; ++row) {
                QVariant tempVar = xlsx.read(row, 4);  // D列
                QVariant condVar = xlsx.read(row, 16); // P列

                if (tempVar.isNull() || condVar.isNull()) {
                    ++emptyCount;
                    continue;
                }

                emptyCount = 0;
                temperatureValues.append(tempVar.toDouble());
                testConditions.append(condVar.toString());
            }

            // 关键：传递上次选择作为默认值（需确保ModelingPointDialog支持defaultSelections参数）
            ModelingPointDialog dlg(temperatureValues, testConditions, deviceName, lastSelections, this);
            if (dlg.exec() == QDialog::Accepted) {
                QVector<bool> currentSelections = dlg.getSelections();
                lastSelections = currentSelections;  // 更新为当前选择，供下一个工作表使用
                processSelectedData(xlsx, sheetName, currentSelections, filePath);
            } else {
                // 可选：用户取消时是否重置上次选择（根据需求决定）
                // lastSelections.clear();
            }
        }
    }
}



// 改为返回生成的模板路径
QString MainWindow::processSelectedData(QXlsx::Document& srcDoc,
                                        const QString& sheetName,
                                        const QVector<bool>& selections,
                                        const QString& sourceFilePath)
{
    // 提取设备名称（逻辑不变）
    static const QRegularExpression re(R"(多(\d+))");
    QRegularExpressionMatch match = re.match(sheetName);
    QString deviceName = match.hasMatch() ? "多" + match.captured(1) : "未知设备";

    // 创建新文档（逻辑不变）
    QXlsx::Document newXlsx;
    newXlsx.addSheet(sheetName);
    newXlsx.selectSheet(sheetName);

    // 写入列标题（逻辑不变）
    QStringList headers = {"测量点温度", "TO1", "TO2", "TO3", "TA1", "TA2", "TA3", "标准",
                           "测量点温度验证", "TO1验证", "TO2验证", "TO3验证", "TA1验证", "TA2验证", "TA3验证", "标准验证"};
    for(int col = 1; col <= headers.size(); ++col) {
        newXlsx.write(1, col, headers[col-1]);
    }

    // 写入数据（逻辑不变）
    int selectedRow = 2;
    int unselectedRow = 2;
    for (int i = 0; i < selections.size(); ++i) {
        const int srcRow = 4 + i;
        QVector<QVariant> rowData;

        for (int col = 4; col <= 10; ++col) {
            rowData.append(srcDoc.read(srcRow, col));
        }
        rowData.append(srcDoc.read(srcRow, 14)); // N列

        if (selections[i]) {
            for (int col = 1; col <= 8; ++col) {
                newXlsx.write(selectedRow, col, rowData[col-1]);
            }
            ++selectedRow;
        } else {
            for (int col = 9; col <= 16; ++col) {
                newXlsx.write(unselectedRow, col, rowData[col-9]);
            }
            ++unselectedRow;
        }
    }

    // 保存文件并返回路径（关键修改）
    QFileInfo sourceFileInfo(sourceFilePath);
    QString outputPath = sourceFileInfo.path() + "/" + deviceName + ".xlsx";

    if (newXlsx.saveAs(outputPath)) {
        // 整合流程中无需自动打开文件，注释此行
        // QDesktopServices::openUrl(QUrl::fromLocalFile(outputPath));
        return outputPath; // 返回生成的模板路径
    } else {
        QMessageBox::critical(this, "错误", "文件保存失败");
        return ""; // 保存失败返回空路径
    }
}

void MainWindow::onPythonProgress(const QString& message)
{
    ui->statusbar->showMessage(message, 3000);
}

void MainWindow::onPythonFinished(bool success, const QString& path)
{
    m_progressDialog->reset();

    if(success) {
        // QMessageBox::information(this, "处理完成",
        //                          QString("处理成功！生成文件：\n%1").arg(path));
        // QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    } else {
        QMessageBox::critical(this, "错误", "处理失败，请检查输入文件和日志");
    }
}

void MainWindow::onPythonError(const QString& error)
{
    QMessageBox::critical(this, "错误", error);
}

void MainWindow::handleProgressUpdated(int percent, const QString& message)
{
    // 确保进度不倒退
    if(percent > m_progressDialog->value()) {
        m_progressDialog->setValue(percent);
        m_progressDialog->setLabelText(message);
        qDebug() << "更新进度：" << percent << "%" << message;
    }
    ui->statusbar->showMessage(QString("%1% - %2").arg(percent).arg(message), 2000);
}

void MainWindow::processSingleHeadFile(const QString& filePath)
{
    QXlsx::Document srcDoc(filePath);
    if (!srcDoc.load()) {
        QMessageBox::critical(this, "错误", "文件加载失败");
        return;
    }

    QVector<bool> lastSelections;  // 关键：保存上一次的选择结果

    foreach (const QString& sheetName, srcDoc.sheetNames()) {
        // 跳过非目标工作表（可选）
        // if (!sheetName.startsWith("N")) continue;

        srcDoc.selectSheet(sheetName);

        // 读取D列（测试条件）和E列（测试温度）
        QVector<double> temperatures;
        QVector<QString> conditions;
        int emptyCount = 0;

        for (int row = 2; emptyCount < 5; ++row) {
            QVariant condVar = srcDoc.read(row, 4);  // D列（第4列）
            QVariant tempVar = srcDoc.read(row, 5);  // E列（第5列）

            if (condVar.isNull() || tempVar.isNull()) {
                ++emptyCount;
                continue;
            }

            emptyCount = 0;
            conditions.append(condVar.toString());
            temperatures.append(tempVar.toDouble());
        }

        // 关键：创建对话框时传递上次选择作为默认值
        ModelingPointDialog dlg(temperatures, conditions, sheetName, lastSelections, this);
        if (dlg.exec() == QDialog::Accepted) {
            QVector<bool> currentSelections = dlg.getSelections();
            lastSelections = currentSelections;  // 更新为当前选择，供下一个工作表使用
            generateTemplateFile(srcDoc, sheetName, currentSelections, filePath);
        } else {
            // 可选：用户取消时是否重置上次选择（根据需求决定）
            // lastSelections.clear();
        }
    }
}

void MainWindow::generateTemplateFile(QXlsx::Document& srcDoc,
                                      const QString& sheetName,
                                      const QVector<bool>& selections,
                                      const QString& sourceFilePath)
{
    // 创建新文档
    QXlsx::Document newXlsx;

    // 添加两个工作表
    newXlsx.addSheet("建模");
    newXlsx.addSheet("验证");

    // 写入表头
    newXlsx.selectSheet("建模");
    newXlsx.write(1, 1, "测试条件");
    newXlsx.write(1, 2, "测量点温度");
    newXlsx.write(1, 3, "目标");
    newXlsx.write(1, 4, "腔体");
    newXlsx.write(1, 5, "标准");

    newXlsx.selectSheet("验证");
    newXlsx.write(1, 1, "测试条件验证");
    newXlsx.write(1, 2, "测量点温度验证");
    newXlsx.write(1, 3, "目标验证");
    newXlsx.write(1, 4, "腔体验证");
    newXlsx.write(1, 5, "标准验证");

    // 分类写入数据
    int modelingRow = 2, validationRow = 2;
    for (int i = 0; i < selections.size(); ++i) {
        const int srcRow = 2 + i; // 源数据起始行
        QVector<QVariant> rowData;

        // 读取D(4), E(5), F(6), G(7), H(8)列
        for (int col = 4; col <= 8; ++col) {
            rowData.append(srcDoc.read(srcRow, col));
        }

        if (selections[i]) {
            newXlsx.selectSheet("建模");
            for (int col = 1; col <= 5; ++col) {
                newXlsx.write(modelingRow, col, rowData[col-1]);
            }
            modelingRow++;
        } else {
            newXlsx.selectSheet("验证");
            for (int col = 1; col <= 5; ++col) {
                newXlsx.write(validationRow, col, rowData[col-1]);
            }
            validationRow++;
        }
    }

    // 保存文件
    QFileInfo sourceInfo(sourceFilePath);
    QString outputPath = sourceInfo.path() + "/" + sheetName + ".xlsx";

    if (newXlsx.saveAs(outputPath)) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(outputPath));
    } else {
        QMessageBox::critical(this, "错误", QString("文件保存失败: %1").arg(outputPath));
    }
}

void MainWindow::setupCalibrationManager()
{
    m_calibrationManager = new CalibrationManager(m_blackbodyController, m_humidityController, this);
    m_calibrationManager->setServoController(m_servoController);
    connect(m_calibrationManager, &CalibrationManager::calibrationFinished, this, &MainWindow::onCalibrationFinished);
    connect(m_calibrationManager, &CalibrationManager::errorOccurred, this, &MainWindow::onCalibrationError);
}

void MainWindow::onStartCalibrationClicked()
{
    calibrationButtonClickCount++;

    // 1. 解析温度点 (保留原有逻辑)
    QString blackbodyTempStr = ui->blackbodyTempInput->text();
    QStringList blackbodyTemps = blackbodyTempStr.split(",");
    QVector<float> blackbodyTempPoints;
    QVector<float> humidityTempPoints;

    for (const QString &tempStr : blackbodyTemps) {
        bool ok;
        float temp = tempStr.toFloat(&ok);
        if (ok) {
            blackbodyTempPoints.append(temp);
        }
    }

    if (blackbodyTempPoints.isEmpty()) {
        QMessageBox::warning(this, "错误", "请输入有效的黑体炉温度点");
        return;
    }

    // 生成恒温箱温度点 (保留原有逻辑)
    int calibrationType = ui->calibrationTypeComboBox->currentIndex();
    bool isInside = (calibrationType == 0 || calibrationType == 2);
    for (float bbTemp : blackbodyTempPoints) {
        if (isInside) humidityTempPoints.append(bbTemp);
        else humidityTempPoints.append(25.0f);
    }

    // ================== 【新增核心逻辑开始】 ==================

    // 2. 连接伺服电机 (如果未连接)
    if (!m_servoController->isConnected()) {
        // 优先从 config.ini 读取 servo/com_port，如果没有则默认 COM1
        QString servoPort = m_settings->value("servo/com_port", "COM1").toString();

        if (!m_servoController->connectDevice(servoPort)) {
            QMessageBox::critical(this, "连接失败",
                                  QString("无法连接伺服电机 (端口: %1)\n请在 config.ini 中配置 [servo] com_port=COMx").arg(servoPort));
            return;
        }
    }

    // 3. 解析设备位置配置 (例如 "1-COM9, 2-COM11")
    QString mappingStr = m_settings->value("devices/com_ports").toString();
    mappingStr.remove('"'); // 去除引号
    QStringList pairs = mappingStr.split(',', Qt::SkipEmptyParts);
    QVector<SensorTask> taskQueue;

    for (const QString &pair : pairs) {
        QStringList parts = pair.split('-');
        if (parts.size() == 2) {
            bool ok;
            int pos = parts[0].toInt(&ok);     // 物理位置
            QString com = parts[1].trimmed();  // COM口

            if (ok && pos >= 1 && pos <= 10) {
                taskQueue.append({com, pos});
            } else {
                qWarning() << "忽略无效的位置配置:" << pair;
            }
        }
    }

    if (taskQueue.isEmpty()) {
        QMessageBox::critical(this, "配置错误", "未解析到有效的设备位置信息！\n请检查 config.ini 的 [devices] com_ports 设置。\n格式应为: 1-COMx, 2-COMy");
        return;
    }

    // 4. 将任务队列传递给管理器
    m_calibrationManager->setMeasurementQueue(taskQueue);

    // ================== 【新增核心逻辑结束】 ==================

    checkAutoSaveSettings();
    calibrationInProgress = true;
    ui->startCalibrationButton->setEnabled(false);

    // 启动标定
    m_calibrationManager->startCalibration(blackbodyTempPoints, humidityTempPoints);
}


void MainWindow::onPauseResumeClicked()
{
    CalibrationManager::State state = m_calibrationManager->getCurrentState();

    if (state == CalibrationManager::Running) {
        m_calibrationManager->pauseCalibration();
        ui->pauseResumeButton->setText("继续");
    }
    else if (state == CalibrationManager::Paused) {
        m_calibrationManager->resumeCalibration();
        ui->pauseResumeButton->setText("暂停");
    }
}

void MainWindow::onCancelClicked()
{
    int reply = QMessageBox::question(this, "确认取消", "确定要取消当前标定过程吗？",
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        m_calibrationManager->cancelCalibration();

    }
}

void MainWindow::onCalibrationStateChanged(CalibrationManager::State newState)
{
    // 记录上一个状态（使用静态变量在多次次调用间保留值）
    static CalibrationManager::State lastState = CalibrationManager::Idle;

    updateButtonStates();

    switch(newState) {
    case CalibrationManager::Running:
        ui->statusLabel_3->setText("标定进行中...");
        break;
    case CalibrationManager::Paused:
        ui->statusLabel_3->setText("标定已暂停");
        break;
    case CalibrationManager::Canceling:
        ui->statusLabel_3->setText("正在取消标定...");
        break;
    case CalibrationManager::Finished:
        ui->statusLabel_3->setText("标定已完成");
        break;
    case CalibrationManager::Idle:  // 新增空闲状态处理
        ui->statusLabel_3->setText("准备就绪");
        calibrationInProgress = false;

        // 核心新增逻辑：当从"取消中"状态切换到"空闲"状态时，显示取消成功提示
        if (lastState == CalibrationManager::Canceling) {
            QMessageBox::information(
                this,
                "取消成功",
                "标定过程已成功取消，所有设备已停止运行。",
                QMessageBox::Ok
                );
        }
        break;
    }

    // 更新上一个状态为当前状态（供下次调用判断）
    lastState = newState;
}


void MainWindow::updateButtonStates()
{
    CalibrationManager::State state = m_calibrationManager->getCurrentState();

    // 开始按钮：在空闲状态或完成状态可用
    bool canStart = (state == CalibrationManager::Idle) ||
                    (state == CalibrationManager::Finished);
    ui->startCalibrationButton->setEnabled(canStart);

    // 暂停/继续按钮：在运行或暂停状态可用
    ui->pauseResumeButton->setEnabled(state == CalibrationManager::Running ||
                                  state == CalibrationManager::Paused);

    // 根据状态设置按钮文本
    if (state == CalibrationManager::Paused) {
        ui->pauseResumeButton->setText("继续");
    } else {
        ui->pauseResumeButton->setText("暂停");
    }

    // 取消按钮：在运行、暂停或取消状态可用
    ui->cancelButton->setEnabled(state == CalibrationManager::Running ||
                             state == CalibrationManager::Paused ||
                             state == CalibrationManager::Canceling);
}

void MainWindow::onCalibrationFinished(/*const QVector<QPair<float, QPair<QDateTime, float>>> &calibrationData*/)
{
    QMessageBox::information(this, "提示", "标校完成，报告已生成");
    // // 可以在这里处理标校完成后的其他操作
    // QDesktopServices::openUrl(QUrl::fromLocalFile("calibration_report.xlsx"));
}

void MainWindow::onCalibrationError(const QString &error)
{
    QMessageBox::critical(this, "错误", error);
}


void MainWindow::checkAutoSaveSettings() {
    // 黑体炉自动保存：优先用blackbody/save_path
    if (ui->savePathLineEdit->text().isEmpty()) {
        // 读取[blackbody]的save_path作为默认
        QString defaultPath = m_settings->value("blackbody/save_path",
                                                QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
        ui->savePathLineEdit->setText(defaultPath);
        ui->enableSaveCheckBox->setChecked(true);
        saveEnabled = true;
        // 同步更新配置（避免下次启动仍为空）
        m_settings->setValue("blackbody/save_path", defaultPath);
    }

    // 恒温箱自动保存：优先用humidity/save_path
    if (ui->savePathLineEdit_3->text().isEmpty()) {
        // 读取[humidity]的save_path作为默认
        QString defaultPath = m_settings->value("humidity/save_path",
                                                QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).toString();
        ui->savePathLineEdit_3->setText(defaultPath);
        ui->enableSaveCheckBox_3->setChecked(true);
        humiditySaveEnabled = true;
        humiditySavePath = defaultPath;
        // 同步更新配置
        m_settings->setValue("humidity/save_path", defaultPath);
    }
}

void MainWindow::updateCalibrationProgress(int progress)
{
    calibrationProgressBar->setValue(progress);
    calibrationProgressBar->show(); // 显示进度条
}

void MainWindow::updateOperationLog(const QString &progressText)
{
    QTextCursor cursor = ui->operationLogTextEdit->textCursor();
    QTextCharFormat format;

    // 移动到文档末尾
    cursor.movePosition(QTextCursor::End);

    // 如果是新操作（不是倒计时更新）
    if (progressText != m_lastOperation && !progressText.contains("剩余时间：")) {
        // 添加时间戳（灰色）
        format.setForeground(QColor("#888888"));
        cursor.setCharFormat(format);
        QString timestamp = QDateTime::currentDateTime().toString("[yyyy-MM-dd HH:mm:ss] ");
        cursor.insertText(timestamp);

        // 添加操作内容（黑色）
        format.setForeground(QColor("#000000"));
        cursor.setCharFormat(format);
        cursor.insertText(progressText);
        cursor.insertText("\n"); // 换行

        // 保存当前操作作为历史记录
        m_lastOperation = progressText;
    }
    // 如果是倒计时更新（动态刷新最后一行）
    else if (progressText.contains("剩余时间：")) {
        // 移动到最后一行并替换
        cursor.movePosition(QTextCursor::End);
        cursor.movePosition(QTextCursor::StartOfLine, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();

        // 倒计时用深蓝色显示
        format.setForeground(QColor("#0000aa"));
        cursor.setCharFormat(format);
        QString timestamp = QDateTime::currentDateTime().toString("[yyyy-MM-dd HH:mm:ss] ");
        cursor.insertText(timestamp + progressText);
        cursor.insertText("\n");
    }

    // 自动滚动到最底部
    ui->operationLogTextEdit->setTextCursor(cursor);
    ui->operationLogTextEdit->ensureCursorVisible();
}

// 新增倒计时显示处理函数
void MainWindow::updateCountdownDisplay(int remainingSeconds, const QString &stage)
{
    // 转换秒为分秒格式
    int minutes = remainingSeconds / 60;
    int seconds = remainingSeconds % 60;
    QString progressText = QString("%1 - 剩余时间：%2分%3秒")
                               .arg(stage)
                               .arg(minutes)
                               .arg(seconds, 2, 10, QChar('0'));

    // 调用日志更新函数
    updateOperationLog(progressText);
}

// mainwindow.cpp 中的 setupCustomTitleBar 方法
void MainWindow::setupCustomTitleBar() {
    m_customTitleBar = new CustomTitleBar(centralWidget());
    QVBoxLayout *existingLayout = qobject_cast<QVBoxLayout*>(centralWidget()->layout());
    if (!existingLayout) {
        qWarning() << "centralWidget has no existing layout!";
        return;
    }

    existingLayout->insertWidget(0, m_customTitleBar);
    existingLayout->setContentsMargins(0, 0, 0, 0);
    existingLayout->setSpacing(0);

    // 连接最小化按钮
    connect(m_customTitleBar, &CustomTitleBar::minimizeClicked, this, &MainWindow::showMinimized);

    // 连接最大化/还原按钮（点击时切换状态）
    connect(m_customTitleBar, &CustomTitleBar::maximizeRestoreClicked, this, [this]() {
        if (isMaximized()) {
            showNormal(); // 还原窗口
        } else {
            showMaximized(); // 最大化窗口
        }
    });

    // 连接关闭按钮
    connect(m_customTitleBar, &CustomTitleBar::closeClicked, this, &MainWindow::close);
}

// 在MainWindow.cpp中实现整合函数
void MainWindow::onIntegratedProcessClicked()
{
    // 第一步：选择箱内和箱外文件
    QString inFile = QFileDialog::getOpenFileName(this, "选择箱内文件");
    QString outFile = QFileDialog::getOpenFileName(this, "选择箱外文件");

    if (inFile.isEmpty() || outFile.isEmpty()) {
        QMessageBox::warning(this, "提示", "请选择箱内和箱外文件");
        return;
    }

    // 显示进度对话框
    m_progressDialog->setWindowTitle("处理中");
    m_progressDialog->setLabelText("正在合并文件...");
    m_progressDialog->setRange(0, 100);
    m_progressDialog->setValue(20);
    m_progressDialog->show();

    // 保存中间路径
    m_integratedMergedPath = "";
    m_integratedTemplatePaths.clear();

    // 异步合并文件，并设置后续处理回调
    QFutureWatcher<void>* mergeWatcher = new QFutureWatcher<void>(this);
    connect(mergeWatcher, &QFutureWatcher<void>::finished, [this, mergeWatcher]() {
        mergeWatcher->deleteLater();

        if (!m_integratedMergedPath.isEmpty()) {
            m_progressDialog->setLabelText("正在生成拟合模板...");
            m_progressDialog->setValue(30);
            processIntegratedTemplatesWithDialog(m_integratedMergedPath);
        } else {
            m_progressDialog->hide();
            QMessageBox::critical(this, "错误", "文件合并失败，无法继续");
        }
    });

    // 启动合并任务
    QFuture<void> mergeFuture = QtConcurrent::run([=]() {
        m_integratedMergedPath = excelProcessor->mergeSingleHeadFiles(inFile, outFile);
    });
    mergeWatcher->setFuture(mergeFuture);
}

void MainWindow::processIntegratedTemplatesWithDialog(const QString& mergedFilePath)
{
    if (mergedFilePath.isEmpty()) {
        m_progressDialog->hide();
        return;
    }

    QXlsx::Document mergedDoc(mergedFilePath);
    if (!mergedDoc.load()) {
        QMessageBox::critical(this, "错误", "合并文件加载失败");
        m_progressDialog->hide();
        return;
    }

    QVector<bool> lastSelections;
    m_integratedTemplatePaths.clear();
    int sheetCount = mergedDoc.sheetNames().count();
    int processedSheets = 0;

    foreach (const QString& sheetName, mergedDoc.sheetNames()) {
        // 跳过标准工作表
        if (sheetName == "标准") continue;

        mergedDoc.selectSheet(sheetName);
        QVector<double> temperatures;
        QVector<QString> conditions;
        int emptyCount = 0;

        // 读取数据
        for (int row = 2; emptyCount < 5; ++row) {
            QVariant condVar = mergedDoc.read(row, 4); // D列（测试条件）
            QVariant tempVar = mergedDoc.read(row, 5);  // E列（温度）

            if (condVar.isNull() || tempVar.isNull()) {
                ++emptyCount;
                continue;
            }

            emptyCount = 0;
            conditions.append(condVar.toString());
            temperatures.append(tempVar.toDouble());
        }

        if (temperatures.isEmpty()) continue;

        // 弹出选择对话框（传递上次选择作为默认）
        ModelingPointDialog dlg(temperatures, conditions, sheetName, lastSelections, this);
        if (dlg.exec() != QDialog::Accepted) {
            QMessageBox::warning(this, "提示", "已取消后续拟合模板生成");
            break; // 用户取消，终止后续处理
        }

        QVector<bool> selections = dlg.getSelections();
        lastSelections = selections;

        // 生成模板文件
        QString templatePath = generateIntegratedTemplate(mergedDoc, sheetName, selections, mergedFilePath);
        if (!templatePath.isEmpty()) {
            m_integratedTemplatePaths.append(templatePath);
        }

        // 更新进度
        processedSheets++;
        int progress = 30 + (processedSheets * 40 / sheetCount);
        m_progressDialog->setValue(progress);
    }

    // 处理所有模板
    if (!m_integratedTemplatePaths.isEmpty()) {
        m_progressDialog->setLabelText("正在进行数据拟合...");
        m_progressDialog->setValue(70);
        processAllTemplatesFitting();
    } else {
        m_progressDialog->hide();
        QMessageBox::information(this, "完成", "未生成任何拟合模板");
    }
}

void MainWindow::processAllTemplatesFitting()
{
    if (m_integratedTemplatePaths.isEmpty()) {
        m_progressDialog->hide();
        return;
    }

    // 初始化模板处理队列和索引
    m_templateQueue = QStringList::fromVector(m_integratedTemplatePaths);
    m_currentTemplateIndex = 0;
    int total = m_templateQueue.size();

    m_progressDialog->setLabelText("开始处理所有模板...");
    m_progressDialog->setRange(70, 100);
    m_progressDialog->setValue(70);
    m_progressDialog->show();

    qDebug() << "待处理模板数量:" << total;
    processNextTemplate();
}

// 在 processNextTemplate 函数中增加状态检查和延迟处理机制
void MainWindow::processNextTemplate()
{
    qDebug() << "进入processNextTemplate，当前索引:" << m_currentTemplateIndex;

    // 检查是否所有模板都已处理
    if (m_currentTemplateIndex >= m_templateQueue.size()) {
        qDebug() << "所有设备拟合完成";
        m_progressDialog->hide();
        QMessageBox::information(this, "完成", "所有设备拟合完成！");
        return;
    }

    // 确保Python进程完全终止
    if (m_pythonProcessor->isProcessing()) {
        qDebug() << "Python进程仍在运行，强制终止...";
        m_pythonProcessor->terminateProcess();
        QThread::msleep(500); // 确保进程完全终止
    }

    const QString& templatePath = m_templateQueue[m_currentTemplateIndex];
    qDebug() << "当前处理模板:" << templatePath << "("
             << (m_currentTemplateIndex + 1) << "/" << m_templateQueue.size() << ")";

    // 更新进度
    int progress = 70 + (30 * m_currentTemplateIndex / m_templateQueue.size());
    m_progressDialog->setValue(progress);
    m_progressDialog->setLabelText(QString("正在处理模板 %1/%2: %3")
                                       .arg(m_currentTemplateIndex + 1)
                                       .arg(m_templateQueue.size())
                                       .arg(QFileInfo(templatePath).fileName()));

    // 获取测试员和审核员信息
    QString tester = ui->testerlineEdit->text().trimmed();
    QString reviewer = ui->reviewerslineEdit->text().trimmed();

    // 传递信息给Python处理器
    emit m_pythonProcessor->setTesterReviewerInfo(tester, reviewer);

    // 传递合并文件路径给Python处理器
    m_pythonProcessor->setMergedFilePath(m_integratedMergedPath);


    // 使用一次性连接确保只处理当前模板
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(m_pythonProcessor, &PythonProcessor::processingFinished,
                          this, [this, templatePath, connection](bool success, const QString& resultPath) {
                              // 断开连接，避免重复触发
                              disconnect(*connection);

                              if (success) {
                                  qDebug() << "模板处理成功:" << templatePath;
                              } else {
                                  qDebug() << "模板处理失败:" << templatePath;
                              }

                              // 移动到下一个模板
                              m_currentTemplateIndex++;

                              // 延迟处理下一个模板，确保资源释放
                              QTimer::singleShot(500, this, [this] {
                                  processNextTemplate();
                              });
                          });


    // 启动Python处理
    m_pythonProcessor->startProcessing(templatePath);
}



QString MainWindow::generateIntegratedTemplate(QXlsx::Document& srcDoc,
                                               const QString& sheetName,
                                               const QVector<bool>& selections,
                                               const QString& sourceFilePath)
{
    // 创建新文档
    QXlsx::Document newXlsx;

    // 添加两个工作表
    newXlsx.addSheet("建模");
    newXlsx.addSheet("验证");

    // 写入表头
    newXlsx.selectSheet("建模");
    newXlsx.write(1, 1, "测试条件");
    newXlsx.write(1, 2, "测量点温度");
    newXlsx.write(1, 3, "目标");
    newXlsx.write(1, 4, "腔体");
    newXlsx.write(1, 5, "标准");

    newXlsx.selectSheet("验证");
    newXlsx.write(1, 1, "测试条件验证");
    newXlsx.write(1, 2, "测量点温度验证");
    newXlsx.write(1, 3, "目标验证");
    newXlsx.write(1, 4, "腔体验证");
    newXlsx.write(1, 5, "标准验证");

    // 分类写入数据
    int modelingRow = 2, validationRow = 2;
    for (int i = 0; i < selections.size(); ++i) {
        const int srcRow = 2 + i;
        QVector<QVariant> rowData;

        // 读取D(4), E(5), F(6), G(7), H(8)列
        for (int col = 4; col <= 8; ++col) {
            rowData.append(srcDoc.read(srcRow, col));
        }

        if (selections[i]) {
            newXlsx.selectSheet("建模");
            for (int col = 1; col <= 5; ++col) {
                newXlsx.write(modelingRow, col, rowData[col-1]);
            }
            modelingRow++;
        } else {
            newXlsx.selectSheet("验证");
            for (int col = 1; col <= 5; ++col) {
                newXlsx.write(validationRow, col, rowData[col-1]);
            }
            validationRow++;
        }
    }

    // 保存文件
    QFileInfo sourceInfo(sourceFilePath);
    QString outputPath = sourceInfo.path() + "/" + sheetName + ".xlsx";

    if (newXlsx.saveAs(outputPath)) {
        return outputPath;
    } else {
        QMessageBox::critical(this, "错误", QString("模板保存失败: %1").arg(outputPath));
        return "";
    }
}

// 整合三个按钮功能的新按钮槽函数
void MainWindow::onIntegratedMultiProcessClicked()
{
    QString inFile = QFileDialog::getOpenFileName(this, "选择箱内数据文件");
    QString outFile = QFileDialog::getOpenFileName(this, "选择箱外数据文件");

    if (inFile.isEmpty() || outFile.isEmpty()) {
        QMessageBox::warning(this, "提示", "请选择箱内和箱外数据文件");
        return;
    }

    m_integratedMergedPath = "";
    m_integratedTemplatePaths.clear();
    m_templateQueue.clear();
    m_currentTemplateIndex = 0;

    m_progressDialog->setWindowTitle("多头数据处理中");
    m_progressDialog->setLabelText("正在合并文件...");
    m_progressDialog->setRange(0, 100);
    m_progressDialog->setValue(10);
    m_progressDialog->show();
    QApplication::processEvents();

    // 步骤1：生成模板文件
    QString templatePath;
    excelProcessor->generateTemplateExcelforMulitiHead(inFile, outFile, templatePath);
    bool templateGenerated = !templatePath.isEmpty() && QFile::exists(templatePath);

    m_progressDialog->setLabelText("生成模板文件...");
    m_progressDialog->setValue(20);
    QApplication::processEvents();

    if (!templateGenerated) {
        m_progressDialog->hide();
        QMessageBox::critical(this, "错误", "模板文件生成失败");
        return;
    }

    // 步骤2：同步等待合并完成（关键修改）
    QEventLoop loop; // 用于阻塞等待信号
    bool mergeSuccess = false;
    QString actualOutputPath;

    // 连接合并完成信号（主线程处理）
    QMetaObject::Connection conn = connect(excelProcessor, &DataExcelProcessor::operationCompleted,
                                           this, [&](bool success, const QString& outputPath) {
                                               mergeSuccess = success;
                                               actualOutputPath = outputPath;
                                               loop.quit(); // 信号触发后退出事件循环
                                           }, Qt::QueuedConnection);

    // 启动合并（注意：原startProcessing参数需调整）
    excelProcessor->startProcessing(DataExcelProcessor::MergeFiles, inFile, outFile, templatePath);

    // 阻塞等待合并完成（界面保持响应）
    m_progressDialog->setLabelText("合并文件中...");
    m_progressDialog->setValue(25);
    loop.exec(); // 进入事件循环，等待信号触发

    // 断开信号连接（避免重复触发）
    disconnect(conn);

    // 步骤3：验证合并结果
    m_progressDialog->setLabelText("验证合并结果...");
    m_progressDialog->setValue(30);
    QApplication::processEvents();

    if (mergeSuccess && !actualOutputPath.isEmpty() && QFile::exists(actualOutputPath)) {
        m_integratedMergedPath = actualOutputPath; // 使用信号传递的真实路径

        // 在启动Python处理前，将合并文件路径传递给PythonProcessor
        m_pythonProcessor->setMergedFilePath(m_integratedMergedPath);

        // 获取测试员和审核员信息
        QString tester = ui->testerlineEdit->text().trimmed();
        QString reviewer = ui->reviewerslineEdit->text().trimmed();

        // 传递信息给Python处理器
        emit m_pythonProcessor->setTesterReviewerInfo(tester, reviewer);

        m_progressDialog->setLabelText("正在生成建模点选择界面...");
        processIntegratedMergedFileWithDialog(m_integratedMergedPath);
    } else {
        m_progressDialog->hide();
        QMessageBox::critical(this, "错误", "文件合并失败：未获取到有效合并路径或文件不存在");
    }
}

// 处理合并后的文件，生成模板并选择建模点
void MainWindow::processIntegratedMergedFileWithDialog(const QString& mergedFilePath)
{
    if (mergedFilePath.isEmpty() || !QFile::exists(mergedFilePath)) {
        m_progressDialog->hide();
        return;
    }

    QXlsx::Document mergedDoc(mergedFilePath);
    if (!mergedDoc.load()) {
        QMessageBox::critical(this, "错误", "合并文件加载失败");
        m_progressDialog->hide();
        return;
    }

    QVector<bool> lastSelections;
    m_integratedTemplatePaths.clear(); // 清空模板路径列表
    int sheetCount = mergedDoc.sheetNames().count();
    int processedSheets = 0;
    bool userCancelled = false;

    foreach (const QString& sheetName, mergedDoc.sheetNames()) {
        if (sheetName == "标准") continue;

        // 仅处理符合多头命名规则的工作表（逻辑不变）
        static const QRegularExpression re(R"(^COM\d+-多(\d+)$)");
        if (!re.match(sheetName).hasMatch()) continue;

        mergedDoc.selectSheet(sheetName);
        QVector<double> temperatures;
        QVector<QString> conditions;
        int emptyCount = 0;

        // 读取D列和P列数据（逻辑不变）
        for (int row = 4; emptyCount < 5; ++row) {
            QVariant tempVar = mergedDoc.read(row, 4);   // D列
            QVariant condVar = mergedDoc.read(row, 16);  // P列
            if (tempVar.isNull() || condVar.isNull()) {
                ++emptyCount;
                continue;
            }
            emptyCount = 0;
            conditions.append(condVar.toString());
            temperatures.append(tempVar.toDouble());
        }

        if (temperatures.isEmpty()) continue;

        // 弹出选择对话框（逻辑不变）
        ModelingPointDialog dlg(temperatures, conditions, sheetName, lastSelections, this);
        if (dlg.exec() != QDialog::Accepted) {
            userCancelled = true;
            break;
        }

        QVector<bool> selections = dlg.getSelections();
        lastSelections = selections;

        // 调用多头模板生成函数并收集路径（关键修改）
        QString templatePath = processSelectedData(mergedDoc, sheetName, selections, mergedFilePath);
        if (!templatePath.isEmpty()) {
            m_integratedTemplatePaths.append(templatePath); // 收集生成的模板路径
        } else {
            QMessageBox::warning(this, "提示", "当前工作表模板生成失败，跳过处理");
        }

        // 更新进度（逻辑不变）
        processedSheets++;
        int progress = 30 + (processedSheets * 30 / sheetCount);
        m_progressDialog->setValue(progress);
    }

    if (userCancelled) {
        if (!m_integratedTemplatePaths.isEmpty()) {
            QMessageBox::warning(this, "提示", "已取消后续模板生成，将对已生成的模板进行拟合");
        } else {
            m_progressDialog->hide();
            QMessageBox::warning(this, "提示", "已取消模板生成");
            return;
        }
    }

    // 处理所有模板（逻辑不变）
    if (!m_integratedTemplatePaths.isEmpty()) {
        m_progressDialog->setLabelText("正在进行数据拟合...");
        m_progressDialog->setValue(60);
        processAllMultiTemplatesFitting(); // 使用收集的模板路径启动拟合
    } else {
        m_progressDialog->hide();
        QMessageBox::information(this, "完成", "未生成任何拟合模板");
    }
}

// 处理所有生成的模板进行数据拟合
void MainWindow::processAllMultiTemplatesFitting()
{
    if (m_integratedTemplatePaths.isEmpty()) {
        m_progressDialog->hide();
        return;
    }

    // 初始化模板处理队列和索引
    m_templateQueue = QStringList::fromVector(m_integratedTemplatePaths);
    m_currentTemplateIndex = 0;
    int total = m_templateQueue.size();

    m_progressDialog->setLabelText("开始处理所有拟合模板...");
    m_progressDialog->setRange(60, 100);
    m_progressDialog->setValue(60);
    m_progressDialog->show();

    qDebug() << "待处理多头模板数量:" << total;
    processNextMultiTemplate();
}

// 处理下一个模板的拟合任务
void MainWindow::processNextMultiTemplate()
{
    qDebug() << "进入processNextMultiTemplate，当前索引:" << m_currentTemplateIndex;

    // 检查是否所有模板都已处理
    if (m_currentTemplateIndex >= m_templateQueue.size()) {
        qDebug() << "所有多头设备拟合完成";
        m_progressDialog->hide();
        QMessageBox::information(this, "完成", "所有多头设备数据处理完成！");
        return;
    }

    // 确保Python进程完全终止（如果有残留）
    if (m_pythonProcessor->isProcessing()) {
        qDebug() << "Python进程仍在运行，等待终止...";
        // 可以添加等待逻辑或强制终止
        QThread::msleep(1000);
    }

    const QString& templatePath = m_templateQueue[m_currentTemplateIndex];
    qDebug() << "当前处理模板:" << templatePath << "("
             << (m_currentTemplateIndex + 1) << "/" << m_templateQueue.size() << ")";

    // 提取设备编号用于进度显示
    QFileInfo fileInfo(templatePath);
    QString deviceName = fileInfo.baseName();
    if (deviceName.startsWith("多")) {
        deviceName = deviceName.left(deviceName.indexOf('.')); // 去除文件扩展名
    } else {
        deviceName = "设备" + QString::number(m_currentTemplateIndex + 1);
    }

    // 更新进度
    int progress = 60 + (40 * m_currentTemplateIndex / m_templateQueue.size());
    m_progressDialog->setValue(progress);
    m_progressDialog->setLabelText(QString("正在拟合 %1: %2")
                                       .arg(deviceName)
                                       .arg(QFileInfo(templatePath).fileName()));

    // 使用一次性连接确保只处理当前模板
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(m_pythonProcessor, &PythonProcessor::processingFinished,
                          this, [this, templatePath, connection](bool success, const QString& resultPath) {
                              // 断开连接，避免重复触发
                              disconnect(*connection);

                              if (success) {
                                  qDebug() << "模板拟合成功:" << templatePath;
                              } else {
                                  qDebug() << "模板拟合失败:" << templatePath;
                              }

                              // 移动到下一个模板
                              m_currentTemplateIndex++;

                              // 延迟处理下一个模板，确保资源释放
                              QTimer::singleShot(1000, this, [this] {
                                  processNextMultiTemplate();
                              });
                          });

    // 启动Python拟合处理（调用现有第三按钮的逻辑）
    QFileInfo fi(templatePath);
    QString baseName = fi.baseName();
    static QRegularExpression nidRegex("^多\\d+"); // 匹配多设备编号
    QRegularExpressionMatch match = nidRegex.match(baseName);

    if (match.hasMatch()) {
        QString nid = match.captured(0);
        m_pythonProcessor->startMultiProcessing(templatePath, nid);
    } else {
        // 处理可能的命名不规范情况
        m_pythonProcessor->startMultiProcessing(templatePath, "多未知");
    }
}

void MainWindow::setupIRTCommTab()
{
    QSettings settings("config.ini", QSettings::IniFormat);
    QStringList portNamesWithIds = settings.value("devices/com_ports")
                                       .toString()
                                       .replace("\"", "")
                                       .split(",", Qt::SkipEmptyParts);

    QStringList portNames;
    QHash<QString, QString> deviceIdMap; // 存储设备ID和COM口的映射关系

    qDebug() << "读取到的COM口配置:" << settings.value("devices/com_ports").toString();

    for (const QString &portWithId : portNamesWithIds) {
        QStringList parts = portWithId.split('-');
        if (parts.size() == 2) {
            QString deviceId = parts[0].trimmed();
            QString portName = parts[1].trimmed();
            portNames.append(portName);
            deviceIdMap.insert(deviceId, portName);

            qDebug() << "解析成功 - 机位号:" << deviceId << "COM口号:" << portName;
        } else {
            // 格式不符合预期，直接添加原始字符串
            portNames.append(portWithId);
            qWarning() << "COM口号格式异常，使用原始值:" << portWithId;
        }
    }

    if (portNames.isEmpty()) {
        qWarning() << "配置文件中未找到串口号！";
    } else {
        qDebug() << "最终解析得到的COM口号列表:" << portNames.join(", ");
    }

    // 全局样式设置（统一风格）
    QString tableStyle = "QTableWidget {"
                         "   background-color: #f5f5f5;"
                         "   alternate-background-color: #e8f0fe;"
                         "   border: 1px solid #cccccc;"
                         "   border-radius: 4px;"
                         "   gridline-color: #dddddd;"
                         "}"
                         "QTableWidget::header {"
                         "   background-color: #4a90e2;"
                         "   color: white;"
                         "   font-weight: bold;"
                         "   border: none;"
                         "   padding: 6px;"
                         "}"
                         "QTableWidget::item {"
                         "   padding: 4px;"
                         "   border: none;"
                         "}";

    QString buttonStyle = "QPushButton {"
                          "   background-color: #4a90e2;"
                          "   color: white;"
                          "   border: none;"
                          "   padding: 5px 12px;"
                          "   border-radius: 3px;"
                          "   font-size: 20px;"
                          "}"
                          "QPushButton:hover {"
                          "   background-color: #3a80d2;"
                          "}"
                          "QPushButton:pressed {"
                          "   background-color: #2a70c2;"
                          "}";

    QString comboStyle = "QComboBox {"
                         "   border: 1px solid #cccccc;"
                         "   border-radius: 3px;"
                         "   padding: 3px 8px;"
                         "   background-color: white;"
                         "}"
                         "QComboBox::drop-down {"
                         "   border-left: 1px solid #cccccc;"
                         "}"
                         "QComboBox::down-arrow {"
                         "   image: url(:/icons/arrow-down.png);"
                         "   width: 12px;"
                         "   height: 12px;"
                         "}";

    QString labelStyle = "QLabel {"
                         "   color: #333333;"
                         "   font-size: 20px;"
                         "}";

    // 第一页：温度数据表格
    QWidget *tableTab = new QWidget();
    QVBoxLayout *tableLayout = new QVBoxLayout(tableTab);
    tableLayout->setContentsMargins(10, 10, 10, 10);
    tableLayout->setSpacing(8);

    // 创建实时数据表格
    QTableWidget *tempTable = new QTableWidget();
    tempTable->setStyleSheet(tableStyle);
    tempTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tempTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    tempTable->setAlternatingRowColors(true);
    tempTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    tempTable->verticalHeader()->setVisible(false);

    // 设置表格列标题（保留LC列）
    QStringList headers;
    headers << "COM口号" << "设备类型"
            << "TO-1" << "TA-1" << "LC-1"  // 单头设备对应列
            << "TO-2" << "TA-2" << "LC-2"  // 多头设备扩展列
            << "TO-3" << "TA-3" << "LC-3"
            << "接收时间";  // 保持12列
    tempTable->setColumnCount(headers.size());
    tempTable->setHorizontalHeaderLabels(headers);

    // 调整列宽（保持LC列的宽度设置）
    tempTable->setColumnWidth(0, 80);   // COM口号
    tempTable->setColumnWidth(1, 80);   // 设备类型
    tempTable->setColumnWidth(11, 140); // 接收时间

    // 显式设置表格行数（参照第二段代码逻辑）
    tempTable->setRowCount(portNames.size());

    // 初始化表格行（整合第二段代码的直接初始化风格）
    for (int i = 0; i < portNames.size(); ++i) {
        const QString &portName = portNames[i];

        // 直接创建COM口号单元格（简化逻辑）
        tempTable->setItem(i, 0, new QTableWidgetItem(portName));
        tempTable->item(i, 0)->setForeground(QColor("#2c3e50"));
        tempTable->item(i, 0)->setFont(QFont("SimHei", 18, QFont::Medium));

        // 设备类型单元格
        tempTable->setItem(i, 1, new QTableWidgetItem("未知"));
        tempTable->item(i, 1)->setForeground(QColor("#e74c3c"));

        // 初始化数据列（包含LC列）
        for (int col = 2; col < 12; ++col) {
            tempTable->setItem(i, col, new QTableWidgetItem(""));
        }

        // 初始化映射关系（参照第二段代码的加锁方式）
        QMutexLocker locker(&portRowMapMutex);
        portRowMap[portName] = i;
    }

    tableLayout->addWidget(tempTable);
    ui->IRTCommTab->addTab(tableTab, "温度数据");
    ui->IRTCommTab->setStyleSheet("QTabBar::tab {"
                                  "   padding: 8px 16px;"
                                  "   font-size: 18px;"
                                  "}"
                                  "QTabBar::tab:selected {"
                                  "   background-color: #e8f0fe;"
                                  "   border-bottom: 2px solid #4a90e2;"
                                  "}");

    // 创建线程
    m_serialThreads.clear();
    for (int i = 0; i < portNames.size(); i++) {
        SerialPortThread *thread = new SerialPortThread(portNames[i], 9600, this);
        m_serialThreads.append(thread);

        // 连接温度数据信号（保留LC列数据处理逻辑）
        connect(thread, &SerialPortThread::temperatureDataReceived,
                this, [=](const QString& portName,
                    const QDateTime& timestamp,
                    const QVector<double>& groupST,    // TO组
                    const QVector<double>& groupTA,    // TA组
                    const QVector<double>& groupLC,    // LC组
                    bool isSingleHead) {
                    QMetaObject::invokeMethod(this, [=]() {
                        QMutexLocker locker(&portRowMapMutex);
                        if (!portRowMap.contains(portName)) {
                            qWarning() << "未找到串口号映射:" << portName;
                            return;
                        }
                        int row = portRowMap[portName];
                        if (row < 0 || row >= tempTable->rowCount()) {
                            qWarning() << "无效的表格行索引:" << row;
                            return;
                        }

                        // 更新设备类型
                        tempTable->item(row, 1)->setText(isSingleHead ? "单头" : "多头");
                        tempTable->item(row, 1)->setForeground(QColor("#27ae60"));

                        // 更新数据（保留LC列处理）
                        if (isSingleHead) {
                            // TO-1、TA-1、LC-1
                            if (groupST.size() > 0) {
                                tempTable->item(row, 2)->setText(QString::number(groupST[0], 'f', 2));
                                tempTable->item(row, 2)->setForeground(QColor("#3498db"));
                            }
                            if (groupTA.size() > 0) {
                                tempTable->item(row, 3)->setText(QString::number(groupTA[0], 'f', 2));
                                tempTable->item(row, 3)->setForeground(QColor("#e67e22"));
                            }
                            if (groupLC.size() > 0) {
                                tempTable->item(row, 4)->setText(QString::number(groupLC[0], 'f', 2));  // LC-1
                                tempTable->item(row, 4)->setForeground(QColor("#2ecc71"));
                            }
                            // 清空多头设备的列
                            for (int col = 5; col < 11; ++col) {  // 从TO-2到LC-3的列
                                tempTable->item(row, col)->setText("");
                            }
                        } else {
                            // 多头设备：更新所有TO、TA、LC列
                            for (int j = 0; j < 3; ++j) {  // 3组传感器
                                // TO-j+1（列索引：2 + j*3）
                                if (j < groupST.size()) {
                                    tempTable->item(row, 2 + j*3)->setText(QString::number(groupST[j], 'f', 2));
                                    tempTable->item(row, 2 + j*3)->setForeground(QColor("#3498db"));
                                } else {
                                    tempTable->item(row, 2 + j*3)->setText("");
                                }
                                // TA-j+1（列索引：3 + j*3）
                                if (j < groupTA.size()) {
                                    tempTable->item(row, 3 + j*3)->setText(QString::number(groupTA[j], 'f', 2));
                                    tempTable->item(row, 3 + j*3)->setForeground(QColor("#e67e22"));
                                } else {
                                    tempTable->item(row, 3 + j*3)->setText("");
                                }
                                // LC-j+1（列索引：4 + j*3）
                                if (j < groupLC.size()) {
                                    tempTable->item(row, 4 + j*3)->setText(QString::number(groupLC[j], 'f', 2));
                                    tempTable->item(row, 4 + j*3)->setForeground(QColor("#2ecc71"));
                                } else {
                                    tempTable->item(row, 4 + j*3)->setText("");
                                }
                            }
                        }

                        // 更新接收时间
                        tempTable->item(row, 11)->setText(timestamp.toString("HH:mm:ss"));
                        tempTable->item(row, 11)->setForeground(QColor("#7f8c8d"));
                    }, Qt::QueuedConnection);
                });
    }

    // 保存表格指针
    m_tempTable = tempTable;
    qDebug() << "[MainWindow] 温度表格初始化完成，行数:" << tempTable->rowCount()
             << "，列数:" << tempTable->columnCount();

    // 为每个串口创建标签页（保持原有逻辑）
    for (int i = 0; i < portNames.size(); i++) {
        QString currentPortName = portNames[i];  // 使用非const变量，允许修改
        QWidget *tabPage = new QWidget();
        QVBoxLayout *layout = new QVBoxLayout(tabPage);
        layout->setContentsMargins(10, 10, 10, 10);
        layout->setSpacing(8);

        // 串口控制区域
        QWidget *controlWidget = new QWidget();
        controlWidget->setStyleSheet("background-color: #f9f9f9; border-radius: 4px; padding: 5px;");
        QHBoxLayout *portControlLayout = new QHBoxLayout(controlWidget);
        portControlLayout->setContentsMargins(5, 5, 5, 5);
        portControlLayout->setSpacing(10);

        QLabel *portLabel = new QLabel("串口:");
        portLabel->setStyleSheet(labelStyle);
        QComboBox *portComboBox = new QComboBox();
        portComboBox->setStyleSheet(comboStyle);

        QLabel *baudRateLabel = new QLabel("波特率:");
        baudRateLabel->setStyleSheet(labelStyle);
        QComboBox *baudRateComboBox = new QComboBox();
        baudRateComboBox->setStyleSheet(comboStyle);

        QPushButton *portControlBtn = new QPushButton("关闭串口");
        portControlBtn->setStyleSheet(buttonStyle);

        QLabel *statusLabel = new QLabel("状态: 已连接");
        statusLabel->setStyleSheet("QLabel {"
                                   "   color: #27ae60;"
                                   "   font-weight: bold;"
                                   "   padding: 0 5px;"
                                   "}");

        portControlLayout->addWidget(portLabel);
        portControlLayout->addWidget(portComboBox);
        portControlLayout->addWidget(baudRateLabel);
        portControlLayout->addWidget(baudRateComboBox);
        portControlLayout->addWidget(portControlBtn);
        portControlLayout->addWidget(statusLabel);
        portControlLayout->addStretch();
        layout->addWidget(controlWidget);

        // 时间戳勾选框
        QHBoxLayout *timestampLayout = new QHBoxLayout();
        timestampLayout->setSpacing(10);
        QCheckBox *timestampCheckBox = new QCheckBox("接收区添加时间戳");
        timestampCheckBox->setChecked(true);
        timestampCheckBox->setStyleSheet("QCheckBox { color: #333333; }");
        timestampLayout->addWidget(timestampCheckBox);
        timestampLayout->addStretch();
        layout->addLayout(timestampLayout);

        // 接收区
        QWidget *receiveWidget = new QWidget();
        receiveWidget->setStyleSheet("border: 1px solid #cccccc; border-radius: 4px;");
        QVBoxLayout *receiveLayout = new QVBoxLayout(receiveWidget);
        receiveLayout->setContentsMargins(0, 0, 0, 0);

        QTextEdit *receiveTextEdit = new QTextEdit();
        receiveTextEdit->setReadOnly(true);
        receiveTextEdit->setStyleSheet("QTextEdit {"
                                       "   background-color: #f0f7ff;"
                                       "   color: #2c3e50;"
                                       "   border: none;"
                                       "   border-radius: 3px;"
                                       "   padding: 8px;"
                                       "   font-family: SimHei, monospace;"
                                       "   font-size: 18px;"
                                       "}");
        receiveTextEdit->setMinimumHeight(200);
        receiveLayout->addWidget(receiveTextEdit);
        layout->addWidget(receiveWidget);

        // 文件保存组件
        QWidget *fileWidget = new QWidget();
        fileWidget->setStyleSheet("background-color: #f9f9f9; border-radius: 4px; padding: 5px;");
        QHBoxLayout *fileLayout = new QHBoxLayout(fileWidget);
        fileLayout->setContentsMargins(5, 5, 5, 5);
        fileLayout->setSpacing(10);

        QLineEdit *filePathEdit = new QLineEdit(QDir::homePath() + "/" +
                                                QDateTime::currentDateTime().toString("yyyyMMddHHmmss") + "_" + currentPortName + "_" +
                                                QDateTime::currentDateTime().toString("yyyyMMdd") + ".txt");
        filePathEdit->setStyleSheet("QLineEdit { border: 1px solid #cccccc; border-radius: 3px; padding: 3px 5px; }");

        QPushButton *browseBtn = new QPushButton("浏览");
        browseBtn->setStyleSheet(buttonStyle);
        connect(browseBtn, &QPushButton::clicked, this, &MainWindow::onBrowseButtonClicked);
        fileLayout->addWidget(filePathEdit);
        fileLayout->addWidget(browseBtn);

        QCheckBox *saveCheckBox = new QCheckBox("保存数据");
        saveCheckBox->setChecked(true);
        saveCheckBox->setStyleSheet("QCheckBox { color: #333333; }");
        fileLayout->addWidget(saveCheckBox);
        layout->addWidget(fileWidget);

        // 发送组件
        QWidget *sendWidget = new QWidget();
        sendWidget->setStyleSheet("background-color: #f9f9f9; border-radius: 4px; padding: 5px;");
        QHBoxLayout *sendLayout = new QHBoxLayout(sendWidget);
        sendLayout->setContentsMargins(5, 5, 5, 5);
        sendLayout->setSpacing(10);

        QComboBox *cmdCombo = new QComboBox();
        cmdCombo->setStyleSheet(comboStyle);
        QLineEdit *sendEdit = new QLineEdit();
        sendEdit->setStyleSheet("QLineEdit { border: 1px solid #cccccc; border-radius: 3px; padding: 3px 5px; }");
        QPushButton *sendBtn = new QPushButton("发送");
        sendBtn->setStyleSheet(buttonStyle);
        QCheckBox *newLineCheck = new QCheckBox("换行");
        newLineCheck->setStyleSheet("QCheckBox { color: #333333; }");

        loadCommonCommands(cmdCombo);
        sendLayout->addWidget(cmdCombo);
        sendLayout->addWidget(sendEdit);
        sendLayout->addWidget(sendBtn);
        sendLayout->addWidget(newLineCheck);
        layout->addWidget(sendWidget);

        // 发送按钮点击事件
        connect(sendBtn, &QPushButton::clicked, [=]() {
            QString sendText = sendEdit->text();
            QByteArray sendData = sendText.toUtf8();
            if (newLineCheck->isChecked()) {
                sendData += "\r\n";
            }

            if (i < m_serialThreads.size()) {
                SerialPortThread *serialThread = m_serialThreads[i];
                if (serialThread) {
                    serialThread->sendData(sendData);
                }
            }

            QString displayMessage;
            if (timestampCheckBox->isChecked()) {
                QString sendTimestamp = QDateTime::currentDateTime().toString("[S:yyyy-MM-dd HH:mm:ss]");
                displayMessage = QString("%1 %2").arg(sendTimestamp, sendText);
            } else {
                displayMessage = sendText;
            }

            receiveTextEdit->append(displayMessage);

            if (saveCheckBox->isChecked() && !filePathEdit->text().isEmpty()) {
                QFile file(filePathEdit->text());
                if (file.open(QIODevice::Append)) {
                    QTextStream(&file) << displayMessage << "\n";
                }
            }

            sendEdit->clear();
        });

        // 初始化控件
        initializePortComboBox(portComboBox, currentPortName);
        initializeBaudRateComboBox(baudRateComboBox);

        // 获取当前线程
        SerialPortThread *thread = m_serialThreads[i];

        // 数据接收信号
        connect(thread, &SerialPortThread::dataReceived, [=](const QByteArray &data) {
            QString message;
            if (timestampCheckBox->isChecked()) {
                QString ts = QDateTime::currentDateTime().toString("[R:yyyy-MM-dd HH:mm:ss]");
                message = QString("%1 %2").arg(ts, QString::fromUtf8(data));
            } else {
                message = QString::fromUtf8(data);
            }
            receiveTextEdit->append(message);

            if (saveCheckBox->isChecked() && !filePathEdit->text().isEmpty()) {
                QFile file(filePathEdit->text());
                if (file.open(QIODevice::Append)) {
                    QTextStream(&file) << message << "\n";
                }
            }
        });

        // 标签页索引
        int tabIndex = i + 1;

        // 端口状态变化信号处理（保持原有逻辑）
        connect(thread, &SerialPortThread::portStatusChanged, this,
                [=](bool isOpen) mutable {
                    if (isOpen) {
                        statusLabel->setText("状态: 已连接");
                        statusLabel->setStyleSheet("QLabel { color: #27ae60; font-weight: bold; }");
                        portControlBtn->setText("关闭串口");

                        QString newPortName = thread->portName();
                        if (newPortName != currentPortName) {
                            // 1. 清空当前行的所有旧数据
                            if (i < tempTable->rowCount()) {
                                for (int col = 1; col < tempTable->columnCount(); ++col) {
                                    tempTable->item(i, col)->setText("");
                                }
                                tempTable->item(i, 0)->setText(newPortName);
                            }

                            // 2. 更新标签页标题
                            ui->IRTCommTab->setTabText(tabIndex, newPortName);

                            // 3. 更新文件名
                            QString currentFilePath = filePathEdit->text();
                            QFileInfo fileInfo(currentFilePath);
                            QString baseName = fileInfo.baseName();
                            QRegularExpression regex(R"((\d{14})_(\w+)_(\d{8}))");
                            QRegularExpressionMatch match = regex.match(baseName);

                            if (match.hasMatch()) {
                                QString timestamp = match.captured(1);
                                QString date = match.captured(3);
                                QString newBaseName = QString("%1_%2_%3").arg(timestamp, newPortName, date);
                                QString newFileName = fileInfo.absolutePath() + "/" + newBaseName + ".txt";
                                filePathEdit->setText(newFileName);
                            }

                            // 4. 更新映射关系
                            {
                                QMutexLocker locker(&portRowMapMutex);
                                portRowMap.remove(currentPortName);
                                portRowMap[newPortName] = i;
                            }
                            currentPortName = newPortName;
                        }
                    } else {
                        statusLabel->setText("状态: 未连接");
                        statusLabel->setStyleSheet("QLabel { color: #e74c3c; font-weight: bold; }");
                        portControlBtn->setText("打开串口");
                    }
                }, Qt::QueuedConnection);

        // 其他信号连接
        connect(cmdCombo, &QComboBox::currentTextChanged, [=](const QString &text) {
            if (!text.isEmpty()) sendEdit->setText(text);
        });

        // 端口控制按钮点击事件
        connect(portControlBtn, &QPushButton::clicked, [=]() {
            if (portControlBtn->text() == "打开串口") {
                QString newPortName = portComboBox->currentText();
                thread->setPortName(newPortName);
                thread->setBaudRate(baudRateComboBox->currentText().toInt());
                thread->openPort();
            } else {
                thread->closePort();
            }
        });

        // 启动线程
        thread->start();
        thread->openPort();

        ui->IRTCommTab->addTab(tabPage, currentPortName);
    }

    // 标签页切换信号
    connect(ui->IRTCommTab, &QTabWidget::currentChanged, this, [this](int index) {
        if (index > 0 && index <= m_serialThreads.size()) {
            SerialPortThread *thread = m_serialThreads[index-1];
            QWidget *tab = ui->IRTCommTab->widget(index);
            if (tab) {
                QComboBox *portComboBox = tab->findChild<QComboBox*>();
                if (portComboBox && thread) {
                    portComboBox->setCurrentText(thread->portName());
                }
            }
        }
    });
}

// void MainWindow::initializeSerialPort(int index, const QString &portName,
//                                       QTextEdit *receiveTextEdit,
//                                       QLineEdit *filePathEdit,
//                                       QCheckBox *saveCheckBox,
//                                       QComboBox *portComboBox,
//                                       QComboBox *baudRateComboBox,
//                                       QPushButton *portControlBtn,
//                                       QLabel *statusLabel)
// {
//     int baudRate = baudRateComboBox->currentText().toInt();
//     SerialPortThread *thread = new SerialPortThread(portName, baudRate);

//     // 确保线程列表足够大
//     if (index >= m_serialThreads.size()) {
//         m_serialThreads.resize(index + 1);
//     }
//     m_serialThreads[index] = thread;

//     // 数据接收处理
//     connect(thread, &SerialPortThread::dataReceived, [=](const QByteArray &data){
//         QString timestamp = QDateTime::currentDateTime().toString("[R:yyyy-MM-dd HH:mm:ss.zzz]");
//         QString msg = QString("%1 %2").arg(timestamp, QString::fromUtf8(data));

//         receiveTextEdit->append(msg);

//         if(saveCheckBox->isChecked() && !filePathEdit->text().isEmpty()){
//             QFile file(filePathEdit->text());
//             if(file.open(QIODevice::Append)){
//                 QTextStream(&file) << msg << "\n";
//             }
//         }
//     });

//     // 温度数据更新图表
//     connect(thread, &SerialPortThread::temperatureDataReceived,
//             this, &MainWindow::updateChart);

//     // 串口状态更新 - 使用QueuedConnection确保在主线程执行UI更新
//     connect(thread, &SerialPortThread::portStatusChanged, this,
//             [=](bool isOpen) {
//                 // UI更新必须在主线程执行
//                 QMetaObject::invokeMethod(this, [=]{
//                     if (isOpen) {
//                         portControlBtn->setText("关闭串口");
//                         statusLabel->setText("状态: 已连接");
//                         statusLabel->setStyleSheet("color: green");
//                     } else {
//                         portControlBtn->setText("打开串口");
//                         statusLabel->setText("状态: 未连接");
//                         statusLabel->setStyleSheet("color: red");
//                     }
//                 }, Qt::QueuedConnection);
//             });

//     thread->start();

// }



void MainWindow::initializePortComboBox(QComboBox *comboBox, const QString &defaultPort)
{
    comboBox->clear();
    const auto ports = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &port : ports) {
        comboBox->addItem(port.portName());
    }

    int index = comboBox->findText(defaultPort);
    if (index >= 0) {
        comboBox->setCurrentIndex(index);
    } else if (!ports.isEmpty()) {
        comboBox->setCurrentIndex(0);
    }

}

void MainWindow::initializeBaudRateComboBox(QComboBox *comboBox)
{
    comboBox->addItems({
        "9600",
        "115200",
        "4800",
        "19200",
        "38400",
        "57600"
    });
    comboBox->setCurrentText("9600");
}

void MainWindow::onPortControlButtonClicked(const QString &portName,
                                            QComboBox *baudRateComboBox,
                                            QPushButton *button,
                                            QLabel *statusLabel)
{
    int tabIndex = ui->IRTCommTab->currentIndex();
    if (tabIndex == 0) return;

    int threadIndex = tabIndex - 1;
    if (threadIndex < 0 || threadIndex >= m_serialThreads.size()) return;

    SerialPortThread *thread = m_serialThreads[threadIndex];
    if (!thread) return;

    if (button->text() == "打开串口") {
        // 关键：先更新线程的端口名（使用用户选择的新端口）
        thread->setPortName(portName);
        // 再设置波特率并打开
        thread->setBaudRate(baudRateComboBox->currentText().toInt());
        thread->openPort();
        button->setText("关闭串口");
    } else {
        thread->closePort();
        button->setText("打开串口");
    }
}


void MainWindow::loadCommonCommands(QComboBox *commandComboBox)
{
    QSettings settings("config.ini", QSettings::IniFormat);
    QStringList commands = settings.value("common_commands").toStringList();
    commandComboBox->clear();
    commandComboBox->addItems(commands);

    if(commands.isEmpty()) {
        commandComboBox->addItems({
            "#GET_DATA",
            "#START_MEASURE",
            "#STOP_MEASURE"
        });
    }
}

void MainWindow::addCommandToCommonList(QComboBox* comboBox, const QString& cmd)
{
    if (comboBox->findText(cmd) != -1) return;

    comboBox->insertItem(0, cmd);
    while (comboBox->count() > 10) {
        comboBox->removeItem(comboBox->count() - 1);
    }
}

void MainWindow::onBrowseButtonClicked()
{
    QPushButton *senderBtn = qobject_cast<QPushButton*>(sender());
    if (!senderBtn) return;

    // 查找父容器中的QLineEdit
    QWidget *parentWidget = senderBtn->parentWidget();
    QLineEdit *filePathEdit = parentWidget->findChild<QLineEdit*>();
    if (!filePathEdit) return;

    // 通过标签页索引获取端口名（更可靠的方法）
    int tabIndex = ui->IRTCommTab->indexOf(parentWidget->parentWidget());
    QString portName;

    if (tabIndex > 0 && tabIndex <= m_serialThreads.size()) {
        // 从串口线程获取端口名（优先）
        portName = m_serialThreads[tabIndex - 1]->portName();
    }

    if (portName.isEmpty()) {
        // 备用：从标签页标题获取
        portName = ui->IRTCommTab->tabText(tabIndex);
    }

    // 生成默认文件名（确保包含端口名）
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMddHHmmss");
    QString defaultFileName = timestamp + "_" + portName + "_" + QDate::currentDate().toString("yyyyMMdd") + ".txt";

    // 获取用户上次选择的路径
    QSettings settings("config.ini", QSettings::IniFormat);
    QString lastPath = settings.value("last_save_path", QDir::homePath()).toString();
    QString initialPath = QDir(lastPath).filePath(defaultFileName);

    // 显示文件保存对话框
    QString filePath = QFileDialog::getSaveFileName(
        this, "选择保存路径", initialPath, "文本文件 (*.txt);;所有文件 (*)"
        );

    if (!filePath.isEmpty()) {
        filePathEdit->setText(filePath);
        settings.setValue("last_save_path", QFileInfo(filePath).absolutePath());
    }
}

// 从config.ini加载配置到UI控件
void MainWindow::loadConfigToUI()
{
    QSettings settings("config.ini", QSettings::IniFormat);

    // 1. 加载红外测温仪端口配置（修改为QTextEdit）
    QString devicesCom = settings.value("devices/com_ports", "1-COM7,2-COM6").toString();
    ui->devicesComPortsEdit->setPlainText(devicesCom.remove("\""));  // 修改点：使用setPlainText

    // 波特率
    QString baudRate = settings.value("serial_config/baud_rate", "9600").toString();
    ui->serialBaudRateComboBox->setCurrentText(baudRate);

    // 数据位
    QString dataBits = settings.value("serial_config/data_bits", "8").toString();
    ui->serialDataBitsComboBox->setCurrentText(dataBits);

    // 停止位
    QString stopBits = settings.value("serial_config/stop_bits", "1").toString();
    ui->serialStopBitsComboBox->setCurrentText(stopBits);

    // 校验位
    QString parity = settings.value("serial_config/parity", "none").toString();
    ui->serialParityComboBox->setCurrentText(parity);

    // 移除配置文件中可能的引号（避免显示多余引号）
    ui->devicesComPortsEdit->setText(devicesCom.remove("\""));

    // 2. 加载黑体炉端口（[blackbody] -> com_port）
    QString blackbodyCom = settings.value("blackbody/com_port", "COM8").toString();
    ui->blackbodyComEdit->setText(blackbodyCom);

    // 3. 加载恒温箱端口（[humidity] -> com_port）
    QString humidityCom = settings.value("humidity/com_port", "COM11").toString();
    ui->humidityComEdit->setText(humidityCom);


    // 加载标定温度配置到"标定配置"页
    QString multiHeadOut = settings.value("calibration_temperatures/multi_head_out",
                                          "30,40,50,60,70,25,20,15,10,5,0,-5,-10,-15,-20,-25,-30").toString();
    QString multiHeadIn = settings.value("calibration_temperatures/multi_head_in",
                                         "30,40,50,60,70,30,25,20,15,10,5,0,-5,-10,-15,-20,-25,-30").toString();
    QString singleHeadOut = settings.value("calibration_temperatures/single_head_out",
                                           "30,40,50,60,70,25,20,15,10,5,0,-5,-10,-15,-20,-25").toString();
    QString singleHeadIn = settings.value("calibration_temperatures/single_head_in",
                                          "30,40,50,60,70,30,25,20,15,10,5,0,-5,-10,-15,-20,-25").toString();

    // 显示到"标定配置"页的QLineEdit（假设控件名对应）
    ui->multiHeadOutTempEdit->setText(multiHeadOut);
    ui->multiHeadInTempEdit->setText(multiHeadIn);
    ui->singleHeadOutTempEdit->setText(singleHeadOut);
    ui->singleHeadInTempEdit->setText(singleHeadIn);

}

// 保存按钮点击事件：将UI控件的值写入config.ini
void MainWindow::on_saveConfigButton_clicked()
{
    QSettings settings("config.ini", QSettings::IniFormat);
    bool isSuccess = true;
    QString errorMsg;

    // 1. 验证并保存“红外测温仪端口”（修改为QTextEdit）
    QString devicesCom = ui->devicesComPortsEdit->toPlainText().trimmed();
    if (devicesCom.isEmpty()) {
        isSuccess = false;
        errorMsg += "红外测温仪端口不能为空；";
    } else {
        settings.setValue("devices/com_ports", devicesCom);
    }

    // 波特率
    QString baudRate = ui->serialBaudRateComboBox->currentText();
    settings.setValue("serial_config/baud_rate", baudRate);

    // 数据位
    QString dataBits = ui->serialDataBitsComboBox->currentText();
    settings.setValue("serial_config/data_bits", dataBits);

    // 停止位
    QString stopBits = ui->serialStopBitsComboBox->currentText();
    settings.setValue("serial_config/stop_bits", stopBits);

    // 校验位
    QString parity = ui->serialParityComboBox->currentText();
    settings.setValue("serial_config/parity", parity);


    // 2. 验证并保存“黑体炉端口”
    QString blackbodyCom = ui->blackbodyComEdit->text().trimmed();
    if (blackbodyCom.isEmpty() || !blackbodyCom.startsWith("COM", Qt::CaseInsensitive)) {
        isSuccess = false;
        errorMsg += "黑体炉端口格式无效（需以COM开头）；";
    } else {
        settings.setValue("blackbody/com_port", blackbodyCom);  // 写入配置
    }

    // 3. 验证并保存“恒温箱端口”
    QString humidityCom = ui->humidityComEdit->text().trimmed();
    if (humidityCom.isEmpty() || !humidityCom.startsWith("COM", Qt::CaseInsensitive)) {
        isSuccess = false;
        errorMsg += "恒温箱端口格式无效（需以COM开头）；";
    } else {
        settings.setValue("humidity/com_port", humidityCom);  // 写入配置
    }

    // ====== 新增：保存两个路径输入框的内容到配置文件 ======
    // 1. 保存黑体炉路径到 [blackbody] -> save_path
    QString bbSavePath = ui->savePathLineEdit->text().trimmed();
    if (!bbSavePath.isEmpty()) {
        settings.setValue("blackbody/save_path", bbSavePath);  // 写入[blackbody]节点
    } else {
        isSuccess = false;
        errorMsg += "黑体炉保存路径不能为空；";
    }

    // 2. 保存恒温箱路径到 [humidity] -> save_path
    QString humSavePath = ui->savePathLineEdit_3->text().trimmed();
    if (!humSavePath.isEmpty()) {
        settings.setValue("humidity/save_path", humSavePath);  // 写入[humidity]节点
    } else {
        isSuccess = false;
        errorMsg += "恒温箱保存路径不能为空；";
    }



    // 4. 显示结果提示
    if (isSuccess) {
        QMessageBox::information(this, "保存成功", "配置已成功写入config.ini\n请重启软件使配置生效");
    } else {
        QMessageBox::warning(this, "保存失败", "输入有误：\n" + errorMsg);
    }
}

void MainWindow::on_saveCalibrationConfigButton_clicked()
{
    QSettings settings("config.ini", QSettings::IniFormat);

    // 保存标定温度配置
    settings.setValue("calibration_temperatures/multi_head_out", ui->multiHeadOutTempEdit->text().trimmed());
    settings.setValue("calibration_temperatures/multi_head_in", ui->multiHeadInTempEdit->text().trimmed());
    settings.setValue("calibration_temperatures/single_head_out", ui->singleHeadOutTempEdit->text().trimmed());
    settings.setValue("calibration_temperatures/single_head_in", ui->singleHeadInTempEdit->text().trimmed());

    QMessageBox::information(this, "保存成功", "测量配置已保存");
}

void MainWindow::on_calibrationTypeComboBox_currentIndexChanged(int index)
{
    // 根据选择的类型获取对应的温度点
    QString tempPoints;
    bool isInside; // 是否箱内（箱内：恒温箱温度=黑体温度；箱外：恒温箱固定25℃）

    switch(index)
    {
    case 0: // 单头箱内
        tempPoints = ui->singleHeadInTempEdit->text();
        isInside = true;
        break;
    case 1: // 单头箱外
        tempPoints = ui->singleHeadOutTempEdit->text();
        isInside = false;
        break;
    case 2: // 多头箱内
        tempPoints = ui->multiHeadInTempEdit->text();
        isInside = true;
        break;
    case 3: // 多头箱外
        tempPoints = ui->multiHeadOutTempEdit->text();
        isInside = false;
        break;
    default:
        return;
    }

    // 填充blackbodyTempInput（测量点温度）
    ui->blackbodyTempInput->setText(tempPoints);

    // 日志提示（不再设置湿度输入框）
    qDebug() << QString("标定类型切换为：%1，恒温箱温度将%2")
                    .arg(ui->calibrationTypeComboBox->currentText())
                    .arg(isInside ? "与黑体炉温度保持一致" : "固定为25℃");
}

// 红外测量开始时启动定时器并记录当前COM口
void MainWindow::onIrMeasurementStarted(const QString &comPort) {
    qDebug() << "[MainWindow] 收到红外测量开始信号，COM口：" << comPort;

    if (!m_tempTable) {
        qWarning() << "[MainWindow] 温度表格指针为空，无法显示红外数据";
        return;
    }

    m_currentIrComPort = comPort;
    m_dualTempChart->setIrDataVisible(true); // 显示红外曲线
    m_irDataTimer->start(); // 启动定时器更新数据

    qDebug() << "[MainWindow] 红外数据显示已启动，定时器间隔：" << m_irDataTimer->interval() << "ms";
}

// 完善onIrMeasurementStopped槽函数
void MainWindow::onIrMeasurementStopped() {
    qDebug() << "[MainWindow] 收到红外测量结束信号，停止数据显示";
    m_irDataTimer->stop();
    m_dualTempChart->clearIrData(); // 清除红外数据
    m_dualTempChart->setIrDataVisible(false); // 隐藏红外曲线
    m_currentIrComPort.clear();
}

void MainWindow::updateIrChartFromTable() {
    qDebug() << "[MainWindow] 提取红外数据，COM口：" << m_currentIrComPort;

    if (!m_tempTable || m_currentIrComPort.isEmpty()) {
        qWarning() << "[MainWindow] 表格或COM口无效，跳过缓存";
        return;
    }

    // 1. 查找表格中对应COM口的行
    int targetRow = -1;
    for (int row = 0; row < m_tempTable->rowCount(); ++row) {
        QTableWidgetItem* portItem = m_tempTable->item(row, 0);
        if (portItem && portItem->text() == m_currentIrComPort) {
            targetRow = row;
            break;
        }
    }
    if (targetRow == -1) {
        qWarning() << "[MainWindow] 未找到COM口" << m_currentIrComPort << "的行";
        return;
    }

    // 2. 获取设备类型（表格第1列）
    QTableWidgetItem* typeItem = m_tempTable->item(targetRow, 1);
    QString deviceType = typeItem ? typeItem->text() : "未知";
    bool isSingle = (deviceType == "单头");
    qDebug() << "[MainWindow] 设备类型：" << deviceType << "，行：" << targetRow;

    // 声明图表更新用的温度变量
    float toTemp = NAN, taTemp = NAN;

    // 3. 提取温度数据并缓存（单头/多头区分）
    QMutexLocker locker(&m_irCacheMutex); // 加锁保证线程安全
    if (isSingle) {
        // 单头：提取TO1（第2列）、TA1（第3列）、LC1（第4列）
        float to1 = NAN, ta1 = NAN, lc1 = NAN;
        if (auto toItem = m_tempTable->item(targetRow, 2))
            to1 = toItem->text().toFloat();
        if (auto taItem = m_tempTable->item(targetRow, 3))
            ta1 = taItem->text().toFloat();
        if (auto lcItem = m_tempTable->item(targetRow, 4))  // 新增：提取LC1值
            lc1 = lcItem->text().toFloat();

        // 缓存最后60秒数据（1秒1条）- 改为存储包含LC1的复合结构
        auto& cache = m_irSingleCache[m_currentIrComPort];
        if (cache.size() >= 60) cache.removeFirst();
        // 修复：使用复合结构存储<<TO1, TA1>, LC1>
        cache.append(qMakePair(qMakePair(to1, ta1), lc1));
        qDebug() << "[MainWindow] 单头缓存更新，当前条数：" << cache.size();

        // 赋值图表用变量
        toTemp = to1;
        taTemp = ta1;
    } else {
        // 多头：提取TO1-3（第2/4/6列）、TA1-3（第3/5/7列）、LC1-3（第4/7/10列）
        QVector<float> toList, taList, lcList;  // 新增：LC列表
        int toCols[] = {2, 4, 6};   // TO1-3对应列索引
        int taCols[] = {3, 5, 7};   // TA1-3对应列索引
        int lcCols[] = {4, 7, 10};  // 新增：LC1-3对应列索引
        for (int i = 0; i < 3; ++i) {
            if (auto toItem = m_tempTable->item(targetRow, toCols[i]))
                toList.append(toItem->text().toFloat());
            else
                toList.append(NAN);
            if (auto taItem = m_tempTable->item(targetRow, taCols[i]))
                taList.append(taItem->text().toFloat());
            else
                taList.append(NAN);
            // 新增：提取LC值
            if (auto lcItem = m_tempTable->item(targetRow, lcCols[i]))
                lcList.append(lcItem->text().toFloat());
            else
                lcList.append(NAN);
        }

        // 缓存最后60秒数据 - 改为存储包含LC的复合结构
        auto& cache = m_irMultiCache[m_currentIrComPort];
        if (cache.size() >= 60) cache.removeFirst();
        // 修复：使用复合结构存储<<TOs, TAs>, LCs>
        cache.append(qMakePair(qMakePair(toList, taList), lcList));
        qDebug() << "[MainWindow] 多头缓存更新，当前条数：" << cache.size();

        // 仅使用TO1和TA1更新图表（满足需求）
        toTemp = toList.size() > 0 ? toList[0] : NAN;
        taTemp = taList.size() > 0 ? taList[0] : NAN;
    }

    // 4. 更新图表（仅当数据有效时）
    if (std::isfinite(toTemp) && std::isfinite(taTemp)) {
        qDebug() << "[MainWindow] 提取到有效温度数据 - TO:" << toTemp << "℃, TA:" << taTemp << "℃，更新图表";
        m_dualTempChart->updateIrData(QDateTime::currentDateTime(), toTemp, taTemp);
    } else {
        qWarning() << "[MainWindow] 温度数据无效 - TO:" << toTemp << "℃, TA:" << taTemp << "℃，不更新图表";
    }
}



CalibrationManager::InfraredData MainWindow::getIrAverage(const QString& comPort) {
    CalibrationManager::InfraredData result;
    QMutexLocker locker(&m_irCacheMutex);

    // 1. 查找对应COM口的表格行（原有逻辑不变）
    int targetRow = -1;
    for (int row = 0; row < m_tempTable->rowCount(); ++row) {
        QTableWidgetItem* portItem = m_tempTable->item(row, 0);
        if (portItem && portItem->text() == comPort) {
            targetRow = row;
            break;
        }
    }
    if (targetRow == -1) {
        qWarning() << "[getIrAverage] 未找到COM口" << comPort << "对应的表格行";
        result.type = "未知设备";
        return result;
    }

    // 2. 获取设备类型（原有逻辑不变）
    QTableWidgetItem* typeItem = m_tempTable->item(targetRow, 1);
    result.type = typeItem ? typeItem->text().trimmed() : "未知类型";
    bool isSingle = (result.type == "单头");
    qDebug() << "[getIrAverage] 处理COM口" << comPort << "，设备类型：" << result.type;

    // 3. 单头设备数据处理（补充LC1）
    if (isSingle) {
        // 提取TO1（2列）、TA1（3列）、LC1（4列）
        float to1 = NAN, ta1 = NAN, lc1 = NAN;
        if (auto toItem = m_tempTable->item(targetRow, 2)) to1 = toItem->text().toFloat();
        if (auto taItem = m_tempTable->item(targetRow, 3)) ta1 = taItem->text().toFloat();
        if (auto lcItem = m_tempTable->item(targetRow, 4)) lc1 = lcItem->text().toFloat(); // LC1列索引4

        // 缓存最后60秒数据（补充LC）
        auto& cache = m_irSingleCache[comPort];
        if (cache.size() >= 60) cache.removeFirst();
        cache.append(qMakePair(qMakePair(to1, ta1), lc1)); // 缓存结构：<<TO, TA>, LC>

        // 计算平均值（补充LC）
        float toSum = 0, taSum = 0, lcSum = 0;
        int validCount = 0;
        for (auto& data : cache) {
            auto& toTa = data.first;
            float lc = data.second;
            if (std::isfinite(toTa.first) && std::isfinite(toTa.second) && std::isfinite(lc)) {
                toSum += toTa.first;
                taSum += toTa.second;
                lcSum += lc;
                validCount++;
            }
        }

        if (validCount > 0) {
            result.toAvgs.append(toSum / validCount);
            result.taAvgs.append(taSum / validCount);
            result.lcAvgs.append(lcSum / validCount); // 存入LC1平均值
        } else {
            result.toAvgs.append(NAN);
            result.taAvgs.append(NAN);
            result.lcAvgs.append(NAN);
        }
    }
    // 4. 多头设备数据处理（补充LC1-LC3）
    else {
        // 提取TO1(2)/TO2(5)/TO3(8)、TA1(3)/TA2(6)/TA3(9)、LC1(4)/LC2(7)/LC3(10)
        int toCols[] = {2, 5, 8};   // TO列索引
        int taCols[] = {3, 6, 9};   // TA列索引
        int lcCols[] = {4, 7, 10};  // LC列索引（新增）

        QVector<float> toList, taList, lcList;
        for (int i = 0; i < 3; ++i) {
            if (auto toItem = m_tempTable->item(targetRow, toCols[i])) toList.append(toItem->text().toFloat());
            else toList.append(NAN);
            if (auto taItem = m_tempTable->item(targetRow, taCols[i])) taList.append(taItem->text().toFloat());
            else taList.append(NAN);
            if (auto lcItem = m_tempTable->item(targetRow, lcCols[i])) lcList.append(lcItem->text().toFloat()); // 提取LC
            else lcList.append(NAN);
        }

        // 缓存最后60秒数据（补充LC）
        auto& cache = m_irMultiCache[comPort];
        if (cache.size() >= 60) cache.removeFirst();
        cache.append(qMakePair(qMakePair(toList, taList), lcList)); // 缓存结构：<<TOs, TAs>, LCs>

        // 计算平均值（补充LC）
        QVector<float> toSums(3, 0), taSums(3, 0), lcSums(3, 0);
        QVector<int> validCounts(3, 0);
        for (auto& data : cache) {
            auto& toTas = data.first;
            auto& lcs = data.second;
            for (int i = 0; i < 3; ++i) {
                if (std::isfinite(toTas.first[i]) && std::isfinite(toTas.second[i]) && std::isfinite(lcs[i])) {
                    toSums[i] += toTas.first[i];
                    taSums[i] += toTas.second[i];
                    lcSums[i] += lcs[i]; // 累加LC值
                    validCounts[i]++;
                }
            }
        }

        // 填充平均值（补充LC）
        for (int i = 0; i < 3; ++i) {
            result.toAvgs.append(validCounts[i] > 0 ? toSums[i]/validCounts[i] : NAN);
            result.taAvgs.append(validCounts[i] > 0 ? taSums[i]/validCounts[i] : NAN);
            result.lcAvgs.append(validCounts[i] > 0 ? lcSums[i]/validCounts[i] : NAN); // 存入LC平均值
        }
    }

    return result;
}


