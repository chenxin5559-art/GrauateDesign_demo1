#include "calibrationmanager.h"
#include <QMessageBox>
#include <xlsxdocument.h>
#include <QDebug>
#include <numeric>
#include <algorithm> // for std::minmax_element

CalibrationManager::CalibrationManager(BlackbodyController *blackbodyController, HumidityController *humidityController, QObject *parent)
    : QObject(parent), m_blackbodyController(blackbodyController), m_humidityController(humidityController)
{
    // 初始化5分钟定时器
    m_sensorStabilizeTimer.setSingleShot(true);
    connect(&m_sensorStabilizeTimer, &QTimer::timeout, this, &CalibrationManager::onSensorStabilizeTimeout);

    // 倒计时定时器
    m_countdownTimer.setInterval(1000);
    connect(&m_countdownTimer, &QTimer::timeout, this, &CalibrationManager::onCountdownTimerTimeout);
}

CalibrationManager::~CalibrationManager() {}

void CalibrationManager::setServoController(ServoMotorController *servo) {
    m_servo = servo;
    connect(m_servo, &ServoMotorController::positionReached, this, &CalibrationManager::onServoInPosition);
}

void CalibrationManager::setMeasurementQueue(const QVector<SensorTask>& queue) {
    m_taskQueue = queue;
    // 按位置排序 (1 -> 10)
    std::sort(m_taskQueue.begin(), m_taskQueue.end(), [](const SensorTask& a, const SensorTask& b){
        return a.position < b.position;
    });
}

// 1. 开始标校
void CalibrationManager::startCalibration(const QVector<float> &blackbodyTempPoints, const QVector<float> &humidityTempPoints)
{
    if (!m_servo || !m_servo->isConnected()) {
        emit errorOccurred("伺服电机未连接，无法开始标校！");
        return;
    }
    if (m_taskQueue.isEmpty()) {
        emit errorOccurred("未配置测温仪任务队列！");
        return;
    }

    m_currentState = Running;
    m_paused = false;
    m_canceling = false;
    m_calibrationData.clear();
    m_blackbodyTempPoints = blackbodyTempPoints;
    m_humidityTempPoints = humidityTempPoints;

    emit stateChanged(Running);
    setCurrentOperation("初始化：正在复位电机零点...");

    // 强制电机复位归零
    m_servo->resetZeroPoint();
    m_servo->moveToZero();

    // 获取控制权
    m_blackbodyController->setMasterControl(true);
    m_humidityController->setMasterControl(true);

    emit calibrationProgress(0);

    // 开始第一个温度点
    calibrateNextPoint(0);
}

// 2. 切换温度点
void CalibrationManager::calibrateNextPoint(int index)
{
    m_currentTempPointIndex = index;

    if (index >= m_blackbodyTempPoints.size()) {
        setCurrentOperation("所有温度点标校完成，正在生成报告...");
        generateCalibrationReport();
        return;
    }

    float bbTemp = m_blackbodyTempPoints[index];
    float humTemp = m_humidityTempPoints[index];

    setCurrentOperation(QString("设置第 %1 个温度点：黑体炉 %2℃，恒温箱 %3℃").arg(index + 1).arg(bbTemp).arg(humTemp));

    // 设定温度
    m_blackbodyController->setTargetTemperature(bbTemp);
    m_blackbodyController->setDeviceState(true);

    m_humidityController->setTargetTemperature(humTemp);
    m_humidityController->setDeviceState(true);

    // 电机回零等待
    m_servo->moveToZero();

    // 进入稳定性检查
    checkStability(index);
}

// 3. 稳定性检查 (稳定后 -> 打开窗口 -> 开始测量循环)
void CalibrationManager::checkStability(int index)
{
    if (m_canceling || m_paused) return;

    float targetTemp = m_blackbodyTempPoints[index];
    m_stabilitySamples.clear();
    m_sampleCount = 0;

    // int windowSize = 15; // 采样窗口：15次 * 2秒 = 30秒
    int windowSize = 150; // 采样窗口：150次 * 2秒 = 5分钟
    int interval = 2;     // 2秒一次

    setCurrentOperation(QString("等待环境稳定 (目标: %1℃)...").arg(targetTemp));
    m_pausedStage = StabilityCheck;

    // 定义检查逻辑 lambda
    auto checkFunc = [this, targetTemp, windowSize]() {
        if (m_currentState != Running) return;

        float currentBB = m_blackbodyController->getCurrentTemperature();

        // 记录数据
        m_stabilitySamples.append(currentBB);
        if (m_stabilitySamples.size() > windowSize) m_stabilitySamples.removeFirst();
        m_sampleCount++;

        // 仅为了显示进度
        setCurrentOperation(QString("稳定性采样 #%1: %2℃").arg(m_sampleCount).arg(currentBB));

        // 判定逻辑
        if (m_stabilitySamples.size() == windowSize) {
            auto minmax = std::minmax_element(m_stabilitySamples.begin(), m_stabilitySamples.end());
            float fluctuation = *minmax.second - *minmax.first;
            float deviation = qAbs(currentBB - targetTemp);

            // 判定标准：偏差 < 1.0℃ 且 波动 < 0.1℃
            if (deviation < 1.0 && fluctuation < 0.1) {
                m_stabilityTimer.stop();
                setCurrentOperation(QString("环境已稳定 (波动%1℃)，打开标定窗口...").arg(fluctuation, 0, 'f', 3));

                // 【关键变更】稳定后直接打开窗口，并开始测量序列
                m_humidityController->toggleCalibrationWindow(true);

                // 延时2秒确保窗口指令发送，然后开始测量
                QTimer::singleShot(2000, this, &CalibrationManager::startSensorSequence);

            } else {
                // 继续等待
                setCurrentOperation(QString("等待稳定: 当前%1℃, 偏差%2, 波动%3 (目标<0.1)").arg(currentBB).arg(deviation, 0, 'f', 2).arg(fluctuation, 0, 'f', 3));
            }
        }
    };

    m_stabilityTimer.disconnect();
    connect(&m_stabilityTimer, &QTimer::timeout, this, checkFunc);
    m_stabilityTimer.start(interval * 1000);
}

