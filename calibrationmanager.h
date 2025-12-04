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
    // 红外数据结构
    struct InfraredData {
        QString type;
        QVector<float> toAvgs;
        QVector<float> taAvgs;
        QVector<float> lcAvgs;
    };

    // 最终记录结构
    struct CalibrationRecord {
        float blackbodyTarget; // 设定温度
        float blackbodyReal;   // 测量期间黑体炉的“最后一分钟平均值”
        QDateTime measureTime; // 测量时间点
        QString comPort;
        InfraredData irData;
        int physicalPosition;
        QString pointType;     // 温度点类型（"建模" 或 "验证"）
        QString environmentType; // 环境类型（"箱内" 或 "箱外"）
    };

    // 批次基准数据
    struct BatchReferenceData {
        float blackbodyTarget;
        float blackbodyReal;
        QDateTime measureTime;
        QString pointType;
    };

    // 流程阶段
    enum PausedStage {
        None,
        StabilityCheck,
        WaitingForNextMinute,
        ServoMoving,
        SensorStabilizing
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

    // 增加 envType 参数
    void startCalibration(const QVector<float> &modelingPoints, const QVector<float> &verifyPoints, const QVector<float> &humidityPoints, const QString &envType);

    void pauseCalibration();
    void resumeCalibration();
    void cancelCalibration();
    State getCurrentState() const;
    void setCurrentOperation(const QString &operation);
    QString getCurrentOperation() const;

    void setServoController(ServoMotorController *servo);
    void setMeasurementQueue(const QVector<SensorTask>& queue);

    void onIrAverageReceived(const QString& comPort, const CalibrationManager::InfraredData& irData);

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
    void startBatchSequence(int index);
    void calibrateNextPoint(int index);

    void generateCalibrationReport(bool isFinal = true);

    void onCountdownTimerTimeout();
    void onWaitNextMinuteTimeout();
    void onServoInPosition();
    void onSensorStabilizeTimeout();
    void onSamplingTimerTimeout();
    void onServoTimeout(); // 【新增】电机移动超时处理槽函数

private:
    BlackbodyController *m_blackbodyController;
    HumidityController *m_humidityController;
    ServoMotorController *m_servo = nullptr;

    struct TempPointInfo {
        float temp;
        QString type;
    };
    QVector<TempPointInfo> m_allTempPoints;

    QVector<float> m_humidityTempPoints;
    QVector<CalibrationRecord> m_calibrationData;

    QString m_currentReportFileName;
    QString m_environmentType;

    QTimer m_stabilityTimer;
    QTimer m_sensorStabilizeTimer;
    QTimer m_countdownTimer;
    QTimer m_waitNextMinuteTimer;
    QTimer m_samplingTimer;
    QTimer m_servoTimeoutTimer; // 【新增】电机超时定时器

    QVector<float> m_bbRealtimeSamples;
    QVector<float> m_stabilitySamples;
    int m_sampleCount = 0;

    State m_currentState = Idle;
    PausedStage m_pausedStage = None;
    bool m_paused = false;
    bool m_canceling = false;
    int m_currentIndex = -1;
    QString currentOperation;

    int m_currentTempPointIndex = 0;
    QVector<SensorTask> m_taskQueue;
    int m_currentTaskIndex = 0;
    const double DEGREES_PER_SLOT = 36.0;

    QDateTime m_currentWaitStartTime;
    int m_totalWaitSeconds = 0;
    QString m_currentCountdownStage;
    int m_currentWaitIndex = 0;

    QDateTime m_waitStartTime;
    int m_waitTotalSeconds = 0;
    QString m_waitDescription;

    BatchReferenceData m_currentBatchData;

    void startSensorSequence();
    void processCurrentTask();
    void finishSequence();
};

Q_DECLARE_METATYPE(CalibrationManager::InfraredData)

#endif // CALIBRATIONMANAGER_H
