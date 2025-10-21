#include "HumidityController.h"
#include <QModbusDataUnit>
#include <QModbusReply>
#include <QVariant>
#include <QDebug>

HumidityController::HumidityController(QObject* parent)
    : QObject(parent), m_slaveAddress(0x03), m_connected(false)
{
    m_modbusDevice = new QModbusRtuSerialMaster(this);

    // 连接状态信号
    connect(m_modbusDevice, &QModbusClient::stateChanged, this, [this](QModbusDevice::State state) {
        bool connected = (state == QModbusDevice::ConnectedState);
        m_connected = connected;
        emit connectionStatusChanged(connected);
    });
}

HumidityController::~HumidityController()
{
    if (m_modbusDevice) {
        m_modbusDevice->disconnectDevice();
    }
}

bool HumidityController::connectDevice(const QString& portName)
{
    if (m_connected) {
        disconnectDevice();
    }

    m_portName = portName;
    configureSerialPort(portName);  // 配置端口参数

    if (m_modbusDevice->connectDevice()) {
        m_connected = true;
        emit connectionStatusChanged(true);
        return true;
    } else {
        m_connected = false;
        emit connectionStatusChanged(false);
        return false;
    }
}

// 断开连接
void HumidityController::disconnectDevice()
{
    if (m_modbusDevice->state() == QModbusDevice::ConnectedState) {
        m_modbusDevice->disconnectDevice();
        m_connected = false;
        emit connectionStatusChanged(false);
    }
}

// 获取连接状态
bool HumidityController::isConnected() const
{
    return m_connected;
}

void HumidityController::configureSerialPort(const QString& portName)
{
    m_modbusDevice->setConnectionParameter(QModbusDevice::SerialPortNameParameter, portName);
    m_modbusDevice->setConnectionParameter(QModbusDevice::SerialBaudRateParameter, QVariant(9600));
    m_modbusDevice->setConnectionParameter(QModbusDevice::SerialDataBitsParameter, QVariant(8));
    m_modbusDevice->setConnectionParameter(QModbusDevice::SerialParityParameter, QVariant(QSerialPort::NoParity));
    m_modbusDevice->setConnectionParameter(QModbusDevice::SerialStopBitsParameter, QVariant(QSerialPort::OneStop));
}

void HumidityController::setTargetTemperature(float temperature)
{
    writeRegister(0x000A, temperature);
}

void HumidityController::setDeviceState(bool start)
{
    QModbusDataUnit unit(QModbusDataUnit::HoldingRegisters, 0x0001, 1);
    unit.setValue(0, start ? 0x0001 : 0x0000);
    sendWriteRequest(unit);
}

void HumidityController::setMasterControl(bool enable) {
    QModbusDataUnit unit(QModbusDataUnit::HoldingRegisters, 0x0000, 1);
    unit.setValue(0, enable ? 0x0001 : 0x0000);
    sendWriteRequest(unit, [this, enable](bool success) {
        if (success) {
            qDebug() << "恒温箱上位机控制" << (enable ? "已获取" : "已释放");
            emit masterControlChanged(enable); // 成功时发射信号
        } else {
            qDebug() << "恒温箱上位机控制" << (enable ? "获取失败" : "释放失败");
            emit errorOccurred(enable ? "获取控制失败" : "释放控制失败");
            // 不发射控制权变更信号
        }
    });
}

void HumidityController::changeSensor(int direction)
{
    // 这里假设direction为1表示下一个，-1表示上一个
    QModbusDataUnit unit(QModbusDataUnit::HoldingRegisters, 0x0018, 1);
    unit.setValue(0, direction > 0 ? 0x01 : 0x02);
    sendWriteRequest(unit);
}

void HumidityController::toggleCalibrationWindow(bool open)
{
    QModbusDataUnit unit(QModbusDataUnit::HoldingRegisters, 0x001A, 1);
    unit.setValue(0, open ? 0x01 : 0x00);
    sendWriteRequest(unit);
}

void HumidityController::connectSignals()
{
    connect(m_modbusDevice, &QModbusClient::stateChanged, this, [this](QModbusDevice::State state) {
        emit connectionStatusChanged(state == QModbusDevice::ConnectedState);
    });

    connect(m_modbusDevice, &QModbusDevice::errorOccurred, this, [this](QModbusDevice::Error error) {
        emit errorOccurred(m_modbusDevice->errorString());
    });
}

