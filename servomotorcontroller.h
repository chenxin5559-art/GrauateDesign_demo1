#ifndef SERVOMOTORCONTROLLER_H
#define SERVOMOTORCONTROLLER_H

#include <QObject>
#include <QSerialPort>
#include <QTimer>
#include <QDebug>

class ServoMotorController : public QObject
{
    Q_OBJECT
public:
    explicit ServoMotorController(QObject *parent = nullptr);
    ~ServoMotorController();

    // 连接设备
    bool connectDevice(const QString &portName, int baudRate = 9600);
    void disconnectDevice();
    bool isConnected() const;

    // 核心运动控制
    void moveRelative(double angle);   // 相对转动（正数为顺时针）
    void moveToAbsolute(double angle); // 绝对角度定位（基于0点）
    void moveToZero();                 // 回零（复位）
    void stop();                       // 急停

signals:
    void positionReached();            // 信号：已到达目标位置
    void errorOccurred(const QString &msg);
    void logMessage(const QString &msg); // 用于界面日志显示

private slots:
    void onDataReceived();
    void checkPositionStatus(); // 定时查询位置

private:
    QSerialPort *m_serial;
    QTimer *m_pollTimer;

    // 电机参数配置
    const int COUNTS_PER_REV = 1310720; // 一圈脉冲数
    const int POSITION_TOLERANCE = 1000; // 到位判断允许误差(脉冲数)

    int m_targetCounts = 0;  // 目标脉冲位置
    bool m_isMoving = false; // 是否正在运动中

    void sendCommand(const QString &cmd);
    int angleToCounts(double angle);
};

#endif // SERVOMOTORCONTROLLER_H
