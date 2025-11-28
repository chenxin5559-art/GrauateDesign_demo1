#include "calibrationmanager.h"
#include <QMessageBox>
#include <xlsxdocument.h>
#include <QSettings>
#include <QDir>
#include <QMutex>
#include <QElapsedTimer>
#include <QDebug>
#include <numeric>

CalibrationManager::CalibrationManager(BlackbodyController *blackbodyController, HumidityController *humidityController, QObject *parent)
    : QObject(parent), m_blackbodyController(blackbodyController), m_humidityController(humidityController)
{
    m_fiveMinuteTimer.setSingleShot(true);
    m_sixMinuteTimer.setSingleShot(true);

    m_countdownTimer.setInterval(1000);
    connect(&m_countdownTimer, &QTimer::timeout, this, &CalibrationManager::onCountdownTimerTimeout);

    m_waitNextMinuteTimer.setSingleShot(true);
    connect(&m_waitNextMinuteTimer, &QTimer::timeout, this, &CalibrationManager::onWaitNextMinuteTimeout);
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
    qDebug() << "标定管理器已加载任务队列，设备数量：" << m_taskQueue.size();
}

void CalibrationManager::startCalibration(const QVector<float> &blackbodyTempPoints, const QVector<float> &humidityTempPoints)
{
    if (!m_servo || !m_servo->isConnected()) {
        emit errorOccurred("伺服电机未连接，无法开始标校！");
        return;
    }
    if (m_taskQueue.isEmpty()) {
        emit errorOccurred("未配置测温仪任务队列！请检查配置文件中的 COM 口映射。");
        return;
    }

    emit currentOperationChanged("正在复位电机零点，请稍候...");
    m_servo->resetZeroPoint();

    m_currentState = Running;
    m_paused = false;
    m_canceling = false;
    m_currentIndex = 0;
    m_calibrationData.clear();

    emit stateChanged(Running);
    m_blackbodyTempPoints = blackbodyTempPoints;
    m_humidityTempPoints = humidityTempPoints;

    m_blackbodyController->setMasterControl(true);
    m_humidityController->setMasterControl(true);

    m_servo->moveToZero();

    emit calibrationProgress(0);
    calibrateNextPoint(0);
}

void CalibrationManager::calibrateNextPoint(int index)
{
    m_currentIndex = index;

    if (m_canceling || m_paused) return;

    if (index >= m_blackbodyTempPoints.size()) {
        setCurrentOperation("所有温度点标校完成，生成报告");
        generateCalibrationReport();
        return;
    }

    float blackbodyTemp = m_blackbodyTempPoints[index];
    float humidityTemp = m_humidityTempPoints[index];

    setCurrentOperation(QString("设置第%1个点 - 黑体炉：%2℃").arg(index + 1).arg(blackbodyTemp));

    m_humidityController->setTargetTemperature(humidityTemp);
    m_humidityController->setDeviceState(true);
    m_blackbodyController->setTargetTemperature(blackbodyTemp);
    m_blackbodyController->setDeviceState(true);

    if (m_servo) {
        m_servo->moveToZero();
    }

    checkStability(index);
}

