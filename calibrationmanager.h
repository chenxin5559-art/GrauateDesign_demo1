#ifndef CALIBRATIONMANAGER_H
#define CALIBRATIONMANAGER_H

#include <QObject>
#include <QVector>
#include <QPair>
#include <QDateTime>
#include "blackbodycontroller.h"
#include "humiditycontroller.h"
#include <QTimer>
#include <QStringList>

class CalibrationManager : public QObject
{
    Q_OBJECT
public:

    struct InfraredData {
        QString type; // 设备类型（"单头"/"多头"）
        QVector<float> toAvgs; // TO通道平均温度（单头1个，多头3个）
        QVector<float> taAvgs; // TA通道平均温度（单头1个，多头3个）
        QVector<float> lcAvgs; // 新增：LC通道平均温度（单头1个，多头3个）
    };

    struct CalibrationRecord {
        float blackbodyTarget; // 黑体炉目标温度
        QDateTime measureTime; // 测量时间
        float blackbodyAvg; // 黑体炉最后一分钟平均温度
        QString comPort; // 红外测温仪COM口
        InfraredData irData; // 红外数据
    };

    // 定义流程阶段（细分步骤）
    enum PausedStage {
        None,               // 无（未暂停）
        StabilityCheck,     // 温度稳定检查阶段（滑动窗口采样中）
        WaitingForNextMinute, // 新增：等待下一分钟开始测量
        WaitingSixMinutes,  // 6分钟等待阶段（未到最后1分钟采样）
        MinuteSampling      // 最后1分钟采样阶段
    };

    enum State {
        Idle,           // 空闲状态
        Running,        // 运行中
        Paused,         // 已暂停
        Canceling,      // 取消中
        Finished        // 已完成
    };
    Q_ENUM(State)

    void pauseCalibration();
    void resumeCalibration();
    void cancelCalibration();
    State getCurrentState() const;

    explicit CalibrationManager(BlackbodyController *blackbodyController, HumidityController *humidityController, QObject *parent = nullptr);
    ~CalibrationManager();

    void startCalibration(const QVector<float> &blackbodyTempPoints, const QVector<float> &humidityTempPoints);

    void setCurrentOperation(const QString &operation);
    QString getCurrentOperation() const;


signals:
    void calibrationFinished(const QVector<CalibrationRecord> &calibrationData);
    void errorOccurred(const QString &error);
    void currentOperationChanged(const QString &operation);
    void calibrationProgress(int progress);
    void stateChanged(State newState);
    // 新增：倒计时更新信号
    void countdownUpdated(int secondsRemaining, const QString &stage);

    // 新增：红外测量开始/结束信号（携带当前COM口）
    void irMeasurementStarted(const QString &currentComPort);
    void irMeasurementStopped();

    // 请求红外平均数据（参数：COM口，接收者）
    void requestIrAverage(const QString& comPort, QObject* receiver);

private slots:
    void checkStability(int index);
    void startMeasurement(int index);
    void recordMeasurement(int index);
    void checkAllSensorsMeasured(int index);
    void calibrateNextPoint(int index);
    void generateCalibrationReport();
    void startWaitingSixMinutes(int index);
    void reverseSwitch(int index);

    void onCountdownTimerTimeout();

    void onWaitNextMinuteTimeout();

    // 接收红外平均数据
    void onIrAverageReceived(const QString& comPort, const CalibrationManager::InfraredData& irData);

private:
    BlackbodyController *m_blackbodyController;
    HumidityController *m_humidityController;
    QVector<float> m_blackbodyTempPoints;
    QVector<float> m_humidityTempPoints;
    QVector<CalibrationRecord> m_calibrationData;
    int m_currentSensorIndex = 0;
    QTimer m_timer;
    QVector<QPair<float, float>> m_samples;      // 存储采样的温度数据
    QVector<float> m_humiditySamples;            // 存储采样的湿度数据
    int m_sampleCount = 0;                       // 当前采样次数
    int m_targetSampleCount = 0;                 // 目标采样次数
    QStringList m_comPorts; // 存储COM口号列表
    QDateTime m_fifthMinuteTime; // 记录第5分钟的时间
    int sensorSwitchCount; // 记录切换测温仪的次数
    QString currentOperation; // 记录当前操作描述

    QVector<float> m_minuteSamples;   // 存储一分钟内的温度采样
    QTimer m_minuteSampleTimer;       // 一分钟采样定时器

    State m_currentState = Idle;
    bool m_paused = false;
    bool m_canceling = false;
    int m_currentIndex = -1;          // 当前处理的温度点索引

    PausedStage m_pausedStage = None; // 记录暂停时的阶段

    QDateTime m_startSixMinuteTime;   // 6分钟等待开始时间
    QDateTime m_currentWaitStartTime; // 当前等待阶段开始时间（通用）
    int m_totalWaitSeconds;           // 当前等待阶段总时长（秒）

    QTimer m_fiveMinuteTimer;   // 5分钟定时器（用于启动最后一分钟采样）
    QTimer m_sixMinuteTimer;    // 6分钟定时器（用于记录数据）
    QTimer m_countdownTimer;    // 新增：倒计时更新定时器
    QTimer m_waitNextMinuteTimer; // 新增：用于“等待下一分钟”的成员定时器
    int m_currentWaitIndex; // 新增：记录当前等待阶段对应的温度点索引
    QString m_currentCountdownStage;  // 新增：当前倒计时阶段描述

    // 新增：声明临时记录变量
    QVector<CalibrationRecord> m_tempRecords;
};

#endif // CALIBRATIONMANAGER_H
