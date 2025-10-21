#include "BlackbodyController.h"
#include <QModbusRtuSerialMaster>
#include <QVariant>
#include <QDebug>

BlackbodyController::BlackbodyController(QObject *parent) : QObject(parent), m_slaveAddress(0x02), m_connected(false)
{
    m_modbusDevice = new QModbusRtuSerialMaster(this);

    // 连接状态信号
    connect(m_modbusDevice, &QModbusClient::stateChanged, this, [this](QModbusDevice::State state) {
        bool connected = (state == QModbusDevice::ConnectedState);
        m_connected = connected;
        emit connectionStatusChanged(connected);
    });

    // 错误处理
    connect(m_modbusDevice, &QModbusDevice::errorOccurred, this, [this](QModbusDevice::Error error) {
        if (error == QModbusDevice::ConnectionError && m_connected) {
            m_connected = false;
            emit connectionStatusChanged(false);
        }
    });
}

BlackbodyController::~BlackbodyController()
{
    if (m_modbusDevice && m_modbusDevice->state() == QModbusDevice::ConnectedState) {
        m_modbusDevice->disconnectDevice();
    }
    delete m_modbusDevice;
}

bool BlackbodyController::connectDevice(const QString &portName)
{
    if (m_connected) {
        disconnectDevice();
    }

    m_portName = portName;

    // 配置端口参数
    m_modbusDevice->setConnectionParameter(
        QModbusDevice::SerialPortNameParameter,
        m_portName
        );
    m_modbusDevice->setConnectionParameter(
        QModbusDevice::SerialBaudRateParameter,
        QVariant(9600)
        );
    m_modbusDevice->setConnectionParameter(
        QModbusDevice::SerialDataBitsParameter,
        QVariant(8)  // 固定数据位
        );
    m_modbusDevice->setConnectionParameter(
        QModbusDevice::SerialParityParameter,
        QVariant(QSerialPort::NoParity)  // 固定无校验
        );
    m_modbusDevice->setConnectionParameter(
        QModbusDevice::SerialStopBitsParameter,
        QVariant(QSerialPort::OneStop)  // 固定停止位
        );

    if (m_modbusDevice->connectDevice()) {
        m_connected = true;
        emit connectionStatusChanged(true); // 关键：成功连接时发出信号
        return true;
    } else {
        m_connected = false;
        emit connectionStatusChanged(false);
        return false;
    }
}

void BlackbodyController::disconnectDevice()
{
    if (m_modbusDevice->state() == QModbusDevice::ConnectedState) {
        m_modbusDevice->disconnectDevice();
        m_connected = false;
        emit connectionStatusChanged(false);
        qDebug() << "已断开端口连接:" << m_portName;
    }
}

bool BlackbodyController::isConnected() const
{
    return m_connected;
}

void BlackbodyController::readCurrentTemperature()
{
    if (auto *reply = m_modbusDevice->sendReadRequest(
            QModbusDataUnit(QModbusDataUnit::HoldingRegisters, 0x000C, 2), m_slaveAddress))
    {
        connect(reply, &QModbusReply::finished, this, [this, reply]() {
            if (reply->error() == QModbusDevice::NoError) {
                const QModbusDataUnit unit = reply->result();
                if (unit.valueCount() == 2) {
                    quint32 tempData = (unit.value(0) << 16) | unit.value(1);
                    float temperature;
                    memcpy(&temperature, &tempData, sizeof(float));
                    m_currentTemperature = temperature;
                    emit currentTemperatureUpdated(temperature);
                }
            }
            reply->deleteLater();
        });
    }
}

void BlackbodyController::setTargetTemperature(float temperature)
{
    quint32 tempData;
    memcpy(&tempData, &temperature, sizeof(quint32));

    QModbusDataUnit writeUnit(QModbusDataUnit::HoldingRegisters, 0x000A, 2);
    writeUnit.setValue(0, tempData >> 16);
    writeUnit.setValue(1, tempData & 0xFFFF);

    if (auto *reply = m_modbusDevice->sendWriteRequest(writeUnit, m_slaveAddress)) {
        connect(reply, &QModbusReply::finished, this, [this, reply]() {
            if (reply->error() == QModbusDevice::NoError) {
                emit targetTemperatureSet(true); // 发射成功信号
            } else {
                emit errorOccurred(tr("设置失败: %1").arg(reply->errorString()));
                emit targetTemperatureSet(false); // 发射失败信号
            }
            reply->deleteLater();
        });
    }
}

void BlackbodyController::setDeviceState(bool start)
{
    QModbusDataUnit writeUnit(QModbusDataUnit::HoldingRegisters, 0x0001, 1);
    writeUnit.setValue(0, start ? 0x01 : 0x00);

    if (auto *reply = m_modbusDevice->sendWriteRequest(writeUnit, m_slaveAddress)) {
        connect(reply, &QModbusReply::finished, [this, reply, start]() {
            if (reply->error() == QModbusDevice::NoError) {
                qDebug() << "Device state set to:" << (start ? "ON" : "OFF");
            } else {
                emit errorOccurred(tr("控制命令失败: %1").arg(reply->errorString()));
            }
            reply->deleteLater();
        });
    }
}

quint16 BlackbodyController::calculateCRC(const QByteArray &data) {
    quint16 crc = 0xFFFF;
    for (int pos = 0; pos < data.size(); ++pos) {
        crc ^= (quint8)data[pos];
        for (int i = 8; i != 0; --i) {
            if (crc & 0x0001) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

void BlackbodyController::setMasterControl(bool enable) {
    QModbusDataUnit writeUnit(QModbusDataUnit::HoldingRegisters, 0x0000, 1);
    writeUnit.setValue(0, enable ? 0x0001 : 0x0000);

    if (auto *reply = m_modbusDevice->sendWriteRequest(writeUnit, m_slaveAddress)) {
        connect(reply, &QModbusReply::finished, this, [this, reply, enable]() {
            if (reply->error() == QModbusDevice::NoError) {
                emit masterControlChanged(enable); // 发射控制权状态
                qDebug() << "黑体炉上位机控制" << (enable ? "已获取" : "已释放");
            } else {
                emit errorOccurred(tr("黑体炉控制权操作失败: %1").arg(reply->errorString()));
            }
            reply->deleteLater();
        });
    }
}

float BlackbodyController::getCurrentTemperature() const
{
    return m_currentTemperature;
}
