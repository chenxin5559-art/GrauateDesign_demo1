#include "calibrationmanager.h"
#include <QMessageBox>
#include <xlsxdocument.h>
#include <QDebug>
#include <numeric>
#include <algorithm>

CalibrationManager::CalibrationManager(BlackbodyController *blackbodyController, HumidityController *humidityController, QObject *parent)
    : QObject(parent), m_blackbodyController(blackbodyController), m_humidityController(humidityController)
{
    m_sensorStabilizeTimer.setSingleShot(true);
    m_waitNextMinuteTimer.setSingleShot(true);
    m_samplingTimer.setInterval(1000);
    m_countdownTimer.setInterval(1000);

    // 【新增】初始化超时定时器
    m_servoTimeoutTimer.setSingleShot(true);

    connect(&m_countdownTimer, &QTimer::timeout, this, &CalibrationManager::onCountdownTimerTimeout);
    connect(&m_waitNextMinuteTimer, &QTimer::timeout, this, &CalibrationManager::onWaitNextMinuteTimeout);
    connect(&m_sensorStabilizeTimer, &QTimer::timeout, this, &CalibrationManager::onSensorStabilizeTimeout);
    connect(&m_samplingTimer, &QTimer::timeout, this, &CalibrationManager::onSamplingTimerTimeout);

    // 【新增】连接超时信号
    connect(&m_servoTimeoutTimer, &QTimer::timeout, this, &CalibrationManager::onServoTimeout);
}

CalibrationManager::~CalibrationManager() {}

void CalibrationManager::setServoController(ServoMotorController *servo) {
    m_servo = servo;
    connect(m_servo, &ServoMotorController::positionReached, this, &CalibrationManager::onServoInPosition);
}

void CalibrationManager::setMeasurementQueue(const QVector<SensorTask>& queue) {
    m_taskQueue = queue;
    std::sort(m_taskQueue.begin(), m_taskQueue.end(), [](const SensorTask& a, const SensorTask& b){
        return a.position < b.position;
    });
}

// 【修改】startCalibration
void CalibrationManager::startCalibration(const QVector<float> &modelingPoints, const QVector<float> &verifyPoints, const QVector<float> &humidityPoints, const QString &envType)
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
    m_allTempPoints.clear();
    m_environmentType = envType;

    for (float t : modelingPoints) {
        m_allTempPoints.append({t, "建模"});
    }
    for (float t : verifyPoints) {
        m_allTempPoints.append({t, "验证"});
    }

    m_humidityTempPoints = humidityPoints;

    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    m_currentReportFileName = QString("measurement_record_%1.xlsx").arg(timestamp);
    setCurrentOperation(QString("初始化完成 (%1)，总计 %2 个温度点").arg(m_environmentType).arg(m_allTempPoints.size()));

    emit stateChanged(Running);

    // =========================================================
    // 【关键修复】使用 resetZeroPoint() 替代 moveToZero()
    // 强制将当前物理位置和软件计数器同时清零，确保坐标系同步
    // =========================================================
    m_servo->resetZeroPoint();

    m_blackbodyController->setMasterControl(true);
    m_humidityController->setMasterControl(true);

    emit calibrationProgress(0);
    calibrateNextPoint(0);
}

void CalibrationManager::calibrateNextPoint(int index) {
    m_currentTempPointIndex = index;
    m_currentIndex = index;

    if (index >= m_allTempPoints.size()) {
        setCurrentOperation("所有温度点标校完成，生成最终报告");
        generateCalibrationReport(true);
        return;
    }

    float bbTemp = m_allTempPoints[index].temp;
    QString type = m_allTempPoints[index].type;
    float humTemp = (index < m_humidityTempPoints.size()) ? m_humidityTempPoints[index] : 25.0f;

    setCurrentOperation(QString("设置第 %1 个点 (%2)：黑体炉 %3℃，恒温箱 %4℃")
                            .arg(index + 1).arg(type).arg(bbTemp).arg(humTemp));

    m_blackbodyController->setTargetTemperature(bbTemp);
    m_blackbodyController->setDeviceState(true);
    m_humidityController->setTargetTemperature(humTemp);
    m_humidityController->setDeviceState(true);

    m_servo->moveToZero(); // 这里调用 moveToZero 是安全的，因为已经在开始时reset过
    checkStability(index);
}

