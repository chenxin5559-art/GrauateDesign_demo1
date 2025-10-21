#include "SerialPortThread.h"
#include <QDebug>
#include <QDateTime>
#include <QMessageBox>
#include <QApplication>

SerialPortThread::SerialPortThread(const QString &portName, int baudRate, QObject *parent)
    : QThread(parent), m_portName(portName), m_baudRate(baudRate)
{
    m_serial = new QSerialPort();
}

SerialPortThread::~SerialPortThread()
{
    closePort();
    delete m_serial;
}

void SerialPortThread::sendData(const QByteArray &data)
{
    QMutexLocker locker(&m_mutex);
    if (m_serial && m_serial->isOpen()) {
        m_serial->write(data);
    } else {
        qWarning() << "串口未打开，无法发送数据";
    }
}


void SerialPortThread::closePort()
{
    // 1. 先请求线程退出（不需要锁）
    requestInterruption();

    // 2. 等待线程结束（不需要锁）
    wait();

    // 3. 最后关闭串口（需要锁）
    {
        QMutexLocker locker(&m_mutex);
        if (m_serial && m_serial->isOpen()) {
            m_serial->clear();
            m_serial->close();
            qDebug() << "Port closed:" << m_portName;
        }
    }

    emit portStatusChanged(false);
}

void SerialPortThread::run()
{
    // ✅ 空循环，线程保持存活即可
    while (!isInterruptionRequested()) {
        QThread::msleep(100);
    }

}


void SerialPortThread::processData(const QString& data)
{
    if(!data.startsWith("ST")) return;

    QStringList parts = data.split(',', Qt::SkipEmptyParts);
    QVector<double> groupST, groupTA, groupLC;  // 新增groupLC存储LC值
    bool isSingleHead = false;

    // 检查是否包含多头设备特征标识（如 "qt"）
    bool hasMultiHeadMarker = parts.contains("qt");

    // 单头设备数据解析（两种格式）
    if (parts.size() >= 4 && !hasMultiHeadMarker) {
        isSingleHead = true;

        bool ok = false;
        // 解析TO和TA
        double TO = parts[2].trimmed().toDouble(&ok);
        if (!ok || !isTemperatureValid(TO)) return;

        double TA = 0.0;
        QString lcPart;  // 存储LC相关部分

        if (parts[3].contains('|')) {
            // 格式1: ST,时间戳, TO, TA| 25.06, 25.06, 25.06,2689,SD
            QStringList taLc = parts[3].split('|', Qt::SkipEmptyParts);
            if (taLc.size() < 1) return;

            // 提取TA（|前的部分）
            TA = taLc[0].trimmed().toDouble(&ok);
            if (!ok || !isTemperatureValid(TA)) return;

            // 核心修改：直接提取第6个字段（索引5）作为LC-1
            lcPart = parts[5].trimmed();  // 第6个字段（0-based索引5）
        } else {
            // 格式2: ST,时间戳, TO, TA,1628,SD（无|，LC-1为TO值）
            TA = parts[3].trimmed().toDouble(&ok);
            if (!ok || !isTemperatureValid(TA)) return;
            lcPart = parts[2].trimmed();  // 第三个字段（TO值）
        }

        // 转换LC值并校验
        double LC = lcPart.toDouble(&ok);
        if (!ok || !isTemperatureValid(LC)) return;

        // 单头设备只存一组数据
        groupST << TO;
        groupTA << TA;
        groupLC << LC;  // 存储LC-1
    }
    // 多头设备数据解析
    else {
        int stIndex = parts.indexOf("ST");
        int qtIndex = parts.indexOf("qt");
        int lccIndex = parts.indexOf("lcc");  // 定位lcc标识

        // 解析ST组TO/TA（3个传感器）
        if (stIndex != -1 && parts.size() > stIndex + 8) {
            bool ok = false;
            // TO-1/2/3（原逻辑，假设是温度值除以100）
            double to1 = parts[stIndex+6].toDouble(&ok)/100.0;
            double to2 = parts[stIndex+7].toDouble(&ok)/100.0;
            double to3 = parts[stIndex+8].toDouble(&ok)/100.0;
            if (!ok) return;

            // TA-1/2/3（假设从qt后提取，根据实际格式调整）
            if (qtIndex != -1 && parts.size() > qtIndex + 3) {
                double ta1 = parts[qtIndex+1].toDouble(&ok)/100.0;
                double ta2 = parts[qtIndex+2].toDouble(&ok)/100.0;
                double ta3 = parts[qtIndex+3].toDouble(&ok)/100.0;
                if (!ok) return;

                groupTA << ta1 << ta2 << ta3;
            } else {
                return; // TA数据不完整
            }

            groupST << to1 << to2 << to3;
        } else {
            return; // ST数据不完整
        }

        // 解析LC-1/2/3（lcc后面的三个值）
        if (lccIndex != -1 && parts.size() > lccIndex + 3) {
            bool ok = false;
            // lcc后三个值，假设是温度值除以100
            double lc1 = parts[lccIndex+1].toDouble(&ok)/100.0;
            double lc2 = parts[lccIndex+2].toDouble(&ok)/100.0;
            double lc3 = parts[lccIndex+3].toDouble(&ok)/100.0;
            if (!ok) return;

            groupLC << lc1 << lc2 << lc3;
        } else {
            return; // LC数据不完整
        }
    }

    // 发送信号时携带LC数据（修改信号参数）
    if (!groupST.isEmpty()) {
        emit temperatureDataReceived(
            m_portName,
            QDateTime::currentDateTime(),
            groupST,    // TO组
            groupTA,    // TA组
            groupLC,    // 新增LC组
            isSingleHead
            );
    }
}

// 温度有效性检查辅助函数
bool SerialPortThread::isTemperatureValid(double temp)
{
    return temp >= -40.0 && temp <= 90.0;
}

void SerialPortThread::openPort()
{
    QMutexLocker locker(&m_mutex);

    if (m_serial->isOpen()) m_serial->close();

    m_serial->setPortName(m_portName);
    m_serial->setBaudRate(m_baudRate);
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serial->open(QIODevice::ReadWrite)) {
        emit portStatusChanged(false);
        return;
    }

    connect(m_serial, &QSerialPort::readyRead, this, [this]() {
        QByteArray data = m_serial->readAll();
        m_receiveBuffer.append(data);
        processBuffer();
    });

    emit portStatusChanged(true);
}

void SerialPortThread::setBaudRate(int baudRate)
{
    QMutexLocker locker(&m_mutex);
    m_baudRate = baudRate;
    if (m_serial && m_serial->isOpen()) {
        m_serial->setBaudRate(baudRate);
    }
}

void SerialPortThread::processBuffer()
{
    while(m_receiveBuffer.contains("\r\n")){
        int endIndex = m_receiveBuffer.indexOf("\r\n");
        QByteArray frame = m_receiveBuffer.left(endIndex);
        m_receiveBuffer = m_receiveBuffer.mid(endIndex + 2);

        emit dataReceived(frame);
        processData(QString::fromLatin1(frame));
    }
}

void SerialPortThread::setPortName(const QString &portName)
{
    QMutexLocker locker(&m_mutex);
    if (m_serial && m_serial->isOpen()) {
        qWarning() << "Cannot change port name while port is open";
        return;
    }
    m_portName = portName;
}