void HumidityController::readCurrentTemperature() {
    sendReadRequest(0x0010, 2, [this](const QModbusDataUnit& unit) {
        if (unit.valueCount() == 2) {
            // 检查大端/小端模式（若从站为小端，交换寄存器顺序）
            quint32 tempData = (unit.value(0) << 16) | unit.value(1); // 大端模式
            // quint32 tempData = (unit.value(1) << 16) | unit.value(0); // 小端模式
            float temperature;
            memcpy(&temperature, &tempData, sizeof(float));
            m_currentTemperature = temperature; // 存储当前温度
            // qDebug() << "读取到的温度数据：" << temperature;
            emit currentTemperatureUpdated(temperature);
        } else {
            qDebug() << "读取温度数据长度错误，实际长度: " << unit.valueCount();
        }
    });
}

void HumidityController::readCurrentHumidity()
{
    sendReadRequest(0x0014, 2, [this](const QModbusDataUnit& unit) {
        if (unit.valueCount() == 2) {
            quint32 humiData = (unit.value(0) << 16) | unit.value(1);
            float humidity;
            memcpy(&humidity, &humiData, sizeof(float));
            m_currentHumidity = humidity;
            // qDebug() << "读取到的湿度数据：" << humidity;
            emit currentHumidityUpdated(humidity);
        } else {
            qDebug() << "读取湿度数据长度错误，实际长度: " << unit.valueCount();
        }
    });
}

void HumidityController::writeRegister(quint16 address, float value)
{
    quint32 data;
    memcpy(&data, &value, sizeof(quint32));
    QModbusDataUnit unit(QModbusDataUnit::HoldingRegisters, address, 2);
    unit.setValue(0, data >> 16);
    unit.setValue(1, data & 0xFFFF);
    sendWriteRequest(unit, [this](bool success) {
        emit targetTemperatureSet(success);
        if (!success) {
            emit errorOccurred("目标温度设置失败"); // 添加错误信号
        }
    });
}

void HumidityController::sendReadRequest(quint16 address, int count, std::function<void(const QModbusDataUnit&)> callback)
{

    if (m_modbusDevice->state() != QModbusDevice::ConnectedState) {
        qDebug() << "Modbus 设备未连接，无法发送读取请求，当前设备状态: " << m_modbusDevice->state();
        return;
    }

    // 显式创建读取单元，便于调试
    QModbusDataUnit readUnit(QModbusDataUnit::HoldingRegisters, address, count);

    QModbusReply* reply = m_modbusDevice->sendReadRequest(readUnit, m_slaveAddress);

    if (reply) {

        connect(reply, &QModbusReply::finished, this, [this, callback, reply]() {
            if (reply->error() == QModbusDevice::NoError) {
                callback(reply->result());
            } else {
                qDebug() << "Modbus 读取错误：" << reply->errorString() << " 错误码: " << reply->error();
            }
            reply->deleteLater();
        });
    } else {
        qDebug() << "发送读取请求失败：Modbus 回复为空";
        qDebug() << "Modbus 设备错误：" << m_modbusDevice->errorString() << " 设备状态: " << m_modbusDevice->state();
    }
}

void HumidityController::sendWriteRequest(const QModbusDataUnit& unit, std::function<void(bool)> callback) {
    qDebug() << "写入请求：地址=" << QString::number(unit.startAddress(), 16)
             << "值=" << unit.value(0) << "," << unit.value(1);

    if (auto* reply = m_modbusDevice->sendWriteRequest(unit, m_slaveAddress)) {
        connect(reply, &QModbusReply::finished, this, [callback, reply]() {
            if (reply->error() != QModbusDevice::NoError) {
                qDebug() << "恒温箱Modbus 写入错误：" << reply->errorString(); // 新增错误日志
            }
            if (callback) callback(reply->error() == QModbusDevice::NoError);
            reply->deleteLater();
        });
    } else {
        qDebug() << "发送写入请求失败：" << m_modbusDevice->errorString(); // 新增错误日志
    }
}

void HumidityController::readCurrentData()
{
    sendReadRequest(0x0010, 4, [this](const QModbusDataUnit& unit) {
        if (unit.valueCount() == 4) {
            // 解析温度
            quint32 tempData = (unit.value(0) << 16) | unit.value(1);
            float temperature;
            memcpy(&temperature, &tempData, sizeof(float));

            // 解析湿度
            quint32 humiData = (unit.value(2) << 16) | unit.value(3);
            float humidity;
            memcpy(&humidity, &humiData, sizeof(float));

            qDebug() << "Read temperature: " << temperature << ", humidity: " << humidity;
            emit currentDataUpdated(temperature, humidity);
        } else {
            qDebug() << "Read current data length error, actual length: " << unit.valueCount();
        }
    });
}

float HumidityController::getCurrentTemperature() const
{
    return m_currentTemperature;
}

float HumidityController::getCurrentHumidity() const
{
    return m_currentHumidity;
}