void CalibrationManager::checkStability(int index)
{
    if (m_canceling) {
        setCurrentOperation("标校已取消，停止稳定性检查");
        return;
    }

    if (m_paused) {
        setCurrentOperation("标校处于暂停状态，稳定性检查暂停");
        return;
    }

    float blackbodyTemp = m_blackbodyTempPoints[index];
    int windowSize = 150;
    int sampleInterval = 2;

    m_samples.clear();
    m_humiditySamples.clear();
    m_sampleCount = 0;

    setCurrentOperation(QString("开始温度稳定性检查 - 窗口大小：%1次采样，采样间隔：%2秒")
                            .arg(windowSize).arg(sampleInterval));

    auto sampler = [this, index, blackbodyTemp, windowSize]() {
        if (m_currentState == Running) {
            m_pausedStage = StabilityCheck;
        }

        float currentBlackbodyTemp = m_blackbodyController->getCurrentTemperature();
        float currentHumidityTemp = m_humidityController->getCurrentTemperature();
        float currentHumidity = m_humidityController->getCurrentHumidity();

        if (!qIsFinite(currentBlackbodyTemp)) {
            QString error = QString("黑体炉测量值无效：%1℃").arg(currentBlackbodyTemp);
            setCurrentOperation(error);
            emit errorOccurred(error);
            return;
        }

        m_samples.append(qMakePair(currentBlackbodyTemp, currentHumidityTemp));
        m_humiditySamples.append(currentHumidity);

        if (m_samples.size() > windowSize) {
            m_samples.removeFirst();
            m_humiditySamples.removeFirst();
        }

        m_sampleCount++;
        setCurrentOperation(QString("稳定性检查采样 #%1 - 黑体炉：%2℃").arg(m_sampleCount).arg(currentBlackbodyTemp));

        if (m_samples.size() == windowSize) {
            float minBlackbody = m_samples[0].first;
            float maxBlackbody = m_samples[0].first;
            bool blackbodyInRange = true;

            for (int i = 0; i < m_samples.size(); i++) {
                if (qAbs(m_samples[i].first - blackbodyTemp) > 1.0) {
                    blackbodyInRange = false;
                }
                if (m_samples[i].first < minBlackbody) minBlackbody = m_samples[i].first;
                if (m_samples[i].first > maxBlackbody) maxBlackbody = m_samples[i].first;
            }

            float blackbodyFluctuation = maxBlackbody - minBlackbody;
            bool blackbodyStable = blackbodyInRange && (blackbodyFluctuation < 0.1);

            if (blackbodyStable) {
                setCurrentOperation(QString("黑体炉温度已稳定 - 波动：%1℃").arg(blackbodyFluctuation));
                m_timer.stop();
                m_samples.clear();
                m_humiditySamples.clear();
                m_sampleCount = 0;
                startMeasurement(index);
            } else {
                QString reason = QString("黑体炉温度波动%1℃（目标范围±1℃，波动<0.1℃）").arg(blackbodyFluctuation);
                setCurrentOperation(QString("未达到稳定条件：%1，继续采样").arg(reason));
            }
        }
    };

    m_timer.disconnect();
    m_timer.setInterval(sampleInterval * 1000);
    connect(&m_timer, &QTimer::timeout, this, sampler);
    m_pausedStage = StabilityCheck;
    m_timer.start();
    sampler();
}

void CalibrationManager::startMeasurement(int index)
{
    if (m_canceling || m_paused) return;

    setCurrentOperation(QString("开始第%1个温度点的正式测量，打开标定窗口").arg(index + 1));
    m_humidityController->toggleCalibrationWindow(true);

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
        this->startWaitingSixMinutes(index);
    }
}

// 【新增】将原来的Lambda逻辑提取为函数：5分钟等待结束
void CalibrationManager::onFiveMinuteTimeout()
{
    QString stageDesc = QString("第%1个温度点最后1分钟采样").arg(m_currentIndex + 1);
    setCurrentOperation(stageDesc + " - 开始采样");

    m_pausedStage = MinuteSampling;
    m_currentCountdownStage = stageDesc;
    m_currentWaitStartTime = QDateTime::currentDateTime();
    m_totalWaitSeconds = 60;

    m_minuteSampleTimer.setInterval(1000);
    // 先断开旧连接防止重复
    disconnect(&m_minuteSampleTimer, nullptr, this, nullptr);
    connect(&m_minuteSampleTimer, &QTimer::timeout, this, &CalibrationManager::onMinuteSampleTimeout);
    m_minuteSampleTimer.start();
}

// 【新增】将原来的Lambda逻辑提取为函数：1秒采样
void CalibrationManager::onMinuteSampleTimeout()
{
    if (m_currentState == Running || m_currentState == Paused) {
        float currentTemp = m_blackbodyController->getCurrentTemperature();
        m_minuteSamples.append(currentTemp);
        setCurrentOperation(QString("第%1个温度点采样 #%2 - 温度：%3℃")
                                .arg(m_currentIndex + 1).arg(m_minuteSamples.size()).arg(currentTemp));
    }
}

