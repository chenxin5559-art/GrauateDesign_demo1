#ifndef BLACKBODYCONTROLLER_H
#define BLACKBODYCONTROLLER_H

#pragma once
#include <QObject>
#include <QSerialPort>
#include <QModbusRtuSerialMaster>

class BlackbodyController : public QObject
{
    Q_OBJECT
public:
    explicit BlackbodyController(QObject *parent = nullptr);
    ~BlackbodyController();

    bool connectDevice(const QString &portName);
    bool isConnected() const;
    void setMasterControl(bool enable);
    bool connectDevice();
    void disconnectDevice();
    void setTargetTemperature(float temperature);
    void readCurrentTemperature();
    void setDeviceState(bool start);

    float getCurrentTemperature() const;

signals:
    void connectionStatusChanged(bool connected);
    void currentTemperatureUpdated(float temp);
    void errorOccurred(const QString &error);
    void targetTemperatureSet(bool success);
    void masterControlChanged(bool acquired);

private:
    QModbusRtuSerialMaster *m_modbusDevice = nullptr;
    quint8 m_slaveAddress; // 确保初始化为0x02
    quint16 calculateCRC(const QByteArray &data);

    float m_currentTemperature = 0.0f; // 新增成员变量用于存储当前温度

    QString m_portName; // 保存当前端口名
    bool m_connected = false; // 连接状态
};

#endif // BLACKBODYCONTROLLER_H