void CalibrationManager::checkStability(int index) {
    if (m_canceling || m_paused) return;

    float targetTemp = m_allTempPoints[index].temp;
    m_stabilitySamples.clear();
    m_sampleCount = 0;
    int windowSize = 150;
    int interval = 2;

    setCurrentOperation(QString("等待环境稳定 (目标: %1℃)...").arg(targetTemp));
    m_pausedStage = StabilityCheck;

    auto checkFunc = [this, targetTemp, windowSize]() {
        if (m_currentState != Running) return;
        float currentBB = m_blackbodyController->getCurrentTemperature();
        m_stabilitySamples.append(currentBB);
        if (m_stabilitySamples.size() > windowSize) m_stabilitySamples.removeFirst();
        m_sampleCount++;
        setCurrentOperation(QString("稳定性采样 #%1: %2℃").arg(m_sampleCount).arg(currentBB));
        if (m_stabilitySamples.size() == windowSize) {
            auto minmax = std::minmax_element(m_stabilitySamples.begin(), m_stabilitySamples.end());
            float fluctuation = *minmax.second - *minmax.first;
            float deviation = qAbs(currentBB - targetTemp);
            if (deviation < 1.0 && fluctuation < 0.1) {
                m_stabilityTimer.stop();
                setCurrentOperation(QString("环境已稳定 (波动%1℃)，打开标定窗口...").arg(fluctuation, 0, 'f', 3));
                m_humidityController->toggleCalibrationWindow(true);
                startMeasurement(m_currentTempPointIndex);
            } else {
                setCurrentOperation(QString("等待稳定: 当前%1℃, 偏差%2, 波动%3").arg(currentBB).arg(deviation, 0, 'f', 2).arg(fluctuation, 0, 'f', 3));
            }
        }
    };
    m_stabilityTimer.disconnect();
    connect(&m_stabilityTimer, &QTimer::timeout, this, checkFunc);
    m_stabilityTimer.start(interval * 1000);
}

void CalibrationManager::startMeasurement(int index) {
    if (m_canceling || m_paused) return;
    QDateTime nextMinute = QDateTime::currentDateTime().addSecs(60);
    nextMinute.setTime(QTime(nextMinute.time().hour(), nextMinute.time().minute(), 0));
    int waitToNextMinute = QDateTime::currentDateTime().secsTo(nextMinute);
    if (waitToNextMinute > 0) {
        setCurrentOperation(QString("等待到下一分钟开始测量（%1秒后）").arg(waitToNextMinute));
        m_pausedStage = WaitingForNextMinute;
        m_currentWaitIndex = index;
        m_currentWaitStartTime = QDateTime::currentDateTime();
        m_totalWaitSeconds = waitToNextMinute;
        m_currentCountdownStage = QString("等待到下一分钟开始第%1个温度点测量").arg(index + 1);
        m_countdownTimer.start();
        m_waitNextMinuteTimer.start(waitToNextMinute * 1000);
    } else {
        startBatchSequence(index);
    }
}

void CalibrationManager::onWaitNextMinuteTimeout() {
    if (m_paused || m_canceling || m_currentState != Running) return;
    m_pausedStage = None;
    m_countdownTimer.stop();
    startBatchSequence(m_currentTempPointIndex);
}

void CalibrationManager::startBatchSequence(int index) {
    m_currentBatchData.blackbodyTarget = m_allTempPoints[index].temp;
    m_currentBatchData.blackbodyReal = 0.0f;
    m_currentBatchData.measureTime = QDateTime::currentDateTime();
    m_currentBatchData.pointType = m_allTempPoints[index].type;
    setCurrentOperation(QString("%1点(%2℃)准备就绪，开始执行多通道测量序列...").arg(m_currentBatchData.pointType).arg(m_currentBatchData.blackbodyTarget));
    startSensorSequence();
}

void CalibrationManager::startSensorSequence() {
    if (m_taskQueue.isEmpty()) return;
    setCurrentOperation("开始执行多通道测量序列");
    m_currentTaskIndex = 0;
    processCurrentTask();
}

void CalibrationManager::processCurrentTask() {
    if (m_canceling) return;
    if (m_currentTaskIndex >= m_taskQueue.size()) {
        finishSequence();
        return;
    }
    SensorTask task = m_taskQueue[m_currentTaskIndex];
    double targetAngle = (task.position - 1) * DEGREES_PER_SLOT;
    setCurrentOperation(QString("电机移动至位置 %1 (COM: %2)...").arg(task.position).arg(task.comPort));
    m_pausedStage = ServoMoving;

    // 【新增】启动超时保护 (20秒)
    // 防止电机实际动了但未收到信号导致死锁
    m_servoTimeoutTimer.start(20000);

    m_servo->moveToAbsolute(targetAngle);
}

void CalibrationManager::onSamplingTimerTimeout() {
    if (m_currentState == Running && m_pausedStage == SensorStabilizing) {
        float currentTemp = m_blackbodyController->getCurrentTemperature();
        m_bbRealtimeSamples.append(currentTemp);
        if (m_bbRealtimeSamples.size() > 60) {
            m_bbRealtimeSamples.removeFirst();
        }
    }
}