// 【新增】将原来的Lambda逻辑提取为函数：6分钟等待结束
void CalibrationManager::onSixMinuteTimeout()
{
    m_minuteSampleTimer.stop();
    m_countdownTimer.stop();
    m_pausedStage = None;
    setCurrentOperation(QString("第%1个温度点 6分钟等待结束，准备执行多通道测量序列").arg(m_currentIndex + 1));

    recordMeasurement(m_currentIndex);
}

void CalibrationManager::startWaitingSixMinutes(int index)
{
    // 停止旧定时器
    m_fiveMinuteTimer.stop();
    m_sixMinuteTimer.stop();
    m_minuteSampleTimer.stop();
    m_countdownTimer.stop();

    // 断开旧连接
    disconnect(&m_fiveMinuteTimer, nullptr, this, nullptr);
    disconnect(&m_sixMinuteTimer, nullptr, this, nullptr);
    disconnect(&m_minuteSampleTimer, nullptr, this, nullptr);

    m_startSixMinuteTime = QDateTime::currentDateTime();
    m_currentWaitStartTime = m_startSixMinuteTime;
    m_totalWaitSeconds = 6 * 60;
    m_minuteSamples.clear();

    m_pausedStage = WaitingSixMinutes;
    m_currentCountdownStage = QString("第%1个温度点 6分钟稳定等待").arg(index + 1);

    setCurrentOperation(QString("%1 - 开始时间：%2").arg(m_currentCountdownStage).arg(m_startSixMinuteTime.toString("HH:mm:ss")));
    m_countdownTimer.start();

    // 连接新槽函数
    connect(&m_fiveMinuteTimer, &QTimer::timeout, this, &CalibrationManager::onFiveMinuteTimeout);
    connect(&m_sixMinuteTimer, &QTimer::timeout, this, &CalibrationManager::onSixMinuteTimeout);

    m_fiveMinuteTimer.start(5 * 60 * 1000);
    m_sixMinuteTimer.start(6 * 60 * 1000);
}

void CalibrationManager::recordMeasurement(int index)
{
    float bbAvg = 0.0f;
    if (!m_minuteSamples.isEmpty()) {
        bbAvg = std::accumulate(m_minuteSamples.begin(), m_minuteSamples.end(), 0.0f) / m_minuteSamples.size();
    } else {
        bbAvg = m_blackbodyController->getCurrentTemperature();
    }

    m_currentBatchData.blackbodyTarget = m_blackbodyTempPoints[index];
    m_currentBatchData.blackbodyAvg = bbAvg;
    m_currentBatchData.measureTime = QDateTime::currentDateTime();

    setCurrentOperation(QString("温度点%1基准数据已记录(BB均值:%2)，开始执行多通道测量序列...")
                            .arg(index + 1).arg(bbAvg));

    startSensorSequence();
}

void CalibrationManager::startSensorSequence() {
    if (!m_servo || !m_servo->isConnected()) {
        emit errorOccurred("伺服电机未连接，无法执行测量序列！");
        return;
    }

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

    setCurrentOperation(QString("电机移动至位置 %1 (%2度) 测量 %3")
                            .arg(task.position).arg(targetAngle).arg(task.comPort));

    m_pausedStage = ServoMoving;
    m_servo->moveToAbsolute(targetAngle);
}

void CalibrationManager::onServoInPosition() {
    if (m_pausedStage != ServoMoving) return;

    QTimer::singleShot(1500, this, [this](){
        if (m_currentTaskIndex < m_taskQueue.size()) {
            QString comPort = m_taskQueue[m_currentTaskIndex].comPort;

            setCurrentOperation(QString("位置 %1 到位，请求红外数据(%2)...").arg(m_taskQueue[m_currentTaskIndex].position).arg(comPort));

            emit irMeasurementStarted(comPort);
            emit requestIrAverage(comPort, this);
        }
    });
}