// 4. 开始测量序列
void CalibrationManager::startSensorSequence() {
    if (m_taskQueue.isEmpty()) return;

    setCurrentOperation("开始执行多通道测量序列");
    m_currentTaskIndex = 0;
    processCurrentTask();
}

// 5. 处理当前传感器任务 (移动电机)
void CalibrationManager::processCurrentTask() {
    if (m_canceling) return;

    // 检查是否所有传感器测完
    if (m_currentTaskIndex >= m_taskQueue.size()) {
        finishSequence(); // 本轮结束
        return;
    }

    SensorTask task = m_taskQueue[m_currentTaskIndex];

    // 计算角度：(位置-1) * 40度
    double targetAngle = (task.position - 1) * DEGREES_PER_SLOT;

    setCurrentOperation(QString("电机移动至位置 %1 (COM: %2)...").arg(task.position).arg(task.comPort));

    m_pausedStage = ServoMoving;
    // 发送绝对定位指令（底层会转为相对指令）
    m_servo->moveToAbsolute(targetAngle);
}

// 6. 电机到位回调 (开始5分钟等待)
void CalibrationManager::onServoInPosition() {
    // 只有在自动流程的移动阶段才处理
    if (m_pausedStage != ServoMoving) return;

    SensorTask task = m_taskQueue[m_currentTaskIndex];

    // === 【修改点】等待5分钟 (300秒) ===
    //int waitSeconds = 5;
    int waitSeconds = 5 * 60;
    // int waitSeconds = 10; // 调试用：改短时间

    m_pausedStage = SensorStabilizing; // 切换状态为“等待中”

    m_waitStartTime = QDateTime::currentDateTime();
    m_waitTotalSeconds = waitSeconds;
    m_waitDescription = QString("位置 %1 (%2) 测量中 - 等待5分钟").arg(task.position).arg(task.comPort);

    // 启动5分钟定时器
    m_sensorStabilizeTimer.start(waitSeconds * 1000);
    m_countdownTimer.start(1000); // UI倒计时

    // 通知UI开始显示该串口的实时红外数据
    emit irMeasurementStarted(task.comPort);
}

// 7. 5分钟时间到 (触发记录)
void CalibrationManager::onSensorStabilizeTimeout() {
    m_countdownTimer.stop();
    SensorTask task = m_taskQueue[m_currentTaskIndex];

    setCurrentOperation(QString("位置 %1 测量时间到，正在记录数据...").arg(task.position));

    // 向UI请求过去一段时间的平均数据 (MainWindow会回调 onIrAverageReceived)
    emit requestIrAverage(task.comPort, this);
}

// 8. 接收数据并保存 (完成一个传感器)
void CalibrationManager::onIrAverageReceived(const QString& comPort, const CalibrationManager::InfraredData& irData) {
    if (m_currentTaskIndex >= m_taskQueue.size()) return;

    SensorTask currentTask = m_taskQueue[m_currentTaskIndex];
    if (currentTask.comPort != comPort) return; // 校验端口匹配

    // 停止UI的红外刷新
    emit irMeasurementStopped();

    // 构建记录
    CalibrationRecord record;
    record.physicalPosition = currentTask.position;
    record.comPort = comPort;
    record.measureTime = QDateTime::currentDateTime(); // 【关键】记录第5分钟的时间点
    record.blackbodyTarget = m_blackbodyTempPoints[m_currentTempPointIndex];

    // 【关键】记录此刻的黑体炉实际温度
    record.blackbodyReal = m_blackbodyController->getCurrentTemperature();

    record.irData = irData; // 保存红外数据

    // 存入总表
    m_calibrationData.append(record);

    setCurrentOperation(QString("位置 %1 数据已保存 (黑体: %2℃)，准备下一个...").arg(currentTask.position).arg(record.blackbodyReal));

    // 移动到下一个传感器
    m_currentTaskIndex++;
    processCurrentTask();
}