void CalibrationManager::onServoInPosition() {
    if (m_pausedStage != ServoMoving) return;

    // 【新增】正常到位，停止超时计时
    m_servoTimeoutTimer.stop();

    SensorTask task = m_taskQueue[m_currentTaskIndex];
    int waitSeconds = 5 * 60;
    m_pausedStage = SensorStabilizing;
    m_waitStartTime = QDateTime::currentDateTime();
    m_waitTotalSeconds = waitSeconds;
    m_waitDescription = QString("位置 %1 (%2) 测量中 - 等待5分钟").arg(task.position).arg(task.comPort);
    m_sensorStabilizeTimer.start(waitSeconds * 1000);
    m_countdownTimer.start(1000);
    m_bbRealtimeSamples.clear();
    m_samplingTimer.start();
    emit irMeasurementStarted(task.comPort);
}

// 【新增】电机移动超时处理
void CalibrationManager::onServoTimeout() {
    if (m_pausedStage == ServoMoving) {
        qWarning() << "电机移动超时（未收到到位信号），强制进入下一阶段";
        setCurrentOperation("警告：电机信号超时，强制开始测量...");
        // 强制进入下一阶段
        onServoInPosition();
    }
}

void CalibrationManager::onSensorStabilizeTimeout() {
    m_countdownTimer.stop();
    m_samplingTimer.stop();
    SensorTask task = m_taskQueue[m_currentTaskIndex];
    float bbAvg = 0.0f;
    if (!m_bbRealtimeSamples.isEmpty()) {
        float sum = std::accumulate(m_bbRealtimeSamples.begin(), m_bbRealtimeSamples.end(), 0.0f);
        bbAvg = sum / m_bbRealtimeSamples.size();
    } else {
        bbAvg = m_blackbodyController->getCurrentTemperature();
    }
    m_currentBatchData.blackbodyReal = bbAvg;
    setCurrentOperation(QString("位置 %1 测量完成，黑体均值: %2℃，记录数据...").arg(task.position).arg(bbAvg, 0, 'f', 3));
    emit requestIrAverage(task.comPort, this);
}

void CalibrationManager::onIrAverageReceived(const QString& comPort, const CalibrationManager::InfraredData& irData) {
    if (m_currentTaskIndex >= m_taskQueue.size()) return;

    SensorTask currentTask = m_taskQueue[m_currentTaskIndex];
    if (currentTask.comPort != comPort) return;

    emit irMeasurementStopped();

    CalibrationRecord record;
    record.physicalPosition = currentTask.position;
    record.comPort = comPort;
    record.measureTime = QDateTime::currentDateTime();
    record.blackbodyTarget = m_currentBatchData.blackbodyTarget;
    record.blackbodyReal = m_currentBatchData.blackbodyReal;
    record.pointType = m_currentBatchData.pointType;
    record.environmentType = m_environmentType;
    record.irData = irData;

    m_calibrationData.append(record);

    setCurrentOperation(QString("位置 %1 数据已保存 (%2)").arg(currentTask.position).arg(record.pointType));

    m_currentTaskIndex++;
    processCurrentTask();
}

void CalibrationManager::finishSequence() {
    setCurrentOperation("本温度点所有通道测量完毕，正在保存中间数据...");

    generateCalibrationReport(false);

    m_humidityController->toggleCalibrationWindow(false);
    m_servo->moveToZero();
    m_pausedStage = None;
    m_samplingTimer.stop();
    m_servoTimeoutTimer.stop(); // 确保停止

    QTimer::singleShot(5000, this, [this](){
        int nextIndex = m_currentTempPointIndex + 1;
        int progress = nextIndex * 100 / m_allTempPoints.size();
        emit calibrationProgress(progress);
        calibrateNextPoint(nextIndex);
    });
}

