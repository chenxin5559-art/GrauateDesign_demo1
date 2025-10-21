#ifndef HUMIDITYCONTROLLER_H
#define HUMIDITYCONTROLLER_H

#include <QObject>
#include <QSerialPort>
#include <QModbusRtuSerialMaster>

class HumidityController : public QObject
{
    Q_OBJECT
public:
    explicit HumidityController(QObject* parent = nullptr);
    ~HumidityController();

    bool connectDevice(const QString& portName);
    void disconnectDevice();
    bool isConnected() const;
    void setTargetTemperature(float temperature);
    void setDeviceState(bool start);
    void setMasterControl(bool enable);
    void changeSensor(int direction);
    void toggleCalibrationWindow(bool open);

    void readCurrentTemperature();
    void readCurrentHumidity();
    void readCurrentData();

    float getCurrentTemperature() const;
    float getCurrentHumidity() const;


signals:
    void connectionStatusChanged(bool connected);
    void currentTemperatureUpdated(float temp);
    void currentHumidityUpdated(float humidity);
    void errorOccurred(const QString& error);
    void targetTemperatureSet(bool success);
    void masterControlChanged(bool acquired);
    void currentDataUpdated(float temp, float humidity);

private:
    QModbusRtuSerialMaster* m_modbusDevice = nullptr;
    quint8 m_slaveAddress;

    void configureSerialPort(const QString& portName);
    void connectSignals();

    void writeRegister(quint16 address, float value);
    void sendReadRequest(quint16 address, int count, std::function<void(const QModbusDataUnit&)> callback);
    void sendWriteRequest(const QModbusDataUnit& unit, std::function<void(bool)> callback = nullptr);

    float m_currentTemperature = 0.0f; // 新增成员变量用于存储当前温度
    float m_currentHumidity = 0.0f;    // 新增成员变量用于存储当前湿度

    QString m_portName;       // 保存当前端口名
    bool m_connected = false; // 连接状态标识
};

#endif // HUMIDITYCONTROLLER_H
