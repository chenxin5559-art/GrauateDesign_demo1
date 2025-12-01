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
    void resetZeroPoint();             // 【关键】复位零点：发送r指令并重新初始化参数
    void moveRelative(double angle);   // 相对转动（正数为顺时针）
    void moveToAbsolute(double angle); // 模拟绝对定位（基于软件记录的0点计算相对差值）
    void moveToZero();                 // 回零（反转回退到0点）
    void stop();                       // 急停

    double currentAngle() const;       // 获取当前软件记录的角度

    void initDriverParameters(); // 发送全套初始化参数(电流、增益、速度)

    QTimer *m_timeoutTimer;

signals:
    void positionReached();            // 信号：已到达目标位置
    void errorOccurred(const QString &msg);
    void logMessage(const QString &msg);

private slots:
    void onDataReceived();
    void checkPositionStatus(); // 定时查询位置

private:
    QSerialPort *m_serial;
    QTimer *m_pollTimer;

    // 电机参数配置
    const int COUNTS_PER_REV = 1310720; // 一圈脉冲数
    const int POSITION_TOLERANCE = 2000; // 到位判断允许误差(放宽一点防止抖动)

    // 【核心】软件维护的“虚拟绝对位置”
    // 因为驱动器内部计数器不能清零且容易溢出，我们在软件层记录当前应该在哪里
    long long m_currentSoftwareCounts = 0;
    long long m_targetSoftwareCounts = 0;

    bool m_isMoving = false;

    void sendCommand(const QString &cmd);

    int angleToCounts(double angle);
};

#endif // SERVOMOTORCONTROLLER_H
