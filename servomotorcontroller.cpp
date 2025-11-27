#include "ServoMotorController.h"
#include <QThread>

ServoMotorController::ServoMotorController(QObject *parent)
    : QObject(parent), m_serial(new QSerialPort(this)), m_pollTimer(new QTimer(this))
{
    // 配置轮询定时器，用于检查电机是否到位
    m_pollTimer->setInterval(200); // 200ms查询一次
    connect(m_pollTimer, &QTimer::timeout, this, &ServoMotorController::checkPositionStatus);

    connect(m_serial, &QSerialPort::readyRead, this, &ServoMotorController::onDataReceived);
}

ServoMotorController::~ServoMotorController() {
    disconnectDevice();
}

bool ServoMotorController::connectDevice(const QString &portName, int baudRate) {
    if (m_serial->isOpen()) m_serial->close();

    m_serial->setPortName(portName);
    m_serial->setBaudRate(baudRate);
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);

    if (m_serial->open(QIODevice::ReadWrite)) {
        emit logMessage(QString("伺服电机连接成功: %1").arg(portName));
        return true;
    } else {
        emit errorOccurred("伺服电机串口打开失败");
        return false;
    }
}

void ServoMotorController::disconnectDevice() {
    if (m_serial->isOpen()) {
        m_serial->close();
        m_pollTimer->stop();
    }
}

bool ServoMotorController::isConnected() const {
    return m_serial && m_serial->isOpen();
}

void ServoMotorController::sendCommand(const QString &cmd) {
    if (!isConnected()) return;

    // 关键：根据要求添加回车换行符
    QByteArray data = (cmd + "\r\n").toLatin1();
    m_serial->write(data);
    m_serial->waitForBytesWritten(50);
    // emit logMessage("发送电机指令: " + cmd);
}

int ServoMotorController::angleToCounts(double angle) {
    // 角度转脉冲： (角度 / 360) * 1310720
    return static_cast<int>((angle / 360.0) * COUNTS_PER_REV);
}

void ServoMotorController::moveToAbsolute(double angle) {
    if (!isConnected()) return;

    m_targetCounts = angleToCounts(angle);

    // 1. 设置目标位置 (寄存器 0x3d)
    sendCommand(QString("s r0x3d %1").arg(m_targetCounts));

    // 2. 稍作延时确保指令被解析(可选)
    QThread::msleep(20);

    // 3. 发送触发运动指令 (t 1)
    sendCommand("t 1");

    m_isMoving = true;
    m_pollTimer->start(); // 开始轮询位置
    emit logMessage(QString("电机开始运动至: %1度 (%2 counts)").arg(angle).arg(m_targetCounts));
}

void ServoMotorController::moveRelative(double angle) {
    // 相对运动无法简单直接读取当前位置来计算（因为读取有延迟），
    // 建议在自动流程中维护一个 m_currentAngle 变量，调用 moveToAbsolute。
    // 这里为了演示，假设你有办法获取当前位置，或者仅用于调试。
    // 如果在自动流程中，建议只使用 moveToAbsolute 配合累加器。
    emit errorOccurred("建议使用绝对定位 moveToAbsolute 以确保精度");
}

void ServoMotorController::moveToZero() {
    moveToAbsolute(0.0);
}

void ServoMotorController::stop() {
    // 发送急停指令，具体看手册，通常可能是设速度为0或特定指令
    // 这里假设重新设置位置为当前位置来停止，或者复位
    // sendCommand("r"); // 视手册复位指令而定
}

void ServoMotorController::checkPositionStatus() {
    // 发送查询实际位置指令 (0x32)
    sendCommand("g r0x32");
}

void ServoMotorController::onDataReceived() {
    QByteArray data = m_serial->readAll();
    QString response = QString::fromLatin1(data).trimmed();

    // 解析返回值，例如 "v 123456"
    if (response.startsWith("v ") && m_isMoving) {
        bool ok;
        // 提取数值部分
        int currentCounts = response.mid(2).toInt(&ok);
        if (ok) {
            // 判断是否到位（允许一定误差）
            if (qAbs(currentCounts - m_targetCounts) <= POSITION_TOLERANCE) {
                m_isMoving = false;
                m_pollTimer->stop();
                emit logMessage("电机已到位");
                emit positionReached(); // 发送信号通知上层逻辑
            }
        }
    }
}