void CalibrationManager::onIrAverageReceived(const QString& comPort, const CalibrationManager::InfraredData& irData) {
    if (m_currentTaskIndex >= m_taskQueue.size()) return;

    SensorTask currentTask = m_taskQueue[m_currentTaskIndex];

    if (currentTask.comPort != comPort) {
        qWarning() << "端口不匹配，期望:" << currentTask.comPort << " 实际:" << comPort;
        return;
    }

    CalibrationRecord record;
    record.blackbodyTarget = m_currentBatchData.blackbodyTarget;
    record.blackbodyAvg = m_currentBatchData.blackbodyAvg;
    record.measureTime = m_currentBatchData.measureTime;
    record.comPort = comPort;
    record.physicalPosition = currentTask.position;
    record.irData = irData;

    m_calibrationData.append(record);

    setCurrentOperation(QString("位置 %1 (%2) 数据已记录").arg(currentTask.position).arg(comPort));
    emit irMeasurementStopped();

    m_currentTaskIndex++;
    processCurrentTask();
}

void CalibrationManager::finishSequence() {
    setCurrentOperation("本温度点所有通道测量完毕，电机复位中...");

    m_humidityController->toggleCalibrationWindow(false);
    m_pausedStage = None;

    m_servo->moveToZero();

    QTimer::singleShot(5000, this, [this](){
        int nextIndex = m_currentIndex + 1;
        int progress = nextIndex * 100 / m_blackbodyTempPoints.size();
        emit calibrationProgress(progress);

        calibrateNextPoint(nextIndex);
    });
}

void CalibrationManager::generateCalibrationReport()
{
    setCurrentOperation(QString("开始生成测量记录，共%1条数据").arg(m_calibrationData.size()));

    QXlsx::Document report;

    report.write(1, 1, "测量温度点(℃)");
    report.write(1, 2, "测量时间");
    report.write(1, 3, "黑体炉平均温度(℃)");
    report.write(1, 4, "物理位置");
    report.write(1, 5, "COM口号");
    report.write(1, 6, "设备类型");
    report.write(1, 7, "TO1平均(℃)");
    report.write(1, 8, "TA1平均(℃)");
    report.write(1, 9, "LC1平均(℃)");
    report.write(1, 10, "TO2平均(℃)");
    report.write(1, 11, "TA2平均(℃)");
    report.write(1, 12, "LC2平均(℃)");
    report.write(1, 13, "TO3平均(℃)");
    report.write(1, 14, "TA3平均(℃)");
    report.write(1, 15, "LC3平均(℃)");

    int row = 2;
    for (const auto &record : m_calibrationData) {
        report.write(row, 1, record.blackbodyTarget);
        report.write(row, 2, record.measureTime.toString("yyyy-MM-dd HH:mm"));
        report.write(row, 3, record.blackbodyAvg);
        report.write(row, 4, record.physicalPosition);
        report.write(row, 5, record.comPort);
        report.write(row, 6, record.irData.type);

        const auto& d = record.irData;
        for (int i = 0; i < 3; ++i) {
            int baseCol = 7 + i * 3;
            if (i < d.toAvgs.size() && qIsFinite(d.toAvgs[i])) report.write(row, baseCol, d.toAvgs[i]);
            if (i < d.taAvgs.size() && qIsFinite(d.taAvgs[i])) report.write(row, baseCol + 1, d.taAvgs[i]);
            if (i < d.lcAvgs.size() && qIsFinite(d.lcAvgs[i])) report.write(row, baseCol + 2, d.lcAvgs[i]);
        }
        row++;
    }

    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString fileName = QString("measurement_record_%1.xlsx").arg(timestamp);

    if (report.saveAs(fileName)) {
        setCurrentOperation(QString("测量记录保存成功：%1").arg(fileName));
        m_currentState = Finished;
        emit stateChanged(Finished);
        emit calibrationFinished(m_calibrationData);
    } else {
        setCurrentOperation("测量记录保存失败");
        emit errorOccurred("测量记录保存失败");
    }
}

