#include "calibrationmanager.h"
#include <QMessageBox>
#include <xlsxdocument.h>
#include <QSettings>
#include <QDir>
#include <QMutex>
#include <QElapsedTimer>
#include <QDebug>

CalibrationManager::CalibrationManager(BlackbodyController *blackbodyController, HumidityController *humidityController, QObject *parent)
    : QObject(parent), m_blackbodyController(blackbodyController), m_humidityController(humidityController),
    sensorSwitchCount(0), m_fiveMinuteTimer(this), m_sixMinuteTimer(this), m_countdownTimer(this),
    m_waitNextMinuteTimer(this)
{
    // 读取配置文件中的COM口号列表（新格式解析）
    QSettings settings(QDir::currentPath() + "/config.ini", QSettings::IniFormat);
    QString comPortsStr = settings.value("devices/com_ports", "").toString();
    // 处理新格式：去除引号 -> 按逗号拆分 -> 提取每个元素中的COM口
    comPortsStr = comPortsStr.replace("\"", ""); // 移除配置文件中的引号
    QStringList portItems = comPortsStr.split(',', Qt::SkipEmptyParts); // 按逗号拆分
    m_comPorts.clear(); // 清空原有列表
    QHash<QString, QString> portMap; // 存储机位号与COM口的映射

    foreach (const QString &item, portItems) {
        QString trimmedItem = item.trimmed(); // 去除前后空格
        QStringList parts = trimmedItem.split('-'); // 按"-"拆分机位号和COM口
        if (parts.size() == 2) {
            // 格式正确：提取COM口部分
            QString comPort = parts[1].trimmed();
            m_comPorts.append(comPort);
            portMap.insert(parts[0].trimmed(), comPort); // 记录机位号->COM口映射
            setCurrentOperation(QString("解析COM口配置：机位号%1对应COM口%2").arg(parts[0].trimmed()).arg(comPort));
        } else {
            // 格式异常：直接使用原始值（兼容旧格式）
            m_comPorts.append(trimmedItem);
            setCurrentOperation(QString("COM口配置格式异常，使用原始值：%1").arg(trimmedItem));
        }
    }

    // 设置定时器为单次触发
    m_fiveMinuteTimer.setSingleShot(true);
    m_sixMinuteTimer.setSingleShot(true);

    // 配置倒计时定时器（每秒更新一次）
    m_countdownTimer.setInterval(1000);
    m_countdownTimer.setSingleShot(false);
    connect(&m_countdownTimer, &QTimer::timeout, this, &CalibrationManager::onCountdownTimerTimeout);

    // 配置“等待下一分钟”定时器为单次触发
    m_waitNextMinuteTimer.setSingleShot(true);
    // 连接超时信号到自定义槽函数
    connect(&m_waitNextMinuteTimer, &QTimer::timeout, this, &CalibrationManager::onWaitNextMinuteTimeout);
}

CalibrationManager::~CalibrationManager()
{
}

void CalibrationManager::startCalibration(const QVector<float> &blackbodyTempPoints, const QVector<float> &humidityTempPoints)
{
    m_currentState = Running;
    m_paused = false;
    m_canceling = false;
    m_currentIndex = 0;
    m_currentSensorIndex = 0;
    sensorSwitchCount = 0;
    m_calibrationData.clear();

    emit stateChanged(Running);
    setCurrentOperation("标校流程初始化完成，准备开始");

    m_blackbodyTempPoints = blackbodyTempPoints;
    m_humidityTempPoints = humidityTempPoints;  // 仅存储温度

    setCurrentOperation(QString("检测到标校参数：黑体炉温度点%1个，恒温箱温度点%2个")
                            .arg(m_blackbodyTempPoints.size()).arg(m_humidityTempPoints.size()));

    m_blackbodyController->setMasterControl(true);
    m_humidityController->setMasterControl(true);
    setCurrentOperation("已获取上位机对黑体炉和恒温箱的控制权限");

    emit calibrationProgress(0);
    setCurrentOperation("开始执行第一个标校温度点");
    calibrateNextPoint(0);
}

