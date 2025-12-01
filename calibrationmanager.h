#ifndef CALIBRATIONMANAGER_H
#define CALIBRATIONMANAGER_H

#include <QObject>
#include <QVector>
#include <QPair>
#include <QDateTime>
#include "blackbodycontroller.h"
#include "humiditycontroller.h"
#include <QTimer>
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
        float blackbodyReal;   // 【新增】测量时刻的黑体实际温度
        QDateTime measureTime; // 【新增】第5分钟的时间点
        QString comPort;
        InfraredData irData;
        int physicalPosition;
    };

    // 流程阶段
    enum PausedStage {
        None,
        StabilityCheck,     // 1. 等待环境稳定
        ServoMoving,        // 2. 电机移动中
        SensorStabilizing   // 3. 【新增】传感器到位后等待5分钟
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

    // 提供给MainWindow直接调用的回调（解决invokeMethod问题）
    void onIrAverageReceived(const QString& comPort, const CalibrationManager::InfraredData& irData);

signals:
    void calibrationFinished(const QVector<CalibrationRecord> &calibrationData);
    void errorOccurred(const QString &error);
    void currentOperationChanged(const QString &operation);
    void calibrationProgress(int progress);
    void stateChanged(State newState);
    void countdownUpdated(int secondsRemaining, const QString &stage);

    // 红外控制信号
    void irMeasurementStarted(const QString &currentComPort);
    void irMeasurementStopped();
    void requestIrAverage(const QString& comPort, QObject* receiver);

private slots:
    void checkStability(int index);         // 检查环境稳定性
    void onServoInPosition();               // 电机到位
    void onSensorStabilizeTimeout();        // 【新增】5分钟等待结束

    void calibrateNextPoint(int index);     // 下一个温度点
    void generateCalibrationReport();       // 生成报告
    void onCountdownTimerTimeout();         // UI倒计时刷新

private:
    BlackbodyController *m_blackbodyController;
    HumidityController *m_humidityController;
    ServoMotorController *m_servo = nullptr;

    QVector<float> m_blackbodyTempPoints;
    QVector<float> m_humidityTempPoints;
    QVector<CalibrationRecord> m_calibrationData;

    QTimer m_stabilityTimer;        // 稳定性检查定时器
    QTimer m_sensorStabilizeTimer;  // 【新增】5分钟等待定时器
    QTimer m_countdownTimer;        // UI倒计时

    QVector<float> m_stabilitySamples;
    int m_sampleCount = 0;

    State m_currentState = Idle;
    PausedStage m_pausedStage = None;
    bool m_paused = false;
    bool m_canceling = false;
    QString currentOperation;

    // 任务控制变量
    int m_currentTempPointIndex = 0;
    QVector<SensorTask> m_taskQueue;
    int m_currentTaskIndex = 0;
    const double DEGREES_PER_SLOT = 36.0; // 360度/10个间隔

    // 倒计时显示辅助
    QDateTime m_waitStartTime;
    int m_waitTotalSeconds = 0;
    QString m_waitDescription;

    void startSensorSequence();     // 开始一轮传感器测量
    void processCurrentTask();      // 执行当前传感器任务
    void finishSequence();          // 本轮结束收尾
};

Q_DECLARE_METATYPE(CalibrationManager::InfraredData) // 注册元类型

#endif // CALIBRATIONMANAGER_H