void CalibrationManager::cancelCalibration()
{
    if (m_currentState == Running || m_currentState == Paused) {
        m_currentState = Canceling;
        m_canceling = true;

        m_timer.stop();
        m_minuteSampleTimer.stop();
        m_fiveMinuteTimer.stop();
        m_sixMinuteTimer.stop();
        m_countdownTimer.stop();
        m_waitNextMinuteTimer.stop();

        if(m_servo) m_servo->stop();

        m_humidityController->toggleCalibrationWindow(false);
        m_blackbodyController->setDeviceState(false);
        m_humidityController->setDeviceState(false);

        emit irMeasurementStopped();
        emit stateChanged(Canceling);

        m_currentIndex = -1;
        m_currentTaskIndex = 0;

        QTimer::singleShot(1000, this, [this]() {
            m_currentState = Idle;
            m_canceling = false;
            emit stateChanged(Idle);
            setCurrentOperation("标校已取消");
        });
    }
}

void CalibrationManager::pauseCalibration()
{
    if (m_currentState == Running) {
        m_currentState = Paused;
        m_paused = true;

        m_timer.stop();
        m_minuteSampleTimer.stop();
        m_fiveMinuteTimer.stop();
        m_sixMinuteTimer.stop();
        m_countdownTimer.stop();
        m_waitNextMinuteTimer.stop();

        setCurrentOperation("标校已暂停");
        emit stateChanged(Paused);
    }
}

void CalibrationManager::resumeCalibration()
{
    if (m_currentState != Paused) return;

    m_currentState = Running;
    m_paused = false;
    emit stateChanged(Running);
    setCurrentOperation("标校已恢复");

    switch (m_pausedStage) {
    case StabilityCheck:
        m_timer.start();
        break;
    case WaitingForNextMinute:
        m_countdownTimer.start();
        if (m_totalWaitSeconds > 0) {
            qint64 elapsed = m_currentWaitStartTime.secsTo(QDateTime::currentDateTime());
            qint64 remaining = m_totalWaitSeconds - elapsed;
            if (remaining > 0) m_waitNextMinuteTimer.start(remaining * 1000);
            else onWaitNextMinuteTimeout();
        }
        break;
    case WaitingSixMinutes:
    {
        qint64 elapsed = m_startSixMinuteTime.secsTo(QDateTime::currentDateTime());
        qint64 remainingSix = 6 * 60 - elapsed;

        if (remainingSix > 0) {
            m_sixMinuteTimer.start(remainingSix * 1000);
        } else {
            onSixMinuteTimeout();
            return;
        }

        if (elapsed < 5 * 60) {
            m_fiveMinuteTimer.start((5 * 60 - elapsed) * 1000);
        } else if (m_pausedStage != MinuteSampling) {
            onFiveMinuteTimeout(); // 手动触发5分钟逻辑
        }
        m_countdownTimer.start();
    }
    break;
    case MinuteSampling:
        m_minuteSampleTimer.start();
        {
            qint64 elapsed = m_currentWaitStartTime.secsTo(QDateTime::currentDateTime());
            qint64 remaining = 60 - elapsed;
            if (remaining > 0) m_sixMinuteTimer.start(remaining * 1000);
            else onSixMinuteTimeout(); // 手动触发6分钟结束逻辑
        }
        break;
    case ServoMoving:
        processCurrentTask();
        break;
    default:
        break;
    }
}

void CalibrationManager::onCountdownTimerTimeout()
{
    qint64 elapsed = m_currentWaitStartTime.secsTo(QDateTime::currentDateTime());
    qint64 remaining = m_totalWaitSeconds - elapsed;
    remaining = qMax(remaining, static_cast<qint64>(0));
    emit countdownUpdated(remaining, m_currentCountdownStage);
}

void CalibrationManager::onWaitNextMinuteTimeout()
{
    if (m_paused || m_canceling || m_currentState != Running) return;

    m_pausedStage = None;
    m_countdownTimer.stop();
    this->startWaitingSixMinutes(m_currentWaitIndex);
}

void CalibrationManager::setCurrentOperation(const QString &operation)
{
    currentOperation = operation;
    emit currentOperationChanged(operation);
    qDebug() << "操作状态：" << operation;
}

QString CalibrationManager::getCurrentOperation() const { return currentOperation; }
CalibrationManager::State CalibrationManager::getCurrentState() const { return m_currentState; }
