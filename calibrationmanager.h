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
#include "ServoMotorController.h"

// 定义任务结构体
struct SensorTask {
    QString comPort;  // 串口号
    int position;     // 物理位置 (1-10)
};

class CalibrationManager : public QObject
{
    Q_OBJECT
public:

    struct InfraredData {
        QString type; // 设备类型（"单头"/"多头"）
        QVector<float> toAvgs;
        QVector<float> taAvgs;
        QVector<float> lcAvgs;
    };

    struct CalibrationRecord {
        float blackbodyTarget; // 黑体炉目标温度
        QDateTime measureTime; // 测量时间
        float blackbodyAvg;    // 黑体炉最后一分钟平均温度
        QString comPort;       // 红外测温仪COM口
        InfraredData irData;   // 红外数据
        int physicalPosition;  // 物理位置
    };

    // 保存当前批次的基准数据
    struct BatchReferenceData {
        float blackbodyTarget;
        float blackbodyAvg;
        QDateTime measureTime;
    };

    // 定义流程阶段
    enum PausedStage {
        None,
        StabilityCheck,
        WaitingForNextMinute,
        WaitingSixMinutes,
        MinuteSampling,
        ServoMoving
    };

    enum State {
        Idle,
        Running,
        Paused,
        Canceling,
        Finished
    };
    Q_ENUM(State)

    explicit CalibrationManager(BlackbodyController *blackbodyController, HumidityController *humidityController, QObject *parent = nullptr);
    ~CalibrationManager();

    void startCalibration(const QVector<float> &blackbodyTempPoints, const QVector<float> &humidityTempPoints);

    void pauseCalibration();
    void resumeCalibration();
    void cancelCalibration();
    State getCurrentState() const;
    void setCurrentOperation(const QString &operation);
    QString getCurrentOperation() const;

    void setServoController(ServoMotorController *servo);
    void setMeasurementQueue(const QVector<SensorTask>& queue);

signals:
    void calibrationFinished(const QVector<CalibrationRecord> &calibrationData);
    void errorOccurred(const QString &error);
    void currentOperationChanged(const QString &operation);
    void calibrationProgress(int progress);
    void stateChanged(State newState);
    void countdownUpdated(int secondsRemaining, const QString &stage);
    void irMeasurementStarted(const QString &currentComPort);
    void irMeasurementStopped();
    void requestIrAverage(const QString& comPort, QObject* receiver);

private slots:
    void checkStability(int index);
    void startMeasurement(int index);
    void recordMeasurement(int index);

    void calibrateNextPoint(int index);
    void generateCalibrationReport();
    void startWaitingSixMinutes(int index);

    void onCountdownTimerTimeout();
    void onWaitNextMinuteTimeout();

    // 【新增】将原来Lambda里的逻辑提取为槽函数
    void onFiveMinuteTimeout();   // 5分钟等待结束，开始采样
    void onSixMinuteTimeout();    // 6分钟等待结束，记录数据
    void onMinuteSampleTimeout(); // 每秒采样一次

    void onIrAverageReceived(const QString& comPort, const CalibrationManager::InfraredData& irData);
    void onServoInPosition();

private:
    BlackbodyController *m_blackbodyController;
    HumidityController *m_humidityController;
    ServoMotorController *m_servo = nullptr;

    QVector<float> m_blackbodyTempPoints;
    QVector<float> m_humidityTempPoints;
    QVector<CalibrationRecord> m_calibrationData;

    QTimer m_timer;
    QTimer m_fiveMinuteTimer;
    QTimer m_sixMinuteTimer;
    QTimer m_minuteSampleTimer;
    QTimer m_countdownTimer;
    QTimer m_waitNextMinuteTimer;

    QVector<QPair<float, float>> m_samples;
    QVector<float> m_humiditySamples;
    QVector<float> m_minuteSamples;
    int m_sampleCount = 0;

    State m_currentState = Idle;
    PausedStage m_pausedStage = None;
    bool m_paused = false;
    bool m_canceling = false;
    int m_currentIndex = -1;
    QString currentOperation;

    QDateTime m_startSixMinuteTime;
    QDateTime m_currentWaitStartTime;
    int m_totalWaitSeconds = 0;
    QString m_currentCountdownStage;
    int m_currentWaitIndex = 0;

    QVector<SensorTask> m_taskQueue;
    int m_currentTaskIndex = 0;
    BatchReferenceData m_currentBatchData;
    const double DEGREES_PER_SLOT = 40.0;

    void startSensorSequence();
    void processCurrentTask();
    void finishSequence();
};

#endif // CALIBRATIONMANAGER_H

