#ifndef SERIALPORTTHREAD_H
#define SERIALPORTTHREAD_H

#include <QThread>
#include <QSerialPort>
#include <QMutex>
#include <QTimer>

class SerialPortThread : public QThread
{
    Q_OBJECT
public:
    explicit SerialPortThread(const QString &portName, int baudRate, QObject *parent = nullptr);
    ~SerialPortThread();

    void sendData(const QByteArray &data);
    void closePort();

    QString portName() const { return m_portName; }

    void openPort();

    void setBaudRate(int baudRate);

    void setPortName(const QString &portName);

    bool isTemperatureValid(double temp);

signals:
    void dataReceived(const QByteArray &data);
    void temperatureDataReceived(const QString& portName,
                                 const QDateTime& timestamp,
                                 const QVector<double>& groupST,  // TO组
                                 const QVector<double>& groupTA,  // TA组
                                 const QVector<double>& groupLC,  // LC组（新增）
                                 bool isSingleHead);
    void portStatusChanged(bool isOpen);

protected:
    void run() override;

private:
    mutable QMutex m_mutex; // 添加 mutable 修饰符

    void processData(const QString& data);
    void processBuffer();

    QTimer* m_bufferTimer;

    QString m_portName;
    int m_baudRate;
    QSerialPort* m_serial = nullptr;
    bool m_running = false;
    QByteArray m_receiveBuffer;
};

#endif // SERIALPORTTHREAD_H