void CalibrationManager::generateCalibrationReport(bool isFinal)
{
    if (m_calibrationData.isEmpty()) return;

    setCurrentOperation(QString("正在生成%1测量报告，共%2条数据...").arg(isFinal?"最终":"中间").arg(m_calibrationData.size()));

    QXlsx::Document report;

    report.write(1, 1, "温度类型");
    report.write(1, 2, "环境类型");
    report.write(1, 3, "测量温度点(℃)");
    report.write(1, 4, "测量时间");
    report.write(1, 5, "黑体炉平均温度(℃)");
    report.write(1, 6, "物理位置");
    report.write(1, 7, "COM口号");
    report.write(1, 8, "设备类型");
    report.write(1, 9, "TO1平均(℃)");
    report.write(1, 10, "TA1平均(℃)");
    report.write(1, 11, "LC1平均(℃)");
    report.write(1, 12, "TO2平均(℃)");
    report.write(1, 13, "TA2平均(℃)");
    report.write(1, 14, "LC2平均(℃)");
    report.write(1, 15, "TO3平均(℃)");
    report.write(1, 16, "TA3平均(℃)");
    report.write(1, 17, "LC3平均(℃)");

    int row = 2;
    for (const auto &record : m_calibrationData) {
        report.write(row, 1, record.pointType);
        report.write(row, 2, record.environmentType);
        report.write(row, 3, record.blackbodyTarget);
        report.write(row, 4, record.measureTime.toString("yyyy-MM-dd HH:mm:ss"));
        report.write(row, 5, record.blackbodyReal);
        report.write(row, 6, record.physicalPosition);
        report.write(row, 7, record.comPort);
        report.write(row, 8, record.irData.type);

        const auto& d = record.irData;
        for (int i = 0; i < 3; ++i) {
            int baseCol = 9 + i * 3;
            if (i < d.toAvgs.size() && qIsFinite(d.toAvgs[i])) report.write(row, baseCol, d.toAvgs[i]);
            if (i < d.taAvgs.size() && qIsFinite(d.taAvgs[i])) report.write(row, baseCol + 1, d.taAvgs[i]);
            if (i < d.lcAvgs.size() && qIsFinite(d.lcAvgs[i])) report.write(row, baseCol + 2, d.lcAvgs[i]);
        }
        row++;
    }

    if (report.saveAs(m_currentReportFileName)) {
        setCurrentOperation(QString("测量记录保存成功：%1").arg(m_currentReportFileName));
        if (isFinal) {
            m_currentState = Finished;
            emit stateChanged(Finished);
            emit calibrationFinished(m_calibrationData);
            QTimer::singleShot(2000, this, [this]() {
                m_currentState = Idle;
                emit stateChanged(Idle);
            });
        }
    } else {
        QString errorMsg = QString("测量记录保存失败，请检查文件是否被打开：%1").arg(m_currentReportFileName);
        setCurrentOperation(errorMsg);
        emit errorOccurred(errorMsg);
    }
}

void CalibrationManager::cancelCalibration() {
    m_currentState = Canceling;
    m_canceling = true;
    m_stabilityTimer.stop();
    m_sensorStabilizeTimer.stop();
    m_countdownTimer.stop();
    m_samplingTimer.stop();
    m_servoTimeoutTimer.stop(); // 停止超时计时

    if(m_servo) m_servo->stop();
    m_humidityController->toggleCalibrationWindow(false);
    m_blackbodyController->setDeviceState(false);
    m_humidityController->setDeviceState(false);

    QTimer::singleShot(1000, this, [this]() {
        m_currentState = Idle;
        m_canceling = false;
        emit stateChanged(Idle);
        setCurrentOperation("标校已取消");
    });
}

void CalibrationManager::pauseCalibration() {
    if (m_currentState == Running) {
        m_currentState = Paused;
        m_paused = true;
        m_stabilityTimer.stop();
        m_sensorStabilizeTimer.stop();
        m_countdownTimer.stop();
        m_samplingTimer.stop();
        m_servoTimeoutTimer.stop();
        emit stateChanged(Paused);
    }
}

void CalibrationManager::resumeCalibration() {
    if (m_currentState == Paused) {
        m_currentState = Running;
        m_paused = false;
        emit stateChanged(Running);

        if (m_pausedStage == StabilityCheck) m_stabilityTimer.start();
        else if (m_pausedStage == SensorStabilizing) {
            qint64 elapsed = m_waitStartTime.secsTo(QDateTime::currentDateTime());
            int remaining = m_waitTotalSeconds - elapsed;
            if (remaining > 0) m_sensorStabilizeTimer.start(remaining * 1000);
            else onSensorStabilizeTimeout();

            m_countdownTimer.start();
            m_samplingTimer.start();
        } else if (m_pausedStage == ServoMoving) {
            // 如果在电机移动时暂停，恢复时重启超时计时（简化处理）
            m_servoTimeoutTimer.start(20000);
        }
    }
}

void CalibrationManager::onCountdownTimerTimeout() {
    qint64 elapsed;
    int remaining = 0;

    if (m_pausedStage == SensorStabilizing) {
        elapsed = m_waitStartTime.secsTo(QDateTime::currentDateTime());
        remaining = m_waitTotalSeconds - elapsed;
        emit countdownUpdated(qMax(0, remaining), m_waitDescription);
    }
    else if (m_pausedStage == WaitingForNextMinute) {
        elapsed = m_currentWaitStartTime.secsTo(QDateTime::currentDateTime());
        remaining = m_totalWaitSeconds - elapsed;
        emit countdownUpdated(qMax(0, remaining), m_currentCountdownStage);
    }
}

void CalibrationManager::setCurrentOperation(const QString &operation) {
    currentOperation = operation;
    emit currentOperationChanged(operation);
    qDebug() << "操作状态：" << operation;
}

QString CalibrationManager::getCurrentOperation() const { return currentOperation; }
CalibrationManager::State CalibrationManager::getCurrentState() const { return m_currentState; }