void CalibrationManager::calibrateNextPoint(int index)
{
    m_currentIndex = index;

    if (m_canceling) {
        setCurrentOperation("标校已取消，跳过当前温度点处理");
        return;
    }

    if (m_paused) {
        setCurrentOperation("标校处于暂停状态，等待用户恢复");
        return;
    }

    if (index >= m_blackbodyTempPoints.size()) {
        setCurrentOperation("所有温度点标校已完成，准备生成报告");
        generateCalibrationReport();
        return;
    }

    float blackbodyTemp = m_blackbodyTempPoints[index];
    float humidityTemp = m_humidityTempPoints[index];  // 仅使用温度

    setCurrentOperation(QString("开始处理第%1个温度点 - 黑体炉目标温度：%2℃，恒温箱目标温度：%3℃")
                            .arg(index + 1).arg(blackbodyTemp).arg(humidityTemp));

    // 仅设置恒温箱温度（移除湿度设置）
    m_humidityController->setTargetTemperature(humidityTemp);
    m_humidityController->setDeviceState(true);
    setCurrentOperation(QString("恒温箱已设置：温度%1℃，设备已启动")
                            .arg(humidityTemp));

    m_blackbodyController->setTargetTemperature(blackbodyTemp);
    m_blackbodyController->setDeviceState(true);
    setCurrentOperation(QString("黑体炉已设置：温度%1℃，设备已启动").arg(blackbodyTemp));

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
    float humidityTemp = m_humidityTempPoints[index];  // 仅作为参考，不参与判断

    int windowSize = 150;
    int sampleInterval = 2;

    m_samples.clear();
    m_humiditySamples.clear();  // 保留湿度采样（仅读取，不参与判断）
    m_sampleCount = 0;

    setCurrentOperation(QString("开始温度稳定性检查（仅校验黑体炉） - 窗口大小：%1次采样，采样间隔：%2秒")
                            .arg(windowSize).arg(sampleInterval));

    auto sampler = [this, index, blackbodyTemp, humidityTemp, sampleInterval, windowSize]() {
        if (m_currentState == Running) {
            m_pausedStage = StabilityCheck;
        }

        float currentBlackbodyTemp = m_blackbodyController->getCurrentTemperature();
        float currentHumidityTemp = m_humidityController->getCurrentTemperature();  // 仅记录，不参与判断
        float currentHumidity = m_humidityController->getCurrentHumidity();  // 仅监控

        if (!qIsFinite(currentBlackbodyTemp)) {  // 仅校验黑体炉温度有效性
            QString error = QString("黑体炉测量值无效：%1℃").arg(currentBlackbodyTemp);
            setCurrentOperation(error);
            emit errorOccurred(error);
            return;
        }

        m_samples.append(qMakePair(currentBlackbodyTemp, currentHumidityTemp));  // 仍记录，但不用于判断
        m_humiditySamples.append(currentHumidity);

        if (m_samples.size() > windowSize) {
            m_samples.removeFirst();
            m_humiditySamples.removeFirst();
        }

        m_sampleCount++;
        setCurrentOperation(QString("稳定性检查采样 #%1 - 黑体炉：%2℃，恒温箱（监控）：%3℃，湿度：%4%")
                                .arg(m_sampleCount).arg(currentBlackbodyTemp).arg(currentHumidityTemp).arg(currentHumidity));

        if (m_samples.size() == windowSize) {
            // 仅计算黑体炉的波动范围（忽略恒温箱）
            bool blackbodyInRange = true;
            float minBlackbody = m_samples[0].first;
            float maxBlackbody = m_samples[0].first;

            for (int i = 0; i < m_samples.size(); i++) {
                // 仅检查黑体炉是否在目标温度±1℃范围内
                if (qAbs(m_samples[i].first - blackbodyTemp) > 1.0) {
                    blackbodyInRange = false;
                }

                // 仅更新黑体炉的最大/最小值
                if (m_samples[i].first < minBlackbody) minBlackbody = m_samples[i].first;
                if (m_samples[i].first > maxBlackbody) maxBlackbody = m_samples[i].first;
            }

            // 仅计算黑体炉的波动
            float blackbodyFluctuation = maxBlackbody - minBlackbody;
            bool blackbodyStable = blackbodyInRange && (blackbodyFluctuation < 0.1);  // 仅判断黑体炉

            if (blackbodyStable) {
                setCurrentOperation(QString("黑体炉温度已稳定 - 波动：%1℃（忽略恒温箱状态）")
                                        .arg(blackbodyFluctuation));
                m_timer.stop();
                startMeasurement(index);
                m_samples.clear();
                m_humiditySamples.clear();
                m_sampleCount = 0;
            } else {
                // 仅提示黑体炉未达稳定条件
                QString reason = QString("黑体炉温度波动%1℃（目标范围±1℃，波动<0.1℃）")
                                     .arg(blackbodyFluctuation);
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
    // 检查是否取消
    if (m_canceling) {
        setCurrentOperation("标校已取消，停止测量流程");
        return;
    }

    // 检查是否暂停
    if (m_paused) {
        setCurrentOperation("标校处于暂停状态，测量流程暂停");
        return;
    }

    setCurrentOperation(QString("开始第%1个温度点的正式测量，打开标定窗口").arg(index + 1));
    // 打开标定窗口
    m_humidityController->toggleCalibrationWindow(true);

    // 计算下一分钟的时间点
    QDateTime nextMinute = QDateTime::currentDateTime().addSecs(60);
    nextMinute.setTime(QTime(nextMinute.time().hour(), nextMinute.time().minute(), 0));
    // 计算等待到下一分钟的时间间隔
    int waitToNextMinute = QDateTime::currentDateTime().secsTo(nextMinute);

    if (waitToNextMinute > 0) {
        setCurrentOperation(QString("等待到下一分钟开始测量（%1秒后）").arg(waitToNextMinute));

        // 关键修改：使用成员定时器替代singleShot
        m_pausedStage = WaitingForNextMinute;
        m_currentWaitIndex = index; // 记录当前温度点索引
        m_currentWaitStartTime = QDateTime::currentDateTime();
        m_totalWaitSeconds = waitToNextMinute;
        m_currentCountdownStage = QString("等待到下一分钟开始第%1个温度点测量").arg(index + 1);

        // 启动倒计时更新定时器（用于显示剩余时间）
        m_countdownTimer.start();
        // 启动成员定时器，设置等待时间
        m_waitNextMinuteTimer.start(waitToNextMinute * 1000);
    } else {
        this->startWaitingSixMinutes(index);
    }
}

void CalibrationManager::startWaitingSixMinutes(int index)
{
    // 停止所有可能存在的定时器
    m_fiveMinuteTimer.stop();
    m_sixMinuteTimer.stop();
    m_minuteSampleTimer.stop();
    m_countdownTimer.stop();

    // 断开所有信号连接
    disconnect(&m_fiveMinuteTimer, nullptr, this, nullptr);
    disconnect(&m_sixMinuteTimer, nullptr, this, nullptr);
    disconnect(&m_minuteSampleTimer, nullptr, this, nullptr);

    // 重置时间变量
    m_startSixMinuteTime = QDateTime::currentDateTime();
    m_currentWaitStartTime = m_startSixMinuteTime;
    m_totalWaitSeconds = 6 * 60; // 6分钟总时长

    // 重置采样数据
    m_minuteSamples.clear();

    // 启动6分钟等待时，标记阶段
    m_pausedStage = WaitingSixMinutes;
    m_currentCountdownStage = QString("第%1个温度点，测温仪%2（%3）6分钟等待").arg(index + 1)
                                  .arg(m_currentSensorIndex + 1).arg(m_comPorts.value(m_currentSensorIndex, "未知"));

    setCurrentOperation(QString("%1 - 开始时间：%2")
                            .arg(m_currentCountdownStage)
                            .arg(m_startSixMinuteTime.toString("HH:mm:ss")));

    // 启动倒计时定时器
    m_countdownTimer.start();

    // 获取当前测温仪的COM口
    QString currentComPort = m_comPorts.value(m_currentSensorIndex, "未知");
    // 添加调试信息：确认当前COM口
    qDebug() << "[CalibrationManager] 6分钟等待开始，当前红外测温仪COM口：" << currentComPort;
    // 发射测量开始信号（携带当前COM口）
    emit irMeasurementStarted(currentComPort);

    // 5分钟后启动最后1分钟采样
    connect(&m_fiveMinuteTimer, &QTimer::timeout, this, [this, index]() {
        QString stageDesc = QString("第%1个温度点，测温仪%2（%3）最后1分钟采样")
                                .arg(index + 1).arg(m_currentSensorIndex + 1)
                                .arg(m_comPorts.value(m_currentSensorIndex, "未知"));

        setCurrentOperation(stageDesc + " - 开始采样");
        m_pausedStage = MinuteSampling;

        // 更新倒计时阶段
        m_currentCountdownStage = stageDesc;
        m_currentWaitStartTime = QDateTime::currentDateTime();
        m_totalWaitSeconds = 60; // 最后1分钟

        // 设置采样定时器
        m_minuteSampleTimer.setInterval(1000);
        connect(&m_minuteSampleTimer, &QTimer::timeout, this, [this, index]() {
            if (m_currentState == Running || m_currentState == Paused) {
                float currentTemp = m_blackbodyController->getCurrentTemperature();
                m_minuteSamples.append(currentTemp);
                setCurrentOperation(QString("第%1个温度点，测温仪%2（%3）采样 #%4 - 温度：%5℃")
                                        .arg(index + 1).arg(m_currentSensorIndex + 1)
                                        .arg(m_comPorts.value(m_currentSensorIndex, "未知"))
                                        .arg(m_minuteSamples.size()).arg(currentTemp));
            }
        });
        m_minuteSampleTimer.start();
    });

    // 6分钟后记录数据
    connect(&m_sixMinuteTimer, &QTimer::timeout, this, [this, index]() {
        m_minuteSampleTimer.stop();
        m_countdownTimer.stop();
        m_pausedStage = None; // 采样阶段结束，重置标记
        setCurrentOperation(QString("第%1个温度点，测温仪%2（%3）6分钟等待结束，准备记录数据")
                                .arg(index + 1).arg(m_currentSensorIndex + 1)
                                .arg(m_comPorts.value(m_currentSensorIndex, "未知")));
        recordMeasurement(index);
    });

    // 启动定时器
    m_fiveMinuteTimer.start(5 * 60 * 1000);  // 5分钟
    m_sixMinuteTimer.start(6 * 60 * 1000);   // 6分钟
}

void CalibrationManager::recordMeasurement(int index)
{
    float blackbodyTemp = m_blackbodyTempPoints[index];
    QDateTime measureTime = QDateTime::currentDateTime();
    measureTime.setTime(QTime(measureTime.time().hour(), measureTime.time().minute(), 0));

    // 1. 计算黑体炉平均温度（原有逻辑）
    float bbAvg = 0.0f;
    if (!m_minuteSamples.isEmpty()) {
        bbAvg = std::accumulate(m_minuteSamples.begin(), m_minuteSamples.end(), 0.0f)
        / m_minuteSamples.size();
    }

    // 2. 获取当前COM口
    QString comPort = m_comPorts.value(m_currentSensorIndex, "未知");
    if (comPort == "未知") {
        setCurrentOperation("未获取到有效COM口，跳过红外数据记录");
        return;
    }

    // 3. 发送信号请求红外平均数据
    setCurrentOperation(QString("请求COM口%1的红外平均数据").arg(comPort));
    emit requestIrAverage(comPort, this); // 触发MainWindow计算平均值

    // 4. 构造临时记录（等待红外数据回调）
    CalibrationRecord tempRecord;
    tempRecord.blackbodyTarget = blackbodyTemp;
    tempRecord.measureTime = measureTime;
    tempRecord.blackbodyAvg = bbAvg;
    tempRecord.comPort = comPort;
    m_tempRecords.append(tempRecord); // 临时存储，回调时补全

    // 7. 设备切换逻辑（保持原有，但调整日志）
    int deviceCount = m_comPorts.size() > 0 ? m_comPorts.size() : 1;
    setCurrentOperation(QString("当前测温仪数量：%1台，当前索引：%2").arg(deviceCount).arg(m_currentSensorIndex));

    // 更换下一个测温仪
    m_humidityController->changeSensor(1);
    sensorSwitchCount++;
    setCurrentOperation(QString("已切换到下一个测温仪，累计切换次数：%1").arg(sensorSwitchCount));

    // 检查是否所有测温仪都已测量
    if (m_currentSensorIndex < deviceCount - 1) {
        m_currentSensorIndex++;
        m_minuteSamples.clear();
        m_pausedStage = None;
        setCurrentOperation(QString("准备第%1个温度点，测温仪%2（%3）的测量")
                                .arg(index + 1).arg(m_currentSensorIndex + 1)
                                .arg(m_comPorts.value(m_currentSensorIndex, "未知")));
        this->startWaitingSixMinutes(index);
    } else {
        m_currentSensorIndex = 0;
        m_pausedStage = None;
        setCurrentOperation(QString("第%1个温度点所有%2台测温仪测量完成，等待1分钟后处理下一个温度点")
                                .arg(index + 1).arg(deviceCount));

        // 启动1分钟等待倒计时
        m_currentWaitStartTime = QDateTime::currentDateTime();
        m_totalWaitSeconds = 60;
        m_currentCountdownStage = QString("第%1个温度点完成后等待").arg(index + 1);
        m_countdownTimer.start();

        QTimer::singleShot(1 * 60 * 1000, this, [this, index]() {
            m_countdownTimer.stop();
            checkAllSensorsMeasured(index);
        });

        emit irMeasurementStopped();
    }
}


void CalibrationManager::checkAllSensorsMeasured(int index)
{
    // 检查是否取消
    if (m_canceling) {
        setCurrentOperation("标校已取消，停止检查流程");
        return;
    }

    // 检查是否暂停
    if (m_paused) {
        setCurrentOperation("标校处于暂停状态，检查流程暂停");
        return;
    }

    setCurrentOperation(QString("确认第%1个温度点所有测温仪已完成测量，关闭标定窗口").arg(index + 1));

    // 关闭测量窗口
    m_humidityController->toggleCalibrationWindow(false);

    // 开始反向切换
    reverseSwitch(index);
}

void CalibrationManager::generateCalibrationReport()
{
    setCurrentOperation(QString("开始生成测量记录，共%1条数据").arg(m_calibrationData.size()));

    // 生成报告文件
    QXlsx::Document report;

    // 1. 更新表头（新增LC列）
    report.write(1, 1, "测量温度点(℃)");       // 1
    report.write(1, 2, "环境温度(℃)");         // 2
    report.write(1, 3, "测量时间");             // 3
    report.write(1, 4, "黑体炉平均温度(℃)");   // 4
    report.write(1, 5, "COM口号");              // 5
    report.write(1, 6, "设备类型");             // 6
    report.write(1, 7, "TO1平均(℃)");           // 7
    report.write(1, 8, "TA1平均(℃)");          // 8
    report.write(1, 9, "LC1平均(℃)");          // 9（新增）
    report.write(1, 10, "TO2平均(℃)");          // 10
    report.write(1, 11, "TA2平均(℃)");         // 11
    report.write(1, 12, "LC2平均(℃)");         // 12（新增）
    report.write(1, 13, "TO3平均(℃)");         // 13
    report.write(1, 14, "TA3平均(℃)");         // 14
    report.write(1, 15, "LC3平均(℃)");         // 15（新增）

    // 2. 写入数据行
    int row = 2;
    for (const auto &record : m_calibrationData) {
        // 基础数据（原有逻辑不变）
        float blackbodyTarget = record.blackbodyTarget;
        QDateTime measureTime = record.measureTime;
        float blackbodyAvg = record.blackbodyAvg;
        QString comPort = record.comPort;
        QString deviceType = record.irData.type;
        const QVector<float>& toAvgs = record.irData.toAvgs;
        const QVector<float>& taAvgs = record.irData.taAvgs;
        const QVector<float>& lcAvgs = record.irData.lcAvgs; // 新增：LC平均值

        // 环境温度（原有逻辑不变）
        int tempIndex = m_blackbodyTempPoints.indexOf(blackbodyTarget);
        float envTemperature = (tempIndex != -1 && tempIndex < m_humidityTempPoints.size())
                                   ? m_humidityTempPoints[tempIndex]
                                   : qQNaN();

        // 写入基础数据（原有逻辑不变）
        report.write(row, 1, blackbodyTarget);
        report.write(row, 2, envTemperature);
        report.write(row, 3, measureTime.toString("yyyy-MM-dd HH:mm"));
        report.write(row, 4, blackbodyAvg);
        report.write(row, 5, comPort);
        report.write(row, 6, deviceType);

        // 写入TO/TA/LC数据（按新列顺序）
        for (int i = 0; i < 3; ++i) { // 固定3组（单头后两组留空）
            // TO列（7,10,13）
            if (i < toAvgs.size() && qIsFinite(toAvgs[i])) {
                report.write(row, 7 + i*3, toAvgs[i]); // 7+0=7（TO1）,7+3=10（TO2）,7+6=13（TO3）
            } else {
                report.write(row, 7 + i*3, "");
            }

            // TA列（8,11,14）
            if (i < taAvgs.size() && qIsFinite(taAvgs[i])) {
                report.write(row, 8 + i*3, taAvgs[i]); // 8+0=8（TA1）,8+3=11（TA2）,8+6=14（TA3）
            } else {
                report.write(row, 8 + i*3, "");
            }

            // LC列（9,12,15）（新增）
            if (i < lcAvgs.size() && qIsFinite(lcAvgs[i])) {
                report.write(row, 9 + i*3, lcAvgs[i]); // 9+0=9（LC1）,9+3=12（LC2）,9+6=15（LC3）
            } else {
                report.write(row, 9 + i*3, "");
            }
        }

        setCurrentOperation(QString("已写入第%1行记录 - COM口：%2，设备类型：%3")
                                .arg(row - 1).arg(comPort).arg(deviceType));
        row++;
    }

    // 3. 保存报告
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString fileName = QString("measurement_record_%1.xlsx").arg(timestamp);

    if (report.saveAs(fileName)) {
        setCurrentOperation(QString("测量记录保存成功：%1").arg(fileName));

        m_currentState = Finished;
        emit stateChanged(Finished);

        QTimer::singleShot(2000, this, [this]() {
            m_currentState = Idle;
            emit stateChanged(Idle);
            setCurrentOperation("标校已完成，准备就绪");
        });

        emit calibrationFinished(m_calibrationData);
    } else {
        setCurrentOperation("测量记录保存失败");
        emit errorOccurred("测量记录保存失败");
    }
}


void CalibrationManager::reverseSwitch(int index)
{
    // 检查是否取消
    if (m_canceling) {
        setCurrentOperation("标校已取消，停止反向切换");
        return;
    }

    // 检查是否暂停
    if (m_paused) {
        setCurrentOperation("标校处于暂停状态，反向切换暂停");
        return;
    }

    if (sensorSwitchCount > 0 && !m_paused) {
        m_humidityController->changeSensor(-1);
        sensorSwitchCount--;
        setCurrentOperation(QString("第%1个温度点反向切换测温仪，剩余切换次数：%2").arg(index + 1).arg(sensorSwitchCount));

        // 定时器触发前再次检查取消状态（关键修改）
        QTimer::singleShot(10 * 1000, this, [this, index]() {
            if (!m_canceling) { // 仅在未取消时继续
                this->reverseSwitch(index);
            } else {
                setCurrentOperation("标校已取消，取消反向切换定时器任务");
            }
        });
    } else {
        setCurrentOperation(QString("第%1个温度点反向切换完成，准备处理下一个温度点").arg(index + 1));
        m_pausedStage = None; // 新增：重置阶段标记
        int progress = (index + 1) * 100 / m_blackbodyTempPoints.size();
        emit calibrationProgress(progress);
        if (!m_canceling) { // 仅在未取消时继续
            calibrateNextPoint(index + 1);
        }
    }
}

void CalibrationManager::setCurrentOperation(const QString &operation)
{
    currentOperation = operation;
    emit currentOperationChanged(operation);
    qDebug() << "操作状态：" << operation;
}

QString CalibrationManager::getCurrentOperation() const
{
    return currentOperation;
}

CalibrationManager::State CalibrationManager::getCurrentState() const
{
    return m_currentState;
}

void CalibrationManager::pauseCalibration()
{
    if (m_currentState == Running) {
        m_currentState = Paused;
        m_paused = true;

        // 停止所有活动的定时器
        m_timer.stop();
        m_minuteSampleTimer.stop();
        m_fiveMinuteTimer.stop();
        m_sixMinuteTimer.stop();
        m_countdownTimer.stop();
        m_waitNextMinuteTimer.stop();

        setCurrentOperation("标校已暂停 - 所有定时器已停止");
        emit stateChanged(Paused);
    }
}

void CalibrationManager::resumeCalibration()
{
    if (m_currentState != Paused) return;

    m_currentState = Running;
    m_paused = false;
    emit stateChanged(Running);
    setCurrentOperation("标校已恢复 - 继续执行当前步骤");

    // 新增：校验“等待下一分钟”阶段的有效性
    bool stageValid = false;
    switch (m_pausedStage) {
    case StabilityCheck:
        stageValid = (m_timer.isActive() || !m_samples.isEmpty());
        break;
    case WaitingForNextMinute:  // 新增：校验等待下一分钟阶段
        stageValid = (m_countdownTimer.isActive() || m_totalWaitSeconds > 0);
        break;
    case WaitingSixMinutes:
        stageValid = (m_sixMinuteTimer.isActive() || !m_minuteSamples.isEmpty());
        break;
    case MinuteSampling:
        stageValid = m_minuteSampleTimer.isActive();
        break;


    default:
        stageValid = false;
    }

    // 如果阶段标记无效，强制重置为None
    if (!stageValid) {
        setCurrentOperation("检测到无效的暂停阶段，使用默认恢复逻辑");
        m_pausedStage = None;
    }

    // 根据阶段恢复流程
    switch (m_pausedStage) {
    case StabilityCheck:
        m_timer.start();
        setCurrentOperation("恢复温度稳定检查流程 - 继续采样");
        break;

    case WaitingForNextMinute: {  // 新增：恢复等待下一分钟阶段
        qint64 elapsed = m_currentWaitStartTime.secsTo(QDateTime::currentDateTime());
        qint64 remaining = m_totalWaitSeconds - elapsed;
        remaining = qMax(remaining, static_cast<qint64>(1));

        setCurrentOperation(QString("恢复等待下一分钟 - 已等待%1秒，剩余%2秒").arg(elapsed).arg(remaining));

        // 重启倒计时定时器
        m_countdownTimer.start();

        // 重启“等待下一分钟”成员定时器
        m_waitNextMinuteTimer.start(remaining * 1000);
        break;
    }


    case WaitingSixMinutes: {
        // （原有6分钟等待阶段恢复逻辑不变）
        qint64 elapsed = m_startSixMinuteTime.secsTo(QDateTime::currentDateTime());
        qint64 totalWait = 6 * 60;
        qint64 remaining = totalWait - elapsed;
        remaining = qMax(remaining, static_cast<qint64>(1));

        setCurrentOperation(QString("恢复6分钟等待 - 已等待%1秒，剩余%2秒").arg(elapsed).arg(remaining));

        if (remaining > 0) {
            qint64 remainingTo5Min = 5 * 60 - elapsed;

            if (remainingTo5Min <= 0) {
                setCurrentOperation("恢复：已过5分钟，直接启动最后一分钟采样");
                m_minuteSampleTimer.start();
                m_pausedStage = MinuteSampling;
                m_currentWaitStartTime = QDateTime::currentDateTime();
                m_totalWaitSeconds = remaining;
                m_countdownTimer.start();
                m_sixMinuteTimer.start(remaining * 1000);
            } else {
                m_fiveMinuteTimer.start(remainingTo5Min * 1000);
                m_sixMinuteTimer.start(remaining * 1000);
                m_currentWaitStartTime = QDateTime::currentDateTime();
                m_totalWaitSeconds = remaining;
                m_countdownTimer.start();
            }
        } else {
            recordMeasurement(m_currentIndex);
        }
        break;
    }

    case MinuteSampling: {
        // （原有最后1分钟采样阶段恢复逻辑不变）
        qint64 elapsed = m_startSixMinuteTime.secsTo(QDateTime::currentDateTime());
        qint64 totalWait = 6 * 60;
        qint64 remaining = totalWait - elapsed;

        if (remaining > 0) {
            setCurrentOperation(QString("恢复最后1分钟采样 - 剩余%1秒").arg(remaining));
            m_minuteSampleTimer.start();
            m_sixMinuteTimer.start(remaining * 1000);
            m_currentWaitStartTime = QDateTime::currentDateTime();
            m_totalWaitSeconds = remaining;
            m_countdownTimer.start();
        } else {
            recordMeasurement(m_currentIndex);
        }
        break;
    }

    default:
        // 关键修改：默认分支不再调用checkAllSensorsMeasured，而是根据当前索引重新进入测量流程
        if (m_currentIndex >= 0 && m_currentIndex < m_blackbodyTempPoints.size()) {
            setCurrentOperation(QString("恢复到第%1个温度点的测量准备阶段").arg(m_currentIndex + 1));
            // 重新进入测量流程（从打开窗口开始）
            startMeasurement(m_currentIndex);
        } else {
            setCurrentOperation("无有效恢复点，重新开始当前温度点");
            calibrateNextPoint(m_currentIndex);
        }
        break;
    }

    // 恢复后清空阶段标记
    m_pausedStage = None;
}

void CalibrationManager::cancelCalibration()
{
    if (m_currentState == Running || m_currentState == Paused) {
        m_currentState = Canceling;
        m_canceling = true;

        // 1. 停止所有定时器并断开连接（关键修改）
        m_timer.stop();
        m_minuteSampleTimer.stop();
        m_fiveMinuteTimer.stop();
        m_sixMinuteTimer.stop();
        m_countdownTimer.stop();

        // 断开所有定时器的连接，防止已排队的事件执行
        disconnect(&m_timer, nullptr, this, nullptr);
        disconnect(&m_minuteSampleTimer, nullptr, this, nullptr);
        disconnect(&m_fiveMinuteTimer, nullptr, this, nullptr);
        disconnect(&m_sixMinuteTimer, nullptr, this, nullptr);
        disconnect(&m_countdownTimer, nullptr, this, nullptr);

        // 2. 立即关闭设备和窗口
        m_humidityController->toggleCalibrationWindow(false);
        m_blackbodyController->setDeviceState(false);
        m_humidityController->setDeviceState(false);

        // ====== 新增：发送红外测量停止信号 ======
        emit irMeasurementStopped();  // 关键：此时设备已关闭，发送停止信号
        // ======================================

        setCurrentOperation("标校正在取消...");
        emit stateChanged(Canceling);

        // 3. 立即重置所有状态变量（关键修改）
        m_currentIndex = -1;
        m_currentSensorIndex = 0;
        sensorSwitchCount = 0;
        m_samples.clear();
        m_minuteSamples.clear();

        // 4. 延迟确认取消完成（确保所有操作终止）
        QTimer::singleShot(1000, this, [this]() {
            m_currentState = Idle;
            m_canceling = false;
            emit stateChanged(Idle);
            setCurrentOperation("标校已完全取消 - 所有设备已停止");
        });
    }
}

// 新增：倒计时定时器处理函数
void CalibrationManager::onCountdownTimerTimeout()
{
    qint64 elapsed = m_currentWaitStartTime.secsTo(QDateTime::currentDateTime());
    qint64 remaining = m_totalWaitSeconds - elapsed;
    remaining = qMax(remaining, static_cast<qint64>(0));

    // 发射倒计时更新信号
    emit countdownUpdated(remaining, m_currentCountdownStage);
}

void CalibrationManager::onWaitNextMinuteTimeout()
{
    // 检查当前状态，若已暂停或取消，则不执行后续操作
    if (m_paused || m_canceling || m_currentState != Running) {
        setCurrentOperation("等待下一分钟的定时器已超时，但当前处于非运行状态，不进入下一阶段");
        return;
    }

    // 正常超时后进入6分钟等待阶段
    m_pausedStage = None;
    m_countdownTimer.stop();
    this->startWaitingSixMinutes(m_currentWaitIndex); // 使用记录的索引
}

void CalibrationManager::onIrAverageReceived(const QString& comPort, const CalibrationManager::InfraredData& irData) {
    if (m_tempRecords.isEmpty()) return;

    // 按COM口匹配临时记录
    auto it = std::find_if(m_tempRecords.begin(), m_tempRecords.end(),
                           [&](const CalibrationRecord& r) { return r.comPort == comPort; });
    if (it == m_tempRecords.end()) return;

    // 补全红外数据
    CalibrationRecord finalRecord = *it;
    finalRecord.irData = irData;
    m_tempRecords.erase(it);

    // 存储完整记录
    m_calibrationData.append(finalRecord);
}