// 9. 本轮温度点结束收尾
void CalibrationManager::finishSequence() {
    setCurrentOperation("本温度点所有通道测量完毕，正在收尾...");

    // 1. 关闭标定窗口
    m_humidityController->toggleCalibrationWindow(false);

    // 2. 电机反转归零 (防止绕线)
    m_servo->moveToZero();

    // 3. 延时后进入下一个温度点
    m_pausedStage = None;

    // 等待5秒让电机复位，然后切换下一个大温度点
    QTimer::singleShot(5000, this, [this](){
        int nextIndex = m_currentTempPointIndex + 1;

        // 更新总进度条
        int progress = nextIndex * 100 / m_blackbodyTempPoints.size();
        emit calibrationProgress(progress);

        // 递归调用开始下一个点
        calibrateNextPoint(nextIndex);
    });
}

// UI倒计时刷新辅助
void CalibrationManager::onCountdownTimerTimeout() {
    if (m_pausedStage != SensorStabilizing) return;

    qint64 elapsed = m_waitStartTime.secsTo(QDateTime::currentDateTime());
    int remaining = m_waitTotalSeconds - elapsed;
    if (remaining < 0) remaining = 0;

    emit countdownUpdated(remaining, m_waitDescription);
}

// 生成报告
void CalibrationManager::generateCalibrationReport()
{
    setCurrentOperation(QString("正在生成Excel报告，共 %1 条数据").arg(m_calibrationData.size()));

    QXlsx::Document report;
    // 写入表头
    report.write(1, 1, "目标温度(℃)");
    report.write(1, 2, "实际黑体温度(℃)"); // 新增列
    report.write(1, 3, "测量时间");
    report.write(1, 4, "物理位置");
    report.write(1, 5, "COM口");
    report.write(1, 6, "TO1");
    report.write(1, 7, "TA1");
    report.write(1, 8, "LC1");

    int row = 2;
    for (const auto &rec : m_calibrationData) {
        report.write(row, 1, rec.blackbodyTarget);
        report.write(row, 2, rec.blackbodyReal); // 写入实际值
        report.write(row, 3, rec.measureTime.toString("yyyy-MM-dd HH:mm:ss"));
        report.write(row, 4, rec.physicalPosition);
        report.write(row, 5, rec.comPort);

        // 写入第一个探头的数据 (如果有更多探头需扩展)
        if (!rec.irData.toAvgs.isEmpty()) report.write(row, 6, rec.irData.toAvgs[0]);
        if (!rec.irData.taAvgs.isEmpty()) report.write(row, 7, rec.irData.taAvgs[0]);
        if (!rec.irData.lcAvgs.isEmpty()) report.write(row, 8, rec.irData.lcAvgs[0]);

        row++;
    }

    QString fileName = QString("Calibration_%1.xlsx").arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    if (report.saveAs(fileName)) {
        setCurrentOperation("报告生成成功: " + fileName);
        m_currentState = Finished;
        emit stateChanged(Finished);
        QMessageBox::information(nullptr, "完成", "标校流程结束，报告已生成！");
    } else {
        emit errorOccurred("报告保存失败！");
    }
}

// 暂停/恢复/取消功能的简单适配
void CalibrationManager::pauseCalibration() {
    if (m_currentState == Running) {
        m_currentState = Paused;
        m_paused = true;
        // 停止所有计时器
        m_stabilityTimer.stop();
        m_sensorStabilizeTimer.stop();
        m_countdownTimer.stop();
        emit stateChanged(Paused);
    }
}

void CalibrationManager::resumeCalibration() {
    if (m_currentState == Paused) {
        m_currentState = Running;
        m_paused = false;
        emit stateChanged(Running);

        // 根据暂停阶段恢复计时器
        if (m_pausedStage == StabilityCheck) m_stabilityTimer.start();
        else if (m_pausedStage == SensorStabilizing) {
            // 恢复剩余时间的计时
            qint64 elapsed = m_waitStartTime.secsTo(QDateTime::currentDateTime());
            int remaining = m_waitTotalSeconds - elapsed;
            if (remaining > 0) m_sensorStabilizeTimer.start(remaining * 1000);
            else onSensorStabilizeTimeout();
            m_countdownTimer.start();
        }
    }
}

void CalibrationManager::cancelCalibration() {
    m_currentState = Canceling;
    m_canceling = true;
    m_stabilityTimer.stop();
    m_sensorStabilizeTimer.stop();
    m_countdownTimer.stop();

    if(m_servo) m_servo->stop();
    m_humidityController->toggleCalibrationWindow(false);
    m_blackbodyController->setDeviceState(false);

    // 必须有这段代码才能恢复 Idle 状态！
    QTimer::singleShot(1000, this, [this]() {
        m_currentState = Idle;
        m_canceling = false;
        emit stateChanged(Idle); // <--- 这一句触发 MainWindow 更新 UI
        setCurrentOperation("标校已取消");
    });
}

void CalibrationManager::setCurrentOperation(const QString &operation)
{
    currentOperation = operation;
    // 发送信号更新UI
    emit currentOperationChanged(operation);
    // 打印调试日志
    qDebug() << "操作状态：" << operation;
}

QString CalibrationManager::getCurrentOperation() const { return currentOperation; }
CalibrationManager::State CalibrationManager::getCurrentState() const { return m_currentState; }
